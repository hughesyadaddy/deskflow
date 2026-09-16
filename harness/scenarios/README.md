# Scenario drivers

One file per matrix row. `harness/run-scenario.sh <row> --seat S --iters N`
sources the driver, wraps it in `tools/fleet-soak sample --once` before /
after / every 60 s, and prints `run-id:`, `deltaMB:` and `deltaPorts:`
(the tokens `harness/check-pr.sh` looks for in PR bodies).

## Header contract

Every driver starts with four comment lines that `run-scenario.sh` parses:

```
# proc: mouser|deskflow-core|deskflow     process the sampler labels
# automation: full|partial|manual         see below
# seat: hackintosh                         seat the row is defined for
# description: one line
```

and defines three functions, run under `set -euo pipefail`:

| function | called |
|---|---|
| `scenario_setup` | once, before iteration 1; non-zero aborts the run (exit 1) |
| `scenario_iter <n>` | `--iters` times; first non-zero stops the loop (exit 1) |
| `scenario_teardown` | always, including on abort / SIGINT |

`automation:` meanings:

- `full` -- runs unattended.
- `partial` -- runs unattended but one step has no userland command; the
  driver calls `harness_todo "<step>"` which logs `TODO(automation):` and
  continues. Grep the run log for it before trusting the delta.
- `manual` -- operator only. `run-scenario.sh` exits 4 unless
  `FLEET_OPERATOR=1`; with `--smoke` it writes
  `{"header":true,"smoke":"skipped-manual","scenario_source":"manual"}` to the
  out file and exits 0 so the smoke matrix stays green.

## Helpers available to drivers

Defined in `run-scenario.sh` (drivers are sourced, not executed):

| helper | what |
|---|---|
| `harness_log msg` | timestamped line to `harness/runs/logs/<proc>/<row>-<ts>.log` and stdout |
| `harness_todo step` | records an automation gap (partial rows) |
| `harness_sleep s` | `sleep s`, or 0.2 s under `--smoke` |
| `harness_python …` | `/usr/bin/python3` / `python3` / `$HARNESS_PYTHON` |
| `harness_require_quartz` | fails with a hint if pyobjc `Quartz` is missing |
| `harness_mesh_status [host]` | `{"t":"status"}` on :24851, reply on stdout |
| `harness_mesh_promote host` | `{"t":"promote"}` -- same as `kvmctl primary` |
| `harness_mesh_peer_hosts` | `name addr` per reachable configured peer |
| `harness_mesh_field json path` | dotted lookup, e.g. `fleet.cursor_host` |

Mesh messages carry `coordination/token` read from `Deskflow.conf`
(`~/Library/Deskflow/Deskflow.conf` on macOS) or `HARNESS_MESH_TOKEN`.

Exported to drivers: `HARNESS_SMOKE`, `HARNESS_ROW`, `HARNESS_PROC`,
`HARNESS_SEAT`, `HARNESS_RUN_ID`, `HARNESS_LOG_FILE`, `HARNESS_OUT`,
`HARNESS_ITERS`.

## Rows

| row | proc | automation | mechanism |
|---|---|---|---|
| `epoch-flip` | deskflow-core | full | mesh `promote` peer, then self |
| `screen-switch` | deskflow-core | partial | CGEvent edge slam along the fleet link; verifies `fleet.cursor_host`; return path best-effort |
| `peer-unreachable` | deskflow-core | partial | records which peers are down and lets the 3 s reconciler run; blocking a peer needs root |
| `mouse-absent-reconnect` | deskflow-core | partial | mesh hand-off and reclaim; detaching the mouse is a TODO |
| `clipboard-10mb` | deskflow-core | full | 10 MB PNG via `osascript` `set the clipboard`, alternated with `pbcopy` text |
| `window-toggle` | mouser | full | AppleScript `reopen`/`activate` + System Events hide (Mouser has no URL scheme) |
| `add-profile` | mouser | full | System Events clicks the `+` button, Escape dismisses (`HARNESS_ADD_PROFILE_BUTTON` overrides the AX name) |
| `scroll-im-granted` | mouser | full | `CGEventCreateScrollWheelEvent` bursts; records `CGPreflightListenEventAccess` and tags the run |
| `scroll-im-denied` | mouser | full | same generator, tag `im_expected: denied`; never touches TCC |
| `bt-sleep-wake` | mouser | manual | `blueutil -p 0` / `-p 1` |
| `lock-unlock` | deskflow-core | manual | `CGSession -suspend` + `caffeinate -u -t 1` |
| `login-bridge` | deskflow-core | manual | System Events log out, wait for the vhid bridge and the operator's re-login |

## Gates in `run-scenario.sh`

- On hackintosh (seat and `hostname -s` both `hackintosh`) the run holds
  `harness/.hackintosh.lock` (`flock` when present, otherwise an atomic
  `mkdir harness/.hackintosh.lock.d`) so swarm auditors serialize.
- Same condition: exit 3 unless `tools/fleet-soak report --in
  harness/baselines/<latest>-hackintosh-<proc>.json --min-hours 72 --validate`
  exits 0. Skip with `--smoke` or `FLEET_NO_BASELINE_GATE=1`.
- `--print-proc` prints the header `proc` and exits without any of the above.

Exit codes: 0 ok, 1 driver failure, 2 usage/header, 3 baseline gate, 4
manual row refused.

## Tests

`tools/tests/test_run_scenario.bats` stubs `tools/fleet-soak` with
`FLEET_SOAK_BIN` and the hostname with `FLEET_HOSTNAME`; it never runs a real
driver. Run with `tools/tests/bats/bin/bats tools/tests/test_run_scenario.bats`.
