/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2011 Nick Bolton
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "KeyStateTests.h"
#include "base/EventQueue.h"
#include "deskflow/KeyMap.h"

#include "MockEventQueue.h"
#include "MockKeyMap.h"
#include "MockKeyState.h"

#include <vector>

namespace {

//! KeyState whose OS reports Shift physically held and whose injections
//! are recorded instead of posted; presses go through the real
//! KeyState::fakeKeyDown so they land in the synthetic ledger.
class ShiftHeldKeyState : public MockKeyState
{
public:
  using MockKeyState::MockKeyState;

  struct Posted
  {
    KeyButton button;
    bool press;
  };
  std::vector<Posted> posted;

  KeyModifierMask pollActiveModifiers() const override
  {
    return KeyModifierShift;
  }
  void fakeKey(const Keystroke &keystroke) override
  {
    if (keystroke.m_type == Keystroke::KeyType::Button) {
      posted.push_back({keystroke.m_data.m_button.m_button, keystroke.m_data.m_button.m_press});
    }
  }
  void fakeKeyDown(KeyID id, KeyModifierMask mask, KeyButton serverID, const std::string &lang) override
  {
    KeyState::fakeKeyDown(id, mask, serverID, lang);
  }
  bool isKeyDown(KeyButton button) const override
  {
    return KeyState::isKeyDown(button);
  }
};

} // namespace

void KeyStateTests::initTestCase()
{
  m_arch.init();
}

void KeyStateTests::keyDown()
{
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  MockKeyState keyState(eventQueue, keyMap);

  keyState.onKey(1, true, KeyModifierAlt);

  QVERIFY(keyState.getKeyState(1));
}

void KeyStateTests::keyUp()
{
  MockEventQueue eventQueue;
  MockKeyState keyState(eventQueue, m_keymap);
  QVERIFY(!keyState.getKeyState(1));
}

void KeyStateTests::invalidKey()
{
  MockEventQueue eventQueue;
  MockKeyState keyState(eventQueue, m_keymap);

  keyState.onKey(0, true, KeyModifierAlt);

  QVERIFY(!keyState.getKeyState(0));
}

void KeyStateTests::onKey_aKeyDown_keyStateOne()
{
  MockEventQueue eventQueue;
  MockKeyState keyState(eventQueue, m_keymap);

  keyState.onKey(1, true, KeyModifierAlt);

  QVERIFY(keyState.getKeyState(1));
}

void KeyStateTests::onKey_aKeyUp_keyStateZero()
{
  MockEventQueue eventQueue;
  MockKeyState keyState(eventQueue, m_keymap);

  keyState.onKey(1, false, KeyModifierAlt);

  QVERIFY(!keyState.getKeyState(1));
}

void KeyStateTests::onKey_invalidKey_keyStateZero()
{
  MockEventQueue eventQueue;
  MockKeyState keyState(eventQueue, m_keymap);

  keyState.onKey(0, true, KeyModifierAlt);

  QVERIFY(!keyState.getKeyState(0));
}

void KeyStateTests::updateKeyState_pollDoesNothing_keyNotSet()
{
  MockEventQueue eventQueue;
  MockKeyState keyState(eventQueue, m_keymap);

  keyState.updateKeyState();

  QVERIFY(!keyState.isKeyDown(1));
}

void KeyStateTests::updateKeyState_activeModifiers_maskNotSet()
{
  MockEventQueue eventQueue;
  MockKeyState keyState(eventQueue, m_keymap);

  keyState.updateKeyState();

  QCOMPARE(0, keyState.getActiveModifiers());
}

void KeyStateTests::fakeKeyRepeat_invalidKey_returnsFalse()
{
  MockEventQueue eventQueue;
  MockKeyState keyState(eventQueue, m_keymap);

  QVERIFY(!keyState.fakeKeyRepeat(0, 0, 0, 0, "en"));
}

void KeyStateTests::fakeKeyUp_buttonNotDown_returnsFalse()
{
  MockEventQueue eventQueue;
  MockKeyState keyState(eventQueue, m_keymap);

  QVERIFY(!keyState.fakeKeyUp(0));
}

void KeyStateTests::isKeyDown_noKeysDown_returnsFalse()
{
  MockEventQueue eventQueue;
  MockKeyState keyState(eventQueue, m_keymap);

  QVERIFY(!keyState.isKeyDown(1));
}

void KeyStateTests::isKeyDown_keyDown_retrunsTrue()
{
  MockKeyMap keyMap;
  MockEventQueue eventQueue;
  MockKeyState keyState(eventQueue, keyMap);

  deskflow::KeyMap::KeyItem key;
  key.m_button = 1;
  keyState.fakeKeyDown(1, 0, 1, "en");

  QVERIFY(keyState.isKeyDown(1));
}

