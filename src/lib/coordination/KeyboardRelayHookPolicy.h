/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <set>

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

//! Tracks which buttons had their Down forwarded to the mesh so a held key
//! resolves to exactly ONE destination: the Repeat/Up of a key always follows
//! where its Down went, even if the cursor switched screens mid-hold.
//! Without this, Down-forwarded-remote + Up-delivered-local leaves the key
//! logically held on the remote target forever (and vice versa strands it
//! locally). Single-threaded: only ever touched from the hook/tap thread.
class KeyboardRelayForwardLedger
{
public:
  //! The Down for \p button was forwarded to the mesh (and swallowed locally).
  void downForwarded(int button)
  {
    m_buttons.insert(button);
  }

  //! The Down for \p button stayed local (passthrough, unmapped, or send failed).
  void downLocal(int button)
  {
    m_buttons.erase(button);
  }

  //! True when \p button is held with its Down forwarded: its Repeat/Up must
  //! also forward (and be swallowed locally), regardless of the cursor now.
  bool follow(int button) const
  {
    return m_buttons.count(button) != 0;
  }

  //! The Up for \p button was handled; forget the hold.
  void release(int button)
  {
    m_buttons.erase(button);
  }

private:
  std::set<int> m_buttons;
};

} // namespace deskflow::coordination
