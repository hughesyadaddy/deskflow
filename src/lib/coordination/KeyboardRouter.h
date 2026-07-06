/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <string>

namespace deskflow::coordination {

//! Where a physical key event should be delivered.
enum class KeyboardRoute
{
  Local,
  Forward
};

struct KeyboardRouteDecision
{
  KeyboardRoute route = KeyboardRoute::Local;
  //! Cursor-host screen or peer name when \c route is Forward.
  std::string forwardHost;
};

struct KeyboardRouteInput
{
  std::string selfName;
  std::string cursorHost;
  bool cursorHostKnown = false;
};

//! Pure cursor-host routing: local synthesize vs forward to fleet cursor host.
KeyboardRouteDecision routeKeyboard(const KeyboardRouteInput &input);

} // namespace deskflow::coordination
