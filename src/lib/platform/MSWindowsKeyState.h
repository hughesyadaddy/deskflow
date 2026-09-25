/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2003 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/KeyState.h"
#include "platform/MSWindowsModifierLedger.h"

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

class Event;
class EventQueueTimer;
class MSWindowsDesks;
class IEventQueue;

//! Microsoft Windows key mapper
/*!
This class maps KeyIDs to keystrokes.
*/
class MSWindowsKeyState : public KeyState
{
public:
  //! Injected-modifier ledger: VK -> tick (ms) of the most recent injected
  //! DOWN (or repeat) that has not been followed by an injected UP.
  /*!
  THREADING (D2): the ledger is written from the main thread (fakeKey,
  releaseInjectedKeys at boundaries) AND from the desk thread
  (MSWindowsScreen::updateKeysCB -> releaseInjectedKeys on every desk
  switch), and read from both. Every access goes through
  m_injectedModifiersMutex in a short critical section; the lock is never
  held across SendInput / releaseHeldKeys (which round-trips to the desk
  thread and would deadlock a desk-thread caller).
  */
  using InjectedModifierMap = deskflow::platform::InjectedModifierMap;
  static_assert(sizeof(WORD) == sizeof(uint16_t) && sizeof(ULONGLONG) == sizeof(uint64_t));

  //! How long an injected DOWN vouches for a physically held modifier AT A
  //! BOUNDARY (screen not entered).
  /*!
  The 1 s audit skips modifiers in the ledger so a chord the server is
  deliberately holding is not released under it. While the screen is
  ENTERED every ledger entry vouches, however old: a macOS server never
  repeats modifiers, so Ctrl/Alt/Win held for more than a couple of seconds
  used to be released under the user (K4 audit MED-1). The grace applies
  only at boundaries (enable, desk switch, wake), where the server holds
  nothing here and a DOWN whose UP never arrived (server gone mid-chord,
  epoch restart, desk switch dropping the UP) must not be protected forever
  -- the ledger would defend the very stranded key the audit exists to
  release.
  */
  static constexpr ULONGLONG kInjectedModifierGraceMs = 2000;

  //! Bitfield of modifier VKs this client has injected DOWN and not released.
  /*!
  Derived only from injection outcomes, never from the OS -- reading the OS
  back would let a stuck modifier certify itself as intended. Bit order
  matches the table in MSWindowsDesks' stale-modifier audit. With
  \p entered false, entries older than kInjectedModifierGraceMs are
  excluded (see there); while entered, every entry vouches.
  */
  uint32_t injectedModifierBits(bool entered) const;

  //! Pure form of injectedModifierBits() for unit tests: bits of \p ledger
  //! entries, all of them while \p entered, else only those stamped within
  //! the grace window of \p nowMs.
  static uint32_t injectedModifierBits(const InjectedModifierMap &ledger, ULONGLONG nowMs, bool entered);

  //! Candidate VKs for sanitizeInjectedKeys(): every VK in \p ledger plus
  //! VK_LSHIFT/VK_RSHIFT, ascending. The desk thread probes and releases
  //! these (it is the only thread bound to the input desktop).
  static std::vector<WORD> injectedKeyCandidates(const InjectedModifierMap &ledger);

  //! Pure decision logic for sanitizeInjectedKeys(): the VKs to release.
  /*!
  injectedKeyCandidates() filtered to those \p isPhysicallyDown(vk) reports
  held. \p isPhysicallyDown stands in for GetAsyncKeyState so the decision
  is testable without Win32 input state; at runtime the desk thread applies
  the same filter (deskReleaseHeldKeys) on the input desktop.
  */
  static std::vector<WORD>
  injectedKeysToRelease(const InjectedModifierMap &ledger, const std::function<bool(WORD)> &isPhysicallyDown);

  //! Index of \p vk in the tracked-modifier table, or -1.
  static int modifierVkIndex(WORD vk);

