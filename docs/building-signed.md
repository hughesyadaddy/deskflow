# Building signed fleet binaries

How to produce installable, *identity-signed* builds of this fork for every
machine in a personal KVM fleet. Signing with a real certificate (not ad-hoc)
matters on macOS because TCC anchors trust to the signing identity: a stable
identity means Accessibility / Input Monitoring grants survive rebuilds and
reinstalls instead of re-prompting every time the binary changes.

## Certificates: what you need (and what you don't)

| Platform | For your own machines | For public distribution |
|---|---|---|
| macOS | A free **Apple Development** certificate (Xcode → Settings → Accounts → Manage Certificates → "+" → Apple Development). No paid program needed. | **Developer ID Application** cert + notarization (paid Apple Developer Program). |
| Windows | Nothing — unsigned binaries run fine locally; SmartScreen only screens downloaded files. | An Authenticode certificate (separate purchase; Apple certs are useless here). |
| Linux | Nothing. | Distro-dependent (repo signing), out of scope. |

List available macOS identities:

```sh
security find-identity -v -p codesigning
```

## macOS (per-arch: build natively on each Mac)

The repo's `APPLE_CODESIGN_DEV` option signs the bundle with hardened runtime
and the dev entitlements after every build, and (this fork) the deployed DMG
app as well:

```sh
export IDENTITY="Apple Development: Your Name (TEAMID)"

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$(brew --prefix qt)" \
  -DAPPLE_CODESIGN_DEV="$IDENTITY"
ninja -C build

# Verify
codesign --verify --deep --strict build/bin/Deskflow.app && echo OK

# Self-contained installable DMG (bundles Qt via macdeployqt, signs with
# the same identity):
(cd build && cpack -G DragNDrop)   # -> build/Deskflow-<ver>-macos-<arch>.dmg
```

Architecture note: a Mac builds for its own architecture. Build the Intel
DMG on the Intel Mac and the Apple Silicon DMG on the arm64 Mac (or pass
`-DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"` for a universal build if every
dependency — brew Qt is per-arch — is available universal, which Homebrew's
usually is not; native-per-machine is the simple, reliable path).

### Using the same identity on a second Mac

The certificate's private key lives in the keychain of the Mac that created
it. To sign on another Mac with the same identity:

1. On the source Mac: Keychain Access → My Certificates → right-click the
   "Apple Development: …" item → Export as `.p12` (set a passphrase).
2. Copy to the target Mac, double-click to import into the login keychain.
3. The same `-DAPPLE_CODESIGN_DEV` value now works there.

(Alternatively sign in to Xcode on the second Mac with the same Apple ID and
mint a second Apple Development cert — different serial, same team, equally
fine for TCC.)

### Known x86_64 quirk (fixed in this fork)

The arm64 linker ad-hoc signs everything it produces; the x86_64 linker does
not. Upstream's codesign step signed only the bundle and failed on Intel with
"code object is not signed at all" for nested executables.
`cmake/MacCodesign.cmake` here signs the nested binaries first.

## Windows

