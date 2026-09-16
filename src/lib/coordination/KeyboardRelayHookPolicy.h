/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/KeyTypes.h"

#include <map>
#include <set>
#include <vector>

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

  //! Buttons currently held with their Down forwarded (stop() flush).
  std::vector<int> forwardedButtons() const
  {
    std::vector<int> buttons;
    for (const auto &[button, destination] : m_buttons) {
      if (destination == Destination::Forwarded) {
        buttons.push_back(button);
      }
    }
    return buttons;
  }

  //! Boundary resync: the peer can no longer be told about releases (lane
  //! failed, relay stopping, rescue). Every forwarded hold becomes Local so
  //! its Up passes to the local OS (a release of a key the local OS never
  //! saw pressed is a harmless no-op) instead of being swallowed and lost.
  //! The peer side is released by KeyClearAll / the stop() Up flush.
  void releaseAllForwardedLocally()
  {
    for (auto &[button, destination] : m_buttons) {
      if (destination == Destination::Forwarded) {
        destination = Destination::Local;
      }
    }
  }

  void clear()
  {
    m_buttons.clear();
  }

  bool empty() const
  {
    return m_buttons.empty();
  }

private:
  std::map<int, Destination> m_buttons;
};

//! Where a held key's Up goes. The ledger decides FIRST; the cursor's
//! current screen (passLocal) is consulted only for a Down the hook never
//! saw. Ordering matters (S1): checking passLocal before the ledger let a
//! modifier pressed while remote and released after the cursor came home
//! release locally only -- and stay held on the peer.
enum class KeyboardRelayUpRoute
{
  Local,          //!< Down went to the local OS (or unseen with a local cursor)
  Forward,        //!< Down went to the peer: the Up follows it, whatever the cursor
  ForwardUnknown, //!< unseen Down, cursor remote: forward best-effort, never held
};

inline KeyboardRelayUpRoute
keyboardRelayRouteUp(KeyboardRelayForwardLedger::Destination destination, bool passLocalNow)
{
  switch (destination) {
  case KeyboardRelayForwardLedger::Destination::Forwarded:
    return KeyboardRelayUpRoute::Forward;
  case KeyboardRelayForwardLedger::Destination::Local:
    return KeyboardRelayUpRoute::Local;
  default:
    return passLocalNow ? KeyboardRelayUpRoute::Local : KeyboardRelayUpRoute::ForwardUnknown;
  }
}

//! Hook-side shadow of the modifier state (Windows relay).
/*!
The LL hook swallows a forwarded Shift/Ctrl/Alt/Win Down, and Windows does
NOT reflect swallowed LL events in GetAsyncKeyState / GetKeyState (see
MSWindowsScreen). So the letter typed after a forwarded Shift mapped as
'a', not 'A', and a forwarded CapsLock never flipped the sampled toggle.
The monitor feeds every hardware edge it sees (forwarded or not) into this
shadow and ORs its mask with the OS view: a modifier the OS knows about is
held either way, and a swallowed one is known only here. CapsLock is a
toggle tracked from Down edges (each hardware press flips it, swallowed or
not; injected presses too, since the OS applies those).
Platform-neutral on purpose (VK codes as plain ints) so it is unit-tested
on every host.
*/
class KeyboardRelayModifierShadow
{
public:
  // Virtual-key codes (winuser.h values; no windows.h dependency).
  static constexpr int kVkShift = 0x10;
  static constexpr int kVkControl = 0x11;
  static constexpr int kVkMenu = 0x12;
  static constexpr int kVkCapital = 0x14;
  static constexpr int kVkLWin = 0x5B;
  static constexpr int kVkRWin = 0x5C;
  static constexpr int kVkLShift = 0xA0;
  static constexpr int kVkRShift = 0xA1;
  static constexpr int kVkLControl = 0xA2;
  static constexpr int kVkRControl = 0xA3;
  static constexpr int kVkLMenu = 0xA4;
  static constexpr int kVkRMenu = 0xA5;

  //! Seed the CapsLock toggle from the OS (monitor start).
  void seedCapsLock(bool on)
  {
    m_capsLock = on;
  }

  //! Note one hardware edge. A CapsLock Down (not repeat) flips the toggle.
  void note(int vk, bool down, bool repeat)
  {
    if (vk == kVkCapital) {
      if (down && !repeat) {
        m_capsLock = !m_capsLock;
      }
      return;
    }
    if (modifierBit(vk) == 0) {
      return;
    }
    if (down) {
      m_held.insert(vk);
    } else {
      m_held.erase(vk);
    }
  }

  //! Everything the shadow believes is held / toggled.
  KeyModifierMask mask() const
  {
    KeyModifierMask mask = 0;
    for (const int vk : m_held) {
      mask |= modifierBit(vk);
    }
    if (m_capsLock) {
      mask |= KeyModifierCapsLock;
    }
    return mask;
  }

  bool capsLock() const
  {
    return m_capsLock;
  }

  //! Forget every hold (monitor restart: the hook missed the releases).
  void reset()
  {
    m_held.clear();
  }

  static KeyModifierMask modifierBit(int vk)
  {
    switch (vk) {
    case kVkShift:
    case kVkLShift:
    case kVkRShift:
      return KeyModifierShift;
    case kVkControl:
    case kVkLControl:
    case kVkRControl:
      return KeyModifierControl;
    case kVkMenu:
    case kVkLMenu:
    case kVkRMenu:
      return KeyModifierAlt;
    case kVkLWin:
    case kVkRWin:
      return KeyModifierSuper;
    default:
      return 0;
    }
  }

private:
  std::set<int> m_held;
  bool m_capsLock = false;
};

} // namespace deskflow::coordination
