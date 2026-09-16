#!/usr/bin/env bash
# proc: deskflow-core
# automation: partial
# seat: hackintosh
# description: hold the server role while at least one configured peer is unreachable (Coordinator blocking per-peer connects every 3 s)
#
# Making a peer unreachable from userland needs root (pfctl) or the peer's
# cooperation, so this driver only records which configured peers are down
# and lets the coordinator's 3 s reconciler run for the iteration. Run it
# while a peer (typically tiny11) is asleep. Sourced by harness/run-scenario.sh.

PEER_TICK="${HARNESS_PEER_TICK:-3}"

_peer_probe() {
  # prints "<name> up|down" per configured peer other than self
  local status
  status="$(harness_mesh_status)" || return 1
  HARNESS_JSON="$status" harness_python - <<'PY'
import json, os, socket
st = json.loads(os.environ["HARNESS_JSON"])
me = st.get("name")
for p in st.get("fleet", {}).get("peers", []):
    if p.get("name") == me:
        continue
    up = False
    for addr in (p.get("lan"), p.get("ip")):
        if not addr:
            continue
        try:
            socket.create_connection((addr, 24851), 0.7).close()
            up = True
            break
        except Exception:  # noqa: BLE001
            pass
    print(p.get("name"), "up" if up else "down")
PY
}

scenario_setup() {
  local probe
  probe="$(_peer_probe)" || { harness_log "local coordinator not reachable"; return 1; }
  harness_log "peers: $(echo "$probe" | tr '\n' ' ')"
  if ! echo "$probe" | grep -q ' down$'; then
    harness_todo "no peer is unreachable; put one to sleep (or block :24851 with pfctl as root) for a real run"
  fi
  harness_mesh_promote 127.0.0.1 || true
}

scenario_iter() {
  local n="$1"
  harness_sleep "$PEER_TICK"
  if [ $((n % 20)) = 0 ]; then
    harness_log "iter $n peers: $(_peer_probe | tr '\n' ' ')"
  fi
  return 0
}

scenario_teardown() { :; }