  //! Pure decision logic for releaseInjectedKeys(): the ledger's VKs minus
  //! those whose modifier bit is in \p keep, ascending. Never adds Shift or
  //! anything else the ledger does not hold (K4 audit MED-2/MED-3).
  static std::vector<WORD> ledgerKeysToRelease(const InjectedModifierMap &ledger, KeyModifierMask keep);

  //! D1: forget every ledgered VK in \p released (the audit / a boundary
  //! sweep just injected its UP outside this class). Thread-safe.
  void forgetInjectedModifiers(const std::vector<WORD> &released);

  //! Tick (ms) of the last key DOWN this client injected on this screen,
  //! 0 if none yet. Feeds the audit's quiet window (D3). Thread-safe.
  uint64_t lastInjectedKeyDownMs() const
  {
    return m_lastInjectedKeyDownMs.load(std::memory_order_relaxed);
  }

  MSWindowsKeyState(
      MSWindowsDesks *desks, void *eventTarget, IEventQueue *events, std::vector<std::string> layouts,
      bool isLangSyncEnabled
  );
  MSWindowsKeyState(
      MSWindowsDesks *desks, void *eventTarget, IEventQueue *events, deskflow::KeyMap &keyMap,
      std::vector<std::string> layouts, bool isLangSyncEnabled
  );
  virtual ~MSWindowsKeyState();

  //! @name manipulators
  //@{

  //! Handle screen disabling
  /*!
  Called when screen is disabled.  This is needed to deal with platform
  brokenness.
  */
  void disable();

  //! Set the active keyboard layout
  /*!
  Uses \p keyLayout when querying the keyboard.
  */
  void setKeyLayout(HKL keyLayout);

  //! Test and set autorepeat state
  /*!
  Returns true if the given button is autorepeating and updates internal
  state.
  */
  bool testAutoRepeat(bool press, bool isRepeat, KeyButton);

  //! Remember modifier state
  /*!
  Records the current non-toggle modifier state.
  */
  void saveModifiers();

  //! Set effective modifier state
  /*!
  Temporarily sets the non-toggle modifier state to those saved by the
  last call to \c saveModifiers if \p enable is \c true.  Restores the
  modifier state to the current modifier state if \p enable is \c false.
  This is for synthesizing keystrokes on the primary screen when the
  cursor is on a secondary screen.  When on a secondary screen we capture
  all non-toggle modifier state, track the state internally and do not
  pass it on.  So if Alt+F1 synthesizes Alt+X we need to synthesize
  not just X but also Alt, despite the fact that our internal modifier
  state indicates Alt is down, because local apps never saw the Alt down
  event.
  */
  void useSavedModifiers(bool enable);

  //@}
  //! @name accessors
  //@{

  //! Map a virtual key to a button
  /*!
  Returns the button for the \p virtualKey.
  */
  KeyButton virtualKeyToButton(UINT virtualKey) const;

  //! Map key event to a key
  /*!
  Converts a key event into a KeyID and the shadow modifier state
  to a modifier mask.
  */
  KeyID mapKeyFromEvent(WPARAM charAndVirtKey, LPARAM info, KeyModifierMask *maskOut) const;

  //! Check if keyboard groups have changed
  /*!
  Returns true iff the number or order of the keyboard groups have
  changed since the last call to updateKeys().
  */
  bool didGroupsChange() const;

  //! Map key to virtual key
  /*!
  Returns the virtual key for key \p key or 0 if there's no such virtual
  key.
  */
  UINT mapKeyToVirtualKey(KeyID key) const;

  //! Map virtual key and button to KeyID
  /*!
  Returns the KeyID for virtual key \p virtualKey and button \p button
  (button should include the extended key bit), or kKeyNone if there is
  no such key.
  */
  KeyID getKeyID(UINT virtualKey, KeyButton button) const;

  //! Map button to virtual key
  /*!
  Returns the virtual key for button \p button
  (button should include the extended key bit), or kKeyNone if there is
  no such key.
  */
  UINT mapButtonToVirtualKey(KeyButton button) const;

  //@}

