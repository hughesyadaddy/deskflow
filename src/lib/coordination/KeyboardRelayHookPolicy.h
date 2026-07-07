/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

namespace deskflow::coordination {

//! Inputs to the fleet keyboard-relay hook swallow decision.
struct KeyboardRelayHookContext
{
  bool passLocal = true;   //!< true when keys should reach the local OS
  bool isInjected = false; //!< true for SendInput / synthetic events (Windows LLKHF_INJECTED)
  bool mapped = false;     //!< true when the hook mapped the key to a relay event
  bool forwarded = false;  //!< true when the mesh forward actually sent the key
};

//! Returns true when the hook should pass the key to the OS (not swallow).
inline bool keyboardRelayHookShouldPassThrough(const KeyboardRelayHookContext &ctx)
{
  if (ctx.passLocal) {
    return true;
  }
  // Deskflow-injected keys (Mac host → Windows client via SendInput) must
  // never be eaten by the fleet relay hook, even when routing is stale
  // after a UAC relaunch.
  if (ctx.isInjected) {
    return true;
  }
  if (!ctx.mapped) {
    return true;
  }
  return !ctx.forwarded;
}

} // namespace deskflow::coordination
