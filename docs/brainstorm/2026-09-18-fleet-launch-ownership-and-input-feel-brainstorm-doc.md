# Brainstorm: fleet launch ownership, login-screen, Mouser performance, scroll feel, observability

Date: 2026-09-18 · Repos: deskflow (`fleet/memory-program`), Mouser (`fleet/memory-program`) · Seats: macbookpro, hackintosh, tiny11

## Problem statement

1. The Deskflow menubar icon is missing on macbookpro; the user fears multiple instances at login or none.
2. Deskflow must be live on the login screen before sign-in on every seat, and launch ownership must be fixable over SSH on all seats.
3. Mouser's UI is very slow; scrolling across seats is janky.
4. Nobody noticed a dead GUI for 24 h, a dead core for 38 min, a 100%-CPU Mouser for 27 h, or lost pre-crash logs.

## Investigation (20 read-only agents, live + code + git history)

Full digest: `scratchpad/findings-digest.md` (session). Load-bearing facts, all confirmed with file:line or live evidence:

- **Menubar**: the GUI process is not running. It quit cleanly 09-17 09:36; its LaunchAgent is `KeepAlive=false`, so a clean quit sticks. Separately, `deskflow-gui.cpp:148-150` exits before creating the tray whenever the freshly re-signed bundle is not yet Accessibility-trusted (every redeploy).
- **Duplicate launchers by design**: `MainWindow.cpp:171-183` re-registers a Login Item via SMAppService on every launch. BTM holds 5 Deskflow + 3 Mouser records on macbookpro. hackintosh has no LaunchAgents at all (GUI-as-Login-Item spawns core). Mouser's startup sync (`core/startup.py:513-547`) runs bootout+bootstrap on every launch, spawning a second Mouser that raises the window and exits — the "self-sync bug"; launchd's copy is parked dead and an unmanaged one survives.
- **Ownership drift / zero states** (red team): GUI spawns its own core after a 3 s `launchctl print` timeout (`CoreProcess.cpp:294-297`) → launchd's core loops on exit 5 forever; GUI↔core re-attach tries 3 connects with zero backoff and never retries (`IpcClient.cpp:52-56`); failed deploy leaves agents booted out and survives reboot; Windows service has no failure actions and the watchdog gives up after 5 fast exits; daemon silently launches nothing if `daemon/configFile` is wiped by any GUI stop.
- **Login screen**: root `deskflow-vhid-bridge` in the LoginWindow session → Karabiner VirtualHIDDevice. Two plist generators diverge; both split peers on `|` without knowing wake syntax (would pass MAC addresses as hosts). No health check for the LoginWindow agent. **Security: the bridge logs every keystroke (the login password) to `/var/log/deskflow-vhid-bridge.log` mode 0644** (`deskflow-vhid-bridge.cpp:1468-1470,1518`).
- **Mouser**: root cause of the slow UI is a closed-sink spin — `core/hid_deskflow_backend.py` singleton `close()`d on reconnect and re-attached, `read()` returns `None` instantly, `hid_gesture.py:3915-3950` loops with no sleep → 100% CPU on one thread, Qt main thread starved for the GIL, macOS killed Mouser's event tap 359+ times, KVM gesture ingress silently dead. Triggered by every screens-wake `force_reconnect()`. The deployed binary is HEAD; no memory-program fix addresses it. Architecture: the macOS CGEventTap callback runs in Python on the Qt main thread at up to 1 kHz (Windows got a native C hook in 9bee5e2; macOS never did).
- **Scroll**: no native/hi-res passthrough exists or ever did — wire is int16 line deltas; server reads only integer `DeltaAxis1` (drops fractional motion, sends 0,0 messages); macOS client injects line-unit events with no continuous/phase; constants stack 3× on (1+scaling) then receiver acceleration. Dominant live cause: Deskflow's injected events are not marked, so on the receiving Mac every one passes through Mouser's GIL-starved active tap. Episodic freezes: auto-mode promotes on every local input burst; reconnect walks 7 of the seat's own addresses first (`PreConnectHosts.cpp:29-37` + `Coordinator::decide` never clears `fleetState.server`) → 7-9 s dead input per flip. Network ruled out (LAN direct, 3.4 ms).
- **Observability**: `LogOutputters.cpp:139-143` removes the log before renaming (rotation = deletion); every core line written twice on macOS; `[log] level=DEBUG` in production; prio log unbounded; server wedge-probe every 9 s produces most of the server's log volume; the fleet-soak sampler built during the memory program was never installed on any seat.

## Approaches considered (4 adversarial panels, 3 advocates each, judged)