  // IKeyState overrides
  void fakeKeyDown(KeyID id, KeyModifierMask mask, KeyButton button, const std::string &lang) override;
  bool fakeKeyRepeat(KeyID id, KeyModifierMask mask, int32_t count, KeyButton button, const std::string &lang) override;
  bool fakeCtrlAltDel() override;
  KeyModifierMask pollActiveModifiers() const override;
  int32_t pollActiveGroup() const override;
  void pollPressedKeys(KeyButtonSet &pressedKeys) const override;
  void setToggleState(KeyModifierMask bit, bool on) override;
  void sanitizeInjectedKeys() override;
  void releaseInjectedKeys(KeyModifierMask keep = 0) override;

  // KeyState overrides
  void onKey(KeyButton button, bool down, KeyModifierMask newState) override;
  void sendKeyEvent(
      void *target, bool press, bool isAutoRepeat, KeyID key, KeyModifierMask mask, int32_t count, KeyButton button
  ) override;

  // Unit test accessors
  KeyButton getLastDown() const
  {
    return m_lastDown;
  }
  void setLastDown(KeyButton value)
  {
    m_lastDown = value;
  }
  KeyModifierMask getSavedModifiers() const
  {
    return m_savedModifiers;
  }
  void setSavedModifiers(KeyModifierMask value)
  {
    m_savedModifiers = value;
  }

protected:
  // KeyState overrides
  void getKeyMap(deskflow::KeyMap &keyMap) override;
  void fakeKey(const Keystroke &keystroke) override;
  KeyModifierMask &getActiveModifiersRValue() override;

private:
  void noteInjectedModifier(WORD vk, bool held);
  InjectedModifierMap m_injectedModifiers;              // guarded by m_injectedModifiersMutex (D2)
  mutable std::mutex m_injectedModifiersMutex;
  std::atomic<uint64_t> m_lastInjectedKeyDownMs{0}; // D3 quiet window

  using GroupList = std::vector<HKL>;

  bool getGroups(GroupList &) const;
  void setWindowGroup(int32_t group);

  KeyID getIDForKey(deskflow::KeyMap::KeyItem &item, KeyButton button, UINT virtualKey, PBYTE keyState, HKL hkl) const;

  void addKeyEntry(deskflow::KeyMap &keyMap, deskflow::KeyMap::KeyItem &item);

  void init();

private:
  // not implemented
  MSWindowsKeyState(const MSWindowsKeyState &);
  MSWindowsKeyState &operator=(const MSWindowsKeyState &);

private:
  using GroupMap = std::map<HKL, int32_t>;
  using KeyToVKMap = std::map<KeyID, UINT>;

  void *m_eventTarget;
  MSWindowsDesks *m_desks;
  HKL m_keyLayout;
  UINT m_buttonToVK[512];
  UINT m_buttonToNumpadVK[512];
  KeyButton m_virtualKeyToButton[256];
  KeyToVKMap m_keyToVKMap;
  IEventQueue *m_events;

  // the timer used to check for fixing key state
  EventQueueTimer *m_fixTimer;

  // the groups (keyboard layouts)
  GroupList m_groups;
  GroupMap m_groupMap;

  // the last button that we generated a key down event for.  this
  // is zero if the last key event was a key up.  we use this to
  // synthesize key repeats since the low level keyboard hook can't
  // tell us if an event is a key repeat.
  KeyButton m_lastDown;

  // modifier tracking
  bool m_useSavedModifiers;
  KeyModifierMask m_savedModifiers;
  KeyModifierMask m_originalSavedModifiers;

  // pointer to ToUnicodeEx.  on win95 family this will be nullptr.
  typedef int(WINAPI *ToUnicodeEx_t)(
      UINT wVirtKey, UINT wScanCode, PBYTE lpKeyState, LPWSTR pwszBuff, int cchBuff, UINT wFlags, HKL dwhkl
  );
  ToUnicodeEx_t m_ToUnicodeEx;

  static const KeyID s_virtualKey[];
};