void KeyStateTests::updateKeyState_pollInsertsSingleKey_keyIsDown()
{
  MockKeyMap keyMap;
  MockEventQueue eventQueue;
  MockKeyState keyState(eventQueue, keyMap);

  deskflow::KeyMap::KeyItem key;
  key.m_button = 1;
  keyState.fakeKeyDown(1, 0, 1, "en");

  keyState.updateKeyState();
  QVERIFY(keyState.isKeyDown(1));
}

void KeyStateTests::fakeAllKeysUp_releasesOnlySyntheticThenReseeds()
{
  // K2: leave releases what WE pressed and nothing the user physically
  // holds, then re-reads the OS so the tracked mask does not go stale.
  deskflow::KeyMap keyMap;
  deskflow::KeyMap::KeyItem a;
  a.m_id = static_cast<KeyID>('a');
  a.m_button = 1;
  a.m_group = 0;
  keyMap.addKeyEntry(a);
  keyMap.finish();
  MockEventQueue eventQueue;
  ShiftHeldKeyState keyState(eventQueue, keyMap);

  // a synthetic 'a' from the server
  keyState.fakeKeyDown(a.m_id, 0, 7, "en");
  QVERIFY(keyState.isKeyDown(1));
  QCOMPARE(keyState.posted.size(), size_t(1));
  QVERIFY(keyState.posted[0].press);
  keyState.posted.clear();

  // the OS holds Shift (the user's) which never entered the ledger
  keyState.fakeAllKeysUp();

  // exactly one release, for 'a'; Shift is untouched ...
  QCOMPARE(keyState.posted.size(), size_t(1));
  QCOMPARE(keyState.posted[0].button, KeyButton(1));
  QVERIFY(!keyState.posted[0].press);
  QVERIFY(!keyState.isKeyDown(1));
  // ... and the tracked mask is reseeded from OS truth
  QCOMPARE(keyState.getActiveModifiers(), KeyModifierShift);

  // with an empty ledger nothing is posted at all, mask still OS truth
  keyState.posted.clear();
  keyState.fakeAllKeysUp();
  QVERIFY(keyState.posted.empty());
  QCOMPARE(keyState.getActiveModifiers(), KeyModifierShift);
}

// K4 audit B-2: fakeAllKeysUp reseeds m_mask from the OS but used to leave
// m_activeModifiers empty, so a modifier the OS reports held had no key item
// and KeyMap could never synthesise its release: the next 'a' went out with
// Shift still down and typed 'A'. After the fix the table is rebuilt like
// updateKeyState does, and the Shift release precedes the letter.
void KeyStateTests::fakeAllKeysUp_reseedsActiveModifiers_osHeldShiftIsReleasable()
{
  constexpr KeyButton kAButton = 1;
  constexpr KeyButton kShiftButton = 2;
  deskflow::KeyMap keyMap;
  deskflow::KeyMap::KeyItem shift;
  shift.m_id = kKeyShift_L;
  shift.m_button = kShiftButton;
  shift.m_group = 0;
  shift.m_generates = KeyModifierShift;
  keyMap.addKeyEntry(shift);
  deskflow::KeyMap::KeyItem a;
  a.m_id = static_cast<KeyID>('a');
  a.m_button = kAButton;
  a.m_group = 0;
  a.m_required = 0;
  a.m_sensitive = KeyModifierShift;
  keyMap.addKeyEntry(a);
  deskflow::KeyMap::KeyItem upperA;
  upperA.m_id = static_cast<KeyID>('A');
  upperA.m_button = kAButton;
  upperA.m_group = 0;
  upperA.m_required = KeyModifierShift;
  upperA.m_sensitive = KeyModifierShift;
  keyMap.addKeyEntry(upperA);
  keyMap.finish();
  MockEventQueue eventQueue;
  ShiftHeldKeyState keyState(eventQueue, keyMap);

  // resync with an empty ledger: nothing posted, mask reseeded to OS truth
  keyState.fakeAllKeysUp();
  QVERIFY(keyState.posted.empty());
  QCOMPARE(keyState.getActiveModifiers(), KeyModifierShift);

  // the server wants a lowercase 'a': Shift must be released first
  keyState.fakeKeyDown(a.m_id, 0, 7, "en");
  size_t shiftReleaseAt = keyState.posted.size();
  size_t aPressAt = keyState.posted.size();
  for (size_t i = 0; i < keyState.posted.size(); ++i) {
    const auto &p = keyState.posted[i];
    if (p.button == kShiftButton && !p.press && shiftReleaseAt == keyState.posted.size()) {
      shiftReleaseAt = i;
    }
    if (p.button == kAButton && p.press) {
      aPressAt = i;
    }
  }
  QVERIFY2(shiftReleaseAt < keyState.posted.size(), "no Shift release was synthesised");
  QVERIFY2(aPressAt < keyState.posted.size(), "the letter was not pressed");
  QVERIFY(shiftReleaseAt < aPressAt);
}

QTEST_MAIN(KeyStateTests)