**A. Launch ownership + login screen** — judged: launchd sole owner via *report-and-repair* converge (never kill by PID), GUI never spawns/kickstarts a core on macOS, GUI stops self-registering as Login Item under launchd, GUI survives no-AX with a "grant Accessibility" tray state + KeepAlive on crash + quit-intent sentinel, installer verifies before bootout/mv, machine lock dir created, bridge keystroke logging removed + 0600, single bridge plist generator (script) with correct peer fields and EUID==0 path, bridge console-user stand-down + SIGTERM drain + ExitTimeOut, Windows: delete watchdog give-up, GUI stop must not wipe coreMode, `sc failure` actions, create ProgramData. Rejected: converge that kills, hard KeepAlive with no quit path, Windows converge task, bridge sharing the core's lock, tray-in-core, converge-in-prio-daemon.

**B. Mouser performance** — judged: fix the sink lifecycle (don't close the process-global sink; defensive None-path guard; readonly early-returns; one-line closed-read log), in-process watchdog (CPU/tap-reenable/heartbeat → reconnect, then exit 3 for launchd), idempotent startup sync off the main thread (also fixes the window-pop), drop MouseMoved from the tap mask outside directional capture; structural: tap on a dedicated CFRunLoop thread + async slots via the existing device-write FIFO; window-open diet (Loader-gated chips, NSApplication delegate). Judge gated the native macOS tap on post-fix numbers; **user decision: build it now** (sequenced last, reviewer compares before/after). Corrections: the "3 s blocking slots" were a replay thread and by-design in ingress mode; QML diet is cosmetic.

**C. Scroll feel** — judged direction (advocate consensus; final judgment pending at time of writing): receiver first — mark Deskflow-injected events with Mouser's existing `kCGEventSourceUserData` marker so Mouser's tap early-returns; zero-delta guard; cache the scroll-scaling pref; fix stacked scaling; exclude self-addresses and clear `fleetState.server` on demotion (LAN-first reconnect); receive-side rescue dedupe + delete the duplicate Esc counter; raise/sustain the promotion burst threshold; quiet the wedge probe. Then protocol: `DMWX` (16.16 pixel deltas, continuous, phase, momentum, timestamp) as protocol minor 1.9 with a 1.8 fallback and the `Client.cpp:604-633` minor-clamp fix (without it a 1.9 client is rejected by the old server). Never send DMWX to a 1.8 peer. Screen/tap reuse across flips deferred.

**D. Observability** — judged direction (final judgment pending): fix rotation (rename before remove, keep generations, one open file), one log sink on macOS, INFO in production with category toggles, prio log print-on-change + newsyslog, bridge log 0600 and no keystrokes, wedge-probe noise, a 5-minute `health:` line, `fleet-health --watch` alert-only, fleet-soak actually installed with `pid:null` and CPU alerts.

## Decisions

- Build all four areas **in parallel** with adversarial builder/reviewer agent pairs in isolated worktrees (user decision).
- Include the native macOS Mouser tap in this program (user decision; judge recommended gating).
- Builders never install, deploy, `launchctl`, `sudo`, or touch live processes; deploy is a separate gated step after review, with hackintosh redeploy still blocked on the console Automation grant.
- Security fix (bridge keystroke logging) ships first and is deployed to both Macs before anything else.

## Open questions for the user

1. Rotate the hackintosh login password (it sat world-readable in `/var/log` for weeks)?
2. BTM cleanup: `sudo sfltool resetbtm` (wipes all login items, needs reboot) vs manual Login Items removal per Mac.
3. Mouser "Start at login" toggle: keep with idempotent sync, or launchd-only.
4. Mouser watchdog escalation: `os._exit(3)` for launchd respawn vs log-and-notify only.
5. Deskflow-attached seats: hide DPI/SmartShift/battery controls or show read-only.

## Acceptance criteria (command-checkable, from the judges)

- `deskflow-ctl assert-single` exits 0 on all three seats 5 min after reboot; `pgrep -x Deskflow | wc -l` == 1 and `pgrep -x deskflow-core | wc -l` == 1 on both Macs.
- `sfltool dumpbtm | grep -ci deskflow` == 0 and `| grep -ci mouser` == 0 on both Macs.
- `kill -9 $(pgrep -x Deskflow)` → tray back within 35 s; tray Quit stays quit for 5 min; re-login restores.
- `sudo grep -c 'key down id=' /var/log/deskflow-vhid-bridge.log` == 0; mode 600; `launchctl print loginwindow/org.deskflow.vhid-bridge` reports a pid at the next login window.
- `launchctl print gui/$UID/io.github.hughesyadaddy.mouser` pid == `pgrep -x Mouser`; no "show" raise lines after a Backend init.
- Mouser idle CPU < 3%; tap timeouts 0/h; cold-start < 1000 ms; footprint stable ±30 MB after hour 1.
- Dead input per role flip < 1 s; rescue lines never duplicated; mesh hello cadence 15 s.
- Core log rotates into `.1`; single sink; INFO by default; `health:` line every 5 min; prio log bounded.
- `sc.exe qfailure Deskflow` shows restart actions; `Test-Path C:\ProgramData\Deskflow` true.
