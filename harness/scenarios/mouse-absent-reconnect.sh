#!/usr/bin/env bash
# proc: deskflow-core
# automation: partial
# seat: hackintosh
# description: with no physical mouse on this seat, hand the server role to a peer over the mesh, reconnect as client, then take it back
#
# The role hand-off is real (mesh `promote`, same as `kvmctl primary`). The
# "mouse absent" precondition -- receiver unplugged / mouse asleep -- has no
# userland command, so the driver records it as a TODO and expects the
# operator to run it with the mouse detached. Sourced by harness/run-scenario.sh.

MAR_PEER_HOST=""
MAR_PEER_NAME=""
MAR_SETTLE="${HARNESS_MAR_SETTLE:-6}"

scenario_setup() {
  local peers
  harness_todo "detach the physical mouse (or power off the receiver) before this run; cannot be automated"
  peers="$(harness_mesh_peer_hosts)" || { harness_log "local coordinator not reachable"; return 1; }
  [ -n "$peers" ] || { harness_log "no reachable peer"; return 1; }
  MAR_PEER_NAME="$(echo "$peers" | head -n1 | cut -d' ' -f1)"
  MAR_PEER_HOST="$(echo "$peers" | head -n1 | cut -d' ' -f2)"
  harness_log "hand-off partner: $MAR_PEER_NAME ($MAR_PEER_HOST)"
}

scenario_iter() {
  local n="$1" status
  harness_mesh_promote "$MAR_PEER_HOST" || return 1
  harness_sleep "$MAR_SETTLE"
  status="$(harness_mesh_status)" || return 1
  harness_log "iter $n: role=$(harness_mesh_field "$status" role) server_ip=$(harness_mesh_field "$status" server_ip)"
  harness_mesh_promote 127.0.0.1 || return 1
  harness_sleep "$MAR_SETTLE"
  return 0
}

scenario_teardown() {
  harness_mesh_promote 127.0.0.1 || true
}
