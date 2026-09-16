#!/usr/bin/env bash
# proc: deskflow-core
# automation: full
# seat: hackintosh
# description: flip the server role to a peer and back over the coordination mesh (AutoModeRunner epoch rebuild per flip)
#
# Uses the same `promote` message as `kvmctl primary <name>`
# (docs/coordination/behavior-spec.md §2). Sourced by harness/run-scenario.sh.

EPOCH_PEER_HOST=""
EPOCH_PEER_NAME=""
EPOCH_SETTLE="${HARNESS_EPOCH_SETTLE:-4}"

scenario_setup() {
  local status peers
  status="$(harness_mesh_status)" || { harness_log "local coordinator not reachable on :24851"; return 1; }
  harness_log "self=$(harness_mesh_field "$status" name) role=$(harness_mesh_field "$status" role)"
  peers="$(harness_mesh_peer_hosts)" || true
  [ -n "$peers" ] || { harness_log "no reachable peer to flip to"; return 1; }
  EPOCH_PEER_NAME="$(echo "$peers" | head -n1 | cut -d' ' -f1)"
  EPOCH_PEER_HOST="$(echo "$peers" | head -n1 | cut -d' ' -f2)"
  harness_log "flip partner: $EPOCH_PEER_NAME ($EPOCH_PEER_HOST)"
}

scenario_iter() {
  local n="$1" role
  harness_mesh_promote "$EPOCH_PEER_HOST" || return 1
  harness_sleep "$EPOCH_SETTLE"
  role="$(harness_mesh_field "$(harness_mesh_status)" role)"
  [ "$role" = client ] || harness_log "iter $n: expected client after peer promote, got '$role'"
  harness_mesh_promote 127.0.0.1 || return 1
  harness_sleep "$EPOCH_SETTLE"
  role="$(harness_mesh_field "$(harness_mesh_status)" role)"
  [ "$role" = server ] || harness_log "iter $n: expected server after self promote, got '$role'"
  if [ $((n % 50)) = 0 ]; then harness_log "iter $n done"; fi
  return 0
}

scenario_teardown() {
  # leave this seat as server, which is where hackintosh normally sits
  harness_mesh_promote 127.0.0.1 || true
}
