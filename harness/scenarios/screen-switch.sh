#!/usr/bin/env bash
# proc: deskflow-core
# automation: partial
# seat: hackintosh
# description: slam the cursor across a screen edge to a neighbour and back (socket output buffers, clipboard marshall per switch)
#
# The mesh has no "switch to screen" command; the server switches when the
# cursor crosses a configured link. We synthesize CGEvent mouse moves toward
# the edge of the link found in the local fleet state and verify with the
# `status` reply's fleet.cursor_host. Coming back relies on the neighbour's
# mirror link; if the cursor is stranded, the driver promotes this seat
# (which clears the cursor host). Sourced by harness/run-scenario.sh.

SWITCH_DIR=""
SWITCH_TO=""
SWITCH_SELF=""
SWITCH_SETTLE="${HARNESS_SWITCH_SETTLE:-1.5}"

scenario_setup() {
  local status link
  harness_require_quartz || return 1
  status="$(harness_mesh_status)" || { harness_log "local coordinator not reachable"; return 1; }
  SWITCH_SELF="$(harness_mesh_field "$status" name)"
  [ "$(harness_mesh_field "$status" role)" = server ] || { harness_log "this seat must be the server"; return 1; }
  link="$(HARNESS_JSON="$status" harness_python - <<'PY'
import json, os
st = json.loads(os.environ["HARNESS_JSON"])
me = st.get("name")
for l in st.get("fleet", {}).get("links", []):
    if l.get("from") == me and l.get("to") and l.get("dir"):
        print(l["dir"], l["to"]); break
PY
)"
  [ -n "$link" ] || { harness_log "no outgoing link from $SWITCH_SELF in fleet state"; return 1; }
  SWITCH_DIR="${link%% *}"
  SWITCH_TO="${link#* }"
  harness_log "link: $SWITCH_SELF --$SWITCH_DIR--> $SWITCH_TO"
}

_slam() {
  # _slam <left|right|up|down> -> post moves that pin the cursor to that edge
  harness_python - "$1" <<'PY'
import sys, time, Quartz
d = sys.argv[1]
bounds = Quartz.CGDisplayBounds(Quartz.CGMainDisplayID())
w, h = bounds.size.width, bounds.size.height
cx, cy = w / 2, h / 2
tx, ty = {"left": (0, cy), "right": (w - 1, cy), "up": (cx, 0), "down": (cx, h - 1)}[d]
loc = Quartz.CGEventGetLocation(Quartz.CGEventCreate(None))
steps = 30
for i in range(1, steps + 1):
    x = loc.x + (tx - loc.x) * i / steps
    y = loc.y + (ty - loc.y) * i / steps
    ev = Quartz.CGEventCreateMouseEvent(None, Quartz.kCGEventMouseMoved, (x, y), 0)
    Quartz.CGEventPost(Quartz.kCGHIDEventTap, ev)
    time.sleep(0.005)
# extra pushes past the edge so the server registers a crossing
dx, dy = {"left": (-40, 0), "right": (40, 0), "up": (0, -40), "down": (0, 40)}[d]
for _ in range(20):
    ev = Quartz.CGEventCreateMouseEvent(None, Quartz.kCGEventMouseMoved, (tx, ty), 0)
    Quartz.CGEventSetIntegerValueField(ev, Quartz.kCGMouseEventDeltaX, dx)
    Quartz.CGEventSetIntegerValueField(ev, Quartz.kCGMouseEventDeltaY, dy)
    Quartz.CGEventPost(Quartz.kCGHIDEventTap, ev)
    time.sleep(0.01)
PY
}

_opposite() {
  case "$1" in left) echo right ;; right) echo left ;; up) echo down ;; down) echo up ;; esac
}

scenario_iter() {
  local n="$1" host
  _slam "$SWITCH_DIR" || return 1
  harness_sleep "$SWITCH_SETTLE"
  host="$(harness_mesh_field "$(harness_mesh_status)" fleet.cursor_host)"
  [ "$host" = "$SWITCH_TO" ] || harness_log "iter $n: cursor_host='$host' (wanted $SWITCH_TO)"
  _slam "$(_opposite "$SWITCH_DIR")" || return 1
  harness_sleep "$SWITCH_SETTLE"
  host="$(harness_mesh_field "$(harness_mesh_status)" fleet.cursor_host)"
  if [ -n "$host" ] && [ "$host" != "$SWITCH_SELF" ]; then
    harness_todo "cursor stranded on $host; return path needs the neighbour's mirror link -- promoting self to recover"
    harness_mesh_promote 127.0.0.1 || true
    harness_sleep "$SWITCH_SETTLE"
  fi
  if [ $((n % 50)) = 0 ]; then harness_log "iter $n done"; fi
  return 0
}

scenario_teardown() {
  harness_mesh_promote 127.0.0.1 || true
}