Prereqs on the Windows machine: Visual Studio Build Tools (C++ workload),
CMake 3.24+, Qt 6 (the official installer's MSVC kit), and optionally
[WiX v4+](https://wixtoolset.org) for an `.msi` (a `.7z` is produced either
way).

```bat
cmake -B build -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_PREFIX_PATH=C:\Qt\6.7.0\msvc2019_64
cmake --build build --config Release
cd build && cpack            :: -> .7z always, .msi when WiX is installed
```

Signing: the fleet signs every `.exe`/`.dll` under the install root with the
certificate whose thumbprint is in `DESKFLOW_SIGN_THUMBPRINT` (`.env` or
`scripts/fleet.env`). `scripts/build-windows.ps1 -Install` calls
`scripts/sign-windows.ps1`, which runs:

```powershell
signtool sign /sha1 $env:DESKFLOW_SIGN_THUMBPRINT /fd SHA256 <every .exe/.dll>
```

The thumbprint is a public identifier of a cert already in the user's
certificate store, not a secret. `tools/fleet-health --check authenticode`
verifies `Get-AuthenticodeSignature` reports Valid with that thumbprint on
every binary. The installer no longer imports a self-signed cert into the
Root store.

## Linux

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build && (cd build && cpack)   # rpm/deb per deploy/linux/deploy.cmake
```

## Mouser (companion app)

Mouser's macOS build script takes the same identity via env:

```sh
cd ../Mouser
MOUSER_SIGN_IDENTITY="$IDENTITY" ./build_macos_app.sh   # -> dist/Mouser.app
```

Unset (or `-`), `scripts/build_and_install.py` **refuses to build**
(`EXIT_NO_SIGN_IDENTITY`); ad-hoc Mouser bundles are never installed. The
identity is read from `Mouser/.env.local` (`MOUSER_SIGN_IDENTITY=<sha1>`),
and after `codesign --verify` the installer walks every Mach-O in
`dist/Mouser.app` (MacOS, Frameworks, PlugIns, Resources) requiring
`TeamIdentifier=J5KPG8ZR5C`, no ad-hoc, and hardened runtime on
`Contents/MacOS/*` before touching `/Applications`.

## Fleet deploy (all seats at once)

The fleet is three seats — `hackintosh` and `macbookpro` (macOS) and `tiny11`
(Windows) — and the controller is **symmetric**: any seat can drive the whole
fleet. Run `scripts/fleet-deploy.sh` on a Mac or `scripts/fleet-deploy.ps1` on
Windows; both accept the same CLI. There is no designated "deploy box".

### Per-seat files (untracked, never committed)

| File | Where | Holds |
|---|---|---|
| `.env` | both Macs and tiny11 | build/sign settings for *that* box. Copy from `env.example` (canonical; `.env.example` is gone). macOS needs `DESKFLOW_CODESIGN_ID` = the cert **hash** from `security find-identity -v -p codesigning`. Windows needs `DESKFLOW_QT_PATH`, `OPENSSL_ROOT_DIR`, `DESKFLOW_INSTALL_DIR`, `DESKFLOW_SIGN_THUMBPRINT`. |
| `scripts/fleet.env` | **all** seats | host list, SSH targets, what to deploy. Copy from `scripts/fleet.env.example`. |

Two rules for `scripts/fleet.env`:

- Each seat's own id is `LOCAL_ID`, derived from the hostname (`hostname -s`
  on macOS, `$env:COMPUTERNAME` on Windows, lowercased) — never from the file.
  The `FLEET_SSH_<LOCAL_ID>` entry on each seat **must be `local`**; the other
  hosts get their SSH target. So the file differs by exactly one line per seat.
- **No passwords in `scripts/fleet.env`.** `FLEET_KEYCHAIN_PASSWORD_*`
  is gone from the shared file and `tools/fleet-doctor` fails if any
  `*PASSWORD*` key exists in `scripts/fleet.env*`. Passwords live only in the
  git-ignored `.env` (mode 600, never tracked -- fleet-doctor checks both);
  see "Signing over SSH" below.

### Signing over SSH — the unattended flow (macOS)

`codesign` needs the private key's ACL to allow it *and* the login keychain to
be unlocked, and the seat's root steps (LoginWindow bridge plist, the
`deskflow-prio` LaunchDaemon, root-owned retired files, the bridge log) need
`sudo`. An SSH session gets none of that by default. Since 2026-09-25 one
password per Mac, kept in **one file on the controller**, covers all of it.

**One-time setup, on the seat you run the deploy from** (any seat; the file
is the repo's git-ignored `.env`, mode 600, never tracked):

```sh
printf 'FLEET_SEAT_PASSWORD_hackintosh=…\n' >> ~/Desktop/deskflow/.env && chmod 600 ~/Desktop/deskflow/.env
printf 'FLEET_SEAT_PASSWORD_macbookpro=…\n' >> ~/Desktop/deskflow/.env   # one line per Mac, ids as in FLEET_HOSTS (lowercase)
```

Windows needs nothing: `DESKFLOW_SIGN_THUMBPRINT` names a cert already in the
user's store, `signtool` and the service manager run in the High-Integrity
SSH session, and the controller transports nothing to `tiny11`
(`tools/fleet-doctor` fails on a password in a Windows `.env`).

What happens on `scripts/fleet-deploy.sh`:

1. **Controller.** `.env` is read by a plain `KEY=VALUE` parser (nothing is
   sourced or exported). It refuses to run if any `*PASSWORD*` value is set
   and the file is not mode 600, and refuses to *start* a deploy of a Mac
   that has no `FLEET_SEAT_PASSWORD_<id>` line -- such a run would stall on
   a prompt -- naming the key to add (`--allow-prompts` overrides for a seat
   that carries its own `.env` password). A remote Mac gets the value on the
   **SSH session's stdin only**: the remote command starts with
   `IFS= read -rs FLEET_SEAT_PASSWORD` and passes it to
   `scripts/fleet-deploy-macos.sh` as `DESKFLOW_KEYCHAIN_PASSWORD` and
   `DESKFLOW_SUDO_PASSWORD` on that one invocation (git never sees it). The
   local seat gets the same two exports in-process. It is never an ssh
   argument, never in `fleet.env`, never in a log line, and `bash -x` cannot
   echo it (xtrace is off around every expansion). `--pull-only` transports
   nothing.
2. **Seat.** `scripts/fleet-deploy-macos.sh` resolves each password as: the
   seat's own `.env` key (`DESKFLOW_KEYCHAIN_PASSWORD` / `DESKFLOW_SUDO_PASSWORD`,
   non-empty, wins) → the transported value → the seat's own
   `FLEET_SEAT_PASSWORD_<this seat>` line; each defaults to the other. Both
   are captured into plain shell variables and **unset before any child
   runs**, so cmake, python3, codesign and the installers never inherit
   them. Then `prepare_keychain_for_ssh` runs `security unlock-keychain` +
   `security set-key-partition-list -S apple-tool:,apple:,codesign:` and
   proves it with a `codesign` probe (Mouser's `scripts/build_and_install.py`
   signs in the same session), and `init_root_mode` verifies the sudo
   password once (`sudo -S -p '' -k true`). A cmake cache that disagrees
   with the identity / strict-signing requirements is reconfigured
   automatically (`cmake cache mismatch → reconfiguring`; from scratch if the
   in-place configure fails).
3. **Root steps, run on the seat through `sudo -S` with the password on
   sudo's stdin (never argv):** install a stale LoginWindow bridge plist
   (`install-login-bridge-macos.sh`), `deskflow-ctl prio --sudo-stdin` (the
   `deskflow-prio` LaunchDaemon + `/private/var/db/deskflow`),
   `deskflow-ctl retire --sudo-stdin` (root-owned leftovers such as
   `/usr/local/bin/deskflow-prio-apply.sh` and
   `/Library/LaunchAgents/com.symless.synergy-agent.plist`), and `chmod 600` +
   `sed -i '' '/key down id=/d'` on `/var/log/deskflow-vhid-bridge.log`. A
   rejected password is reported **once** (`sudo password rejected for
   <seat>; check FLEET_SEAT_PASSWORD_<id>`) and every root step falls back
   to being printed; the deploy still builds and installs.
4. **What is left for a human -- exactly two things, neither fails the run:**
   removing a BTM Login Item (System Settings → General → Login Items &
   Extensions; listed under *System Settings only*), and a log-out or reboot
   after a new bridge plist was installed (*takes effect at next login
   window*). Real blockers (a second core, a process outside the bundle, a
   root-owned file nobody could remove, an audit that could not run) still
   exit non-zero under *BLOCKERS*.

Without any password the old behaviour remains: signing is relayed by
`tools/fleet-gui-exec.py` into the logged-in console session (Terminal +
System Events Automation granted, console user logged in; Mouser through
`scripts/build_macos_gui_session.py`) and the root steps are printed at the
end as *root steps still pending*. The controller only runs that mode with
`--allow-prompts`.

### How a macOS build actually runs: GUI-session routing

macOS build+sign is executed inside the seat's logged-in GUI session, not in
the SSH session. The controller invokes `tools/fleet-gui-exec.py` on the
target, which checks that the console user is the target user, sends the
build command into Terminal via `osascript`, tails the log for a sentinel and
enforces an 1800 s timeout. The login keychain is already unlocked there, so
`codesign` sees `DESKFLOW_CODESIGN_ID` without any password handling. The
first run on a seat triggers a Terminal Automation prompt that a human must
approve (see below).

Strict signing is the CMake option `FLEET_STRICT_SIGNING`: with it ON, an
empty or `-` (ad-hoc) `APPLE_CODESIGN_DEV` is a configure-time
`FATAL_ERROR`, and every `codesign` `execute_process` checks its result. The
identity comes only from `.env`; the deploy script no longer falls back to
"any Apple Development cert".

### Commands

```sh
scripts/fleet-deploy.sh                      # pull FLEET_BRANCH, build, sign, install, clients → server
scripts/fleet-deploy.sh --dry-run --json     # print the plan per host, touch nothing
scripts/fleet-deploy.sh --self-test [--ref R] [--json]
                                             # rebuild+install on every host, then fleet-health --check all
scripts/fleet-deploy.sh --rollback [--host H] [--app deskflow|mouser]
                                             # reinstall the last-good build recorded in tools/state/last-good.json

tools/fleet-doctor [--host all] [--check noise]
tools/fleet-health --json [--check sign|no-adhoc|identifiers|tcc|authenticode|session|mesh|all] [--host H|all]
```

- **`tools/fleet-doctor`** is the preflight. It exits non-zero on: an
  unreachable host, a dirty tree or branch mismatch in either repo,
  `APPLE_CODESIGN_DEV=-` or `FLEET_STRICT_SIGNING:BOOL=OFF` in any deployed
  `CMakeCache.txt`, console user != target user, a missing or locked Windows
  interactive session, a missing cert, a Python version that is not the seat's
  pinned one, any `*PASSWORD*` key in `fleet.env*`, a `.env` holding a
  password at a mode other than 600 or tracked by git, or a password in a
  Windows `.env`. `--check noise`
  (hackintosh) also fails if `com.cursor.worker.*` is loaded, vitest workers
  exist, or Spotlight is indexing the app bundle; `--fix` boots the Cursor
  workers out.
- **`tools/fleet-health`** is the post-deploy check. macOS: `codesign -dvv`
  per Mach-O (Authority, TeamIdentifier, Identifier allowlist, no ad-hoc),
  TCC entries decoded as `certificate leaf[subject.CN]` (never `cdhash`),
  `deskflow-core --check-permissions` (AX + IOHID both granted), and a live
  GUI session in `launchctl print gui/$UID`. Windows: Authenticode Valid with
  the fleet thumbprint on every binary, `sc query Deskflow` RUNNING, GUI
  `SessionId != 0`. All platforms: mesh round-trip across every host pair.
- **`--self-test`** is what the weekly `tools/launchd/com.fleet.selftest.plist`
  runs: a full rebuild+install on every host followed by `fleet-health
  --check all`. Use `--ref R` to test a specific commit.
- **`--rollback`** reinstalls from `tools/state/last-good.json` (gitignored),
  which the deploy writes only after `fleet-health` passes. Scope it with
  `--host` and/or `--app`.
- The deploy takes a per-repo lock (`flock` on macOS, `New-Item` on Windows),
  installs clients before the server, prints a per-host result table, and
  checks every `ssh` exit code and `$LASTEXITCODE` explicitly — a failed seat
  is a failed run, never a silent success.

### Human steps (agents verify these, never perform them)

1. Rotate the two login-keychain passwords that were once written into
   `scripts/fleet.env` on hackintosh, delete `scripts/fleet.env.bak-20260814`,
   and remove every `FLEET_KEYCHAIN_PASSWORD_*` line. Agents only check
   `! grep -rq KEYCHAIN_PASSWORD scripts/fleet.env*`.
2. Put `FLEET_SEAT_PASSWORD_hackintosh` and `FLEET_SEAT_PASSWORD_macbookpro`
   into the controller's `.env` (`chmod 600`; see "Signing over SSH"). Only
   then is the Terminal Automation prompt for `tools/fleet-gui-exec.py`
   irrelevant; without the lines approve it once on hackintosh.
3. Quit Cursor on hackintosh before harness runs (or run
   `tools/fleet-doctor --host hackintosh --check noise --fix`).
4. Put `DESKFLOW_CODESIGN_ID` (hash from `security find-identity -v -p
   codesigning`) in `.env` on both Macs. The tiny11 user is `alexh`.
5. Wait out the 72 h baseline and the 7-day soak. Run the three
   `# automation: manual` scenario rows (`bt-sleep-wake`, `lock-unlock`,
   `login-bridge`) yourself with
   `FLEET_OPERATOR=1 harness/run-scenario.sh <row> --seat hackintosh --iters 1000`
   on hackintosh. `FLEET_OPERATOR` stays `0` everywhere else; agents never
   set it.
