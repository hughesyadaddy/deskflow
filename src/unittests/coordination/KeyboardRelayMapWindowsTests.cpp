/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "KeyboardRelayMapWindowsTests.h"

#include "coordination/CoordinationProtocol.h"
#include "coordination/KeyboardRelayMap.h"
#include "deskflow/KeyTypes.h"

#include <QTest>

#include <windows.h>

using deskflow::coordination::mapRelayKeyFromHook;
using deskflow::coordination::mapRelayVirtualKey;
using deskflow::coordination::Message;

void KeyboardRelayMapWindowsTests::modifierKeys_mapToFleetKeyIds()
{
  const auto expectDown = [](int vk, KeyID expected) {
    KeyID id = kKeyNone;
    KeyModifierMask mask = 0;
    KeyButton button = 0;
    Message::KeyPhase phase = Message::KeyPhase::Up;
    QVERIFY(mapRelayKeyFromHook(vk, 0, false, false, false, id, mask, button, phase));
    QCOMPARE(phase, Message::KeyPhase::Down);
    QCOMPARE(id, expected);
  };

  expectDown(VK_CONTROL, kKeyControl_L);
  expectDown(VK_LWIN, kKeySuper_L);
  expectDown(VK_SHIFT, kKeyShift_L);
  expectDown(VK_MENU, kKeyAlt_L);
  expectDown(VK_RCONTROL, kKeyControl_R);
  expectDown(VK_RWIN, kKeySuper_R);
}

void KeyboardRelayMapWindowsTests::modifierKeyUp_clearsId()
{
  KeyID id = kKeyControl_L;
  KeyModifierMask mask = 0;
  KeyButton button = 0;
  Message::KeyPhase phase = Message::KeyPhase::Down;
  QVERIFY(mapRelayKeyFromHook(VK_CONTROL, 0, false, true, false, id, mask, button, phase));
  QCOMPARE(phase, Message::KeyPhase::Up);
  QCOMPARE(id, kKeyNone);
  QCOMPARE(button, static_cast<KeyButton>(VK_CONTROL));
}

void KeyboardRelayMapWindowsTests::shiftedKey_translatesToShiftedKeyId()
{
  // Regression: the relay used to call ToUnicodeEx with a zeroed keyboard
  // state, so Shift+A relayed as 'a' and the target's KeyMap released Shift
  // to reproduce the lowercase glyph -- modifiers appeared dead on relayed
  // keyboards. The KeyID must reflect the given Shift/CapsLock state.
  const KeyID unshifted = mapRelayVirtualKey('A', false, false);
  const KeyID shifted = mapRelayVirtualKey('A', true, false);
  const KeyID capsLocked = mapRelayVirtualKey('A', false, true);
  const KeyID shiftCaps = mapRelayVirtualKey('A', true, true);

  QVERIFY(unshifted != kKeyNone);
  QVERIFY(shifted != kKeyNone);
  // On any Latin layout the A key produces a lowercase letter unshifted and
  // its uppercase counterpart shifted or caps-locked; Shift cancels CapsLock.
  if (unshifted >= 'a' && unshifted <= 'z') {
    QCOMPARE(shifted, static_cast<KeyID>(unshifted - 'a' + 'A'));
    QCOMPARE(capsLocked, shifted);
    QCOMPARE(shiftCaps, unshifted);
  } else {
    QVERIFY(shifted != unshifted);
  }

  // Digit row: Shift changes the glyph but CapsLock must not. Only assert on
  // US-like layouts where the key produces '1' unshifted.
  const KeyID digit = mapRelayVirtualKey('1', false, false);
  if (digit == static_cast<KeyID>('1')) {
    QVERIFY(mapRelayVirtualKey('1', true, false) != digit);
    QCOMPARE(mapRelayVirtualKey('1', false, true), digit);
  }
}

QTEST_MAIN(KeyboardRelayMapWindowsTests)
