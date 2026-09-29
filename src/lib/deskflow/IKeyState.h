/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2003 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "base/IEventQueue.h"
#include "base/String.h"
#include "deskflow/KeyMap.h"
#include "deskflow/KeyTypes.h"

#include <set>
#include <string>

//! Key state interface
/*!
This interface provides access to set and query the keyboard state and
to synthesize key events.
*/
class IKeyState
{
public:
  explicit IKeyState(const IEventQueue *events);
  virtual ~IKeyState() = default;
  inline static const auto s_numButtons = 0x200;

  //! Key event data
  class KeyInfo
  {
  public:
    static KeyInfo *alloc(KeyID, KeyModifierMask, KeyButton, int32_t count);
    static KeyInfo *alloc(KeyID, KeyModifierMask, KeyButton, int32_t count, const std::set<std::string> &destinations);
    static KeyInfo *alloc(const KeyInfo &);

    static bool isDefault(const char *screens);
    static bool contains(const char *screens, const std::string_view &name);
    static bool equal(const KeyInfo *, const KeyInfo *);
    static std::string join(const std::set<std::string> &destinations);
    static void split(const char *screens, std::set<std::string> &);

  public:
    KeyID m_key;
    KeyModifierMask m_mask;
    KeyButton m_button;
    int32_t m_count;
    std::string m_screens;
  };

  using KeyButtonSet = std::set<KeyButton>;

  //! The lock (toggle) modifier bits: Caps, Num and Scroll Lock.
  inline static const KeyModifierMask s_lockModifierMask =
      KeyModifierCapsLock | KeyModifierNumLock | KeyModifierScrollLock;

  //! Lock modifier bit driven by lock key \p id, or 0 if \p id is not a lock key.
  static KeyModifierMask lockModifierForKey(KeyID id)
  {
    switch (id) {
    case kKeyCapsLock:
      return KeyModifierCapsLock;
    case kKeyNumLock:
      return KeyModifierNumLock;
    case kKeyScrollLock:
      return KeyModifierScrollLock;
    default:
      return 0;
    }
  }

  //! Key id for logs: "65 ('A')" for printable ids (actual case), else
  //! "61413 (CapsLock)" via the KeyMap name table.
  static std::string describeKey(KeyID id)
  {
    if (id >= 0x20 && id < 0x7F) {
      return deskflow::string::sprintf("%u ('%c')", static_cast<unsigned>(id), static_cast<char>(id));
    }
    return deskflow::string::sprintf("%u (%s)", static_cast<unsigned>(id), deskflow::KeyMap::formatKey(id, 0).c_str());
  }

  //! Lock key that drives lock modifier bit \p lock, or kKeyNone.
  static KeyID lockKeyForModifier(KeyModifierMask lock)
  {
    switch (lock) {
    case KeyModifierCapsLock:
      return kKeyCapsLock;
    case KeyModifierNumLock:
      return kKeyNumLock;
    case KeyModifierScrollLock:
      return kKeyScrollLock;
    default:
      return kKeyNone;
    }
  }

  //! @name manipulators
  //@{

  //! Update the keyboard map
  /*!
  Causes the key state to get updated to reflect the current keyboard
  mapping.
  */
  virtual void updateKeyMap() = 0;

  //! Update the key state
  /*!
  Causes the key state to get updated to reflect the physical keyboard
  state.
  */
  virtual void updateKeyState() = 0;

  //! Set half-duplex mask
  /*!
  Sets which modifier toggle keys are half-duplex.  A half-duplex
  toggle key doesn't report a key release when toggled on and
  doesn't report a key press when toggled off.
  */
  virtual void setHalfDuplexMask(KeyModifierMask) = 0;

  //! Fake a key press
  /*!
  Synthesizes a key press event and updates the key state.
  */
  virtual void fakeKeyDown(KeyID id, KeyModifierMask mask, KeyButton button, const std::string &lang) = 0;

  //! Fake a key repeat
  /*!
  Synthesizes a key repeat event and updates the key state.
  */
  virtual bool
  fakeKeyRepeat(KeyID id, KeyModifierMask mask, int32_t count, KeyButton button, const std::string &lang) = 0;

  //! Fake a key release
  /*!
  Synthesizes a key release event and updates the key state.
  */
  virtual bool fakeKeyUp(KeyButton button) = 0;

  //! Fake key releases for all fake pressed keys
  /*!
  Synthesizes a key release event for every key that is synthetically
  pressed and updates the key state.
  */
  virtual void fakeAllKeysUp() = 0;

  //! Clear stale modifiers
  /*!
  Clears stuck modifier state in platform-specific keyboard tracking (e.g. XKB).
  Default implementation does nothing.
  */
  virtual void clearStaleModifiers()
  {
    // Default implementation does nothing
  }

  //! Force a toggle modifier (CapsLock/NumLock/ScrollLock) to a state
  /*!
  Drives the OS lock state for \p bit to \p on without faking a key press
  when the OS already agrees. Default implementation does nothing.
  */
  virtual void setToggleState(KeyModifierMask bit, bool on)
  {
  }

  //! Release modifiers this process injected that the OS still reports down
  /*!
  Reconciles injected modifier state with OS truth (e.g. after lock/unlock,
  wake or a lost key-up). Never touches modifiers the user physically holds.
  Default implementation does nothing.
  */
  virtual void sanitizeInjectedKeys()
  {
  }

  //! Release ONLY the modifiers this process injected and never released
  /*!
  The strict subset of sanitizeInjectedKeys(): the platform's injected
  ledger, nothing else. Unlike the full sweep it never reasons about
  freshness, so a modifier the user is physically holding at THIS keyboard
  cannot be swept -- safe to call while the user may be mid-gesture (return
  to the primary, the post-switch verifier). Modifiers whose bit is in
  \p keep are left in the ledger and NOT released: the post-switch verifier
  passes the modifiers it re-asserted on enter for an ongoing shift-drag,
  so closing a stuck Ctrl never drops the user's Shift with it (K4 audit
  MED-3). Default does nothing.
  */
  virtual void releaseInjectedKeys(KeyModifierMask keep = 0)
  {
    (void)keep;
  }

  //! Fake ctrl+alt+del
  /*!
  Synthesize a press of ctrl+alt+del.  Return true if processing is
  complete and false if normal key processing should continue.
  */
  virtual bool fakeCtrlAltDel() = 0;

  //! Fake a media key
  /*!
   Synthesizes a media key down and up. Only Mac would implement this by
   use cocoa appkit framework.
   */
  virtual bool fakeMediaKey(KeyID id) = 0;

  //@}
  //! @name accessors
  //@{

  //! Test if key is pressed
  /*!
  Returns true iff the given key is down.  Half-duplex toggles
  always return false.
  */
  virtual bool isKeyDown(KeyButton) const = 0;

  //! Get the active modifiers
  /*!
  Returns the modifiers that are currently active according to our
  shadowed state.
  */
  virtual KeyModifierMask getActiveModifiers() const = 0;

  //! Get the active modifiers from OS
  /*!
  Returns the modifiers that are currently active according to the
  operating system.
  */
  virtual KeyModifierMask pollActiveModifiers() const = 0;

  //! Get the active keyboard layout from OS
  /*!
  Returns the active keyboard layout according to the operating system.
  */
  virtual int32_t pollActiveGroup() const = 0;

  //! Get the keys currently pressed from OS
  /*!
  Adds any keys that are currently pressed according to the operating
  system to \p pressedKeys.
  */
  virtual void pollPressedKeys(KeyButtonSet &pressedKeys) const = 0;

  //@}
};
