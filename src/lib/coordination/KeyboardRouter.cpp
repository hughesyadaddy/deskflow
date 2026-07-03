/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "common/FleetCursor.h"
#include "coordination/KeyboardRouter.h"

#include "coordination/KeyboardRelayDecision.h"

namespace deskflow::coordination {

using deskflow::common::cursorHostIsLocal;

KeyboardRouteDecision routeKeyboard(const KeyboardRouteInput &input)
{
  // Unknown cursor host must NEVER swallow the local keyboard: a client
  // that has not yet received a fleet fragment (late join, dropped
  // broadcast) would otherwise eat every physical keystroke and forward
  // it blindly. Local is always the safe default; the server's heartbeat
  // rebroadcast converges the snapshot within seconds.
  if (!input.cursorHostKnown || input.cursorHost.empty() ||
      cursorHostIsLocal(input.selfName, input.cursorHost)) {
    return {KeyboardRoute::Local, {}};
  }

  return {KeyboardRoute::Forward, input.cursorHost};
}

} // namespace deskflow::coordination
