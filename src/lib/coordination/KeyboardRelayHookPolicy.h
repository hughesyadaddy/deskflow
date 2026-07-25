/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <map>

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

//! Tracks where each held button's Down actually went so its Repeat/Up
//! follows the SAME destination, even if the cursor switched screens
//! mid-hold. Tri-state on purpose: "Down went local" must be remembered
//! DISTINCTLY from "never seen". Collapsing them let a locally-delivered
//! Down have its Up forwarded-and-swallowed when the cursor happened to be
//! remote by release time -- the physical key then never released on the
//! local machine (a stuck Shift = everything typed after it is uppercase,
//! including in a login password box).
//! Single-threaded: only ever touched from the hook/tap thread.
class KeyboardRelayForwardLedger
{
public:
  enum class Destination
  {
    Unknown,   //!< never seen (or already released): no claim
    Local,     //!< Down was delivered to the local OS; its Up must be too
    Forwarded, //!< Down went to the mesh; its Up must follow
  };

  //! The Down for \p button was forwarded to the mesh (and swallowed locally).
  void downForwarded(int button)
  {
    m_buttons[button] = Destination::Forwarded;
  }

  //! The Down for \p button stayed local (passthrough, unmapped, or send failed).
  void downLocal(int button)
  {
    m_buttons[button] = Destination::Local;
  }

  //! Where \p button's Down went. Unknown only when genuinely unseen.
  Destination destination(int button) const
  {
    const auto it = m_buttons.find(button);
    return it == m_buttons.end() ? Destination::Unknown : it->second;
  }

  //! True when \p button is held with its Down forwarded: its Repeat/Up must
  //! also forward (and be swallowed locally), regardless of the cursor now.
  bool follow(int button) const
  {
    return destination(button) == Destination::Forwarded;
  }

  //! The Up for \p button was handled; forget the hold.
  void release(int button)
  {
    m_buttons.erase(button);
  }

private:
  std::map<int, Destination> m_buttons;
};

} // namespace deskflow::coordination
