/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/FleetStateMerge.h"

#include "common/FleetCursor.h"

namespace deskflow::coordination {

bool operator==(const FleetState &a, const FleetState &b)
{
  return a.server == b.server && a.cursorHost == b.cursorHost && a.cursorScreen == b.cursorScreen && a.seq == b.seq &&
         a.peers == b.peers && a.links == b.links && a.screens == b.screens;
}

bool operator!=(const FleetState &a, const FleetState &b)
{
  return !(a == b);
}

FleetMergeResult applyServerFragment(FleetState &state, const FleetFragment &fragment)
{
  FleetMergeResult result;
  if (fragment.server.empty()) {
    return result;
  }
  // Sequence ordering is per author. A fragment from a *different* server is
  // a change of authority (election moved) and must be accepted even with a
  // lower seq -- otherwise an ex-server's high-seq snapshot rejects the new
  // server's fragments forever and the fleet cursor freezes.
  const bool sameAuthor = deskflow::common::namesEqual(state.server, fragment.server);
  if (sameAuthor && fragment.seq < state.seq) {
    return result;
  }

  const bool hadTopology = !state.links.empty();
  const FleetState before = state;

  state = fragment;

  result.changed = state != before;
  result.topologyBecameReady = !hadTopology && !state.links.empty();
  return result;
}

} // namespace deskflow::coordination
