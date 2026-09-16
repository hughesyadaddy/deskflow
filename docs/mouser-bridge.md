# Mouser bridge (fork extension)

Forwards Logitech HID++ gestures and buttons from the machine the mouse is
physically attached to (the Deskflow **server**) to whichever machine has
KVM focus, where the local [Mouser](https://github.com/hughesyadaddy/Mouser)
instance executes them natively. No HID emulation: device identity and
events travel as data and feed Mouser's existing pipeline.

## Flow

```
machine A (server, mouse attached)            machine B (client, has focus)
┌─────────────┐  loopback   ┌──────────┐ DMSR ┌──────────┐ loopback ┌─────────────┐
│  Mouser A    │◄───────────┤ Deskflow │─────►│ Deskflow ├─────────►│  Mouser B    │
│ (listener)   ├───────────►│  core    │      │  core    │◄─────────┤ (listener)   │
└─────────────┘ events/focus└──────────┘      └──────────┘  focus   └─────────────┘
```

- **Mouser owns the listener** (`127.0.0.1:19795`) and writes a token file
  (`~/Library/Application Support/Mouser/bridge.token` on macOS,
  `%LOCALAPPDATA%\Mouser\bridge.token` on Windows, `~/.local/share/Mouser/bridge.token`
  on Linux; mode 0600). Deskflow never listens in this mode.
- **`deskflow::MouserLink`** (`src/lib/deskflow/MouserLink.*`) is the ONE
  role-agnostic connector. It belongs to the process (`MouserLink::shared()`,
  driven by `AutoModeRunner`), never to a `Server` or `ServerProxy`, so a role
  flip or a screen switch never tears the Mouser session down. It reconnects
  with 1 s → 30 s backoff and logs one INFO line when Mouser is absent.
- **DMSR protocol message** (`"DMSR%s"`, server → client): one Mouser JSON
  line relayed verbatim. Only ever sent when a device is attached; stock
  clients never see it.

## Wire contract ("proto 2", JSON lines, `\n`-terminated)

| Direction | Message | Notes |
|---|---|---|
| core → Mouser | `{"t":"hello","proto":2,"app":"deskflow-core","ver":"<version>","pid":N,"role":"server\|client\|none","caps":["focus","hidr","decode"],"token":"<bridge.token>"}` | first line on every connection |
| Mouser → core | `{"ok":true,"proto":2,"caps":[...]}` | accept |
| Mouser → core | `{"ok":false,"reason":"proto"}` | core sleeps 30 s between retries, logs once |
| Mouser → core | `{"ok":false,"reason":"auth"}` | core re-reads the token file and retries after 5 s |
| core → Mouser | `{"t":"attach","session_id":"<uuid>"}` | right after hello; the id is fixed for the process lifetime, so a reconnect repeats it and Mouser treats a repeat as a no-op |
| core → Mouser | `{"t":"role","role":"server\|client\|none"}` | at epoch start (and `none` at epoch end); replayed after every hello |
| core → Mouser | `{"t":"focus","screen":"<name>","here":bool}` | on every screen switch, **instead of** connect/disconnect. `here` = focus is on the machine receiving the line. Replayed after every hello |
| both | `{"t":"ping"}` / `{"t":"pong"}` | core pings every 2 s of silence; 3 unanswered pings drop the session |
| both | `{"t":"bye","reason":"restart\|upgrade\|shutdown"}` | core sends it from its destructor; a bye from Mouser makes core reconnect immediately (no backoff) |
| Mouser A → core | `{"type":"connect",...}`, `{"type":"event",...}`, `{"type":"report",...}`, `{"type":"decode",...}`, `{"type":"disconnect"}` | unchanged v1 shapes; `connect` is cached and relayed once per client, `event`/`report` follow focus |
| core → Mouser B | the relayed lines above plus the `focus` notices, and binary `DFHR` HID frames | unchanged shapes so the current decoder keeps working during migration |

The server-side `VirtualHostTracker` keeps a per-client "attached" set: a
client gets the cached `connect` line the first time it gains focus (or again
after the device identity changes) and from then on only `focus` notices.
`disconnect` is reserved for the device really going away (Mouser said so, or
its session vanished), in which case every attached client is told.

## Compatibility shim (un-upgraded Mouser)

When the token file is absent the link falls back to the old behaviour and
logs `mouser link: legacy mode`:

- server role: a loopback listener on `server/mouserBridgePort` (19796) that
  expects the v1 hello (`{"type":"hello","token":...}`), answers
  `{"ok":true,"server":"deskflow-bridge","version":1}` and sends focus as
  `{"type":"focus","screen":...,"local":bool}`;
- client role: a connector to `client/mouserPort` with the v1 hello and
  `client/mouserToken`; relayed `focus` notices are translated back into the
  `connect`/`disconnect` an old Mouser expects.

Both need the corresponding `*Enabled` setting and token. The token file is
re-checked on every reconnect, so upgrading Mouser switches the link to proto
2 without restarting Deskflow. `DESKFLOW_MOUSER_TOKEN_FILE` overrides the
token-file path (tests, odd setups).

## Settings

Lego mode needs nothing in `deskflow.conf`; Mouser's token file is the on/off
switch. The legacy fallback still reads:

```
[server]
mouserBridgeEnabled=true
mouserBridgePort=19796
mouserBridgeToken=<shared secret with Mouser A's settings.remote_forward.token>

[client]
mouserEnabled=true
mouserPort=19795
mouserToken=<shared secret with Mouser B's settings.remote_device.token>
```

## Security posture

- Every local hop is loopback-only; the only cross-machine hop rides
  Deskflow's existing (optionally TLS) connection.
- Proto 2 carries Mouser's own token (file mode 0600) in the hello; the
  legacy hops still require their configured tokens and refuse to start
  without one.
- The relay carries an allowlisted event vocabulary (Mouser side enforces
  it); there is no generic input-injection path.

## Tests

`src/unittests/deskflow/MouserLinkTests.cpp` (QTest, fake loopback listener):
hello/attach/role/focus, proto mismatch sleeps and logs once, auth rejection
re-reads the token, bye reconnects without backoff, reconnect reuses the
session id, focus switches never send disconnect, 50 Server-style
register/unregister cycles produce a single attach, unanswered pings drop the
session, and both legacy shims. `VirtualHostTrackerTests` cover the
attached-set semantics.
