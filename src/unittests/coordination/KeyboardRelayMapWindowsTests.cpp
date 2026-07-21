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

void KeyboardRelayMapWindowsTests::functionAndKeypadKeys_mapToFleetKeyIds()
{
  // These used to fall through ToUnicodeEx to kKeyNone: Down leaked to the
  // LOCAL machine while the cursor was remote and the Up was swallowed --
  // key stuck down locally, target never saw it.
  QCOMPARE(mapRelayVirtualKey(VK_F1, false, false), kKeyF1);
  QCOMPARE(mapRelayVirtualKey(VK_F12, false, false), kKeyF12);
  QCOMPARE(mapRelayVirtualKey(VK_F24, false, false), kKeyF24);
  QCOMPARE(mapRelayVirtualKey(VK_SNAPSHOT, false, false), kKeyPrint);
  QCOMPARE(mapRelayVirtualKey(VK_INSERT, false, false), kKeyInsert);
  QCOMPARE(mapRelayVirtualKey(VK_APPS, false, false), kKeyMenu);
  QCOMPARE(mapRelayVirtualKey(VK_NUMLOCK, false, false), kKeyNumLock);
  QCOMPARE(mapRelayVirtualKey(VK_SCROLL, false, false), kKeyScrollLock);
  QCOMPARE(mapRelayVirtualKey(VK_PAUSE, false, false), kKeyPause);
  QCOMPARE(mapRelayVirtualKey(VK_NUMPAD0, false, false), kKeyKP_0);
  QCOMPARE(mapRelayVirtualKey(VK_NUMPAD9, false, false), kKeyKP_9);
  QCOMPARE(mapRelayVirtualKey(VK_ADD, false, false), kKeyKP_Add);
  QCOMPARE(mapRelayVirtualKey(VK_DECIMAL, false, false), kKeyKP_Decimal);

  // Full hook path: an F-key Down must relay (return true, phase Down).
  KeyID id = kKeyNone;
  KeyModifierMask mask = 0;
  KeyButton button = 0;
  Message::KeyPhase phase = Message::KeyPhase::Up;
  QVERIFY(mapRelayKeyFromHook(VK_F5, 0, false, false, false, id, mask, button, phase));
  QCOMPARE(id, kKeyF5);
  QCOMPARE(phase, Message::KeyPhase::Down);
}

void KeyboardRelayMapWindowsTests::unmappedKeyUp_notConsumed()
{
  // Down/Up symmetry: a key whose Down is not relayable (kKeyNone) leaks to
  // the local OS -- its Up must leak too, or the key sticks down locally.
  // VK_PACKET (0xE7) never maps.
  KeyID id = kKeyNone;
  KeyModifierMask mask = 0;
  KeyButton button = 0;
  Message::KeyPhase phase = Message::KeyPhase::Down;
  const bool downMapped = mapRelayKeyFromHook(0xE7, 0, false, false, false, id, mask, button, phase);
  QVERIFY(!downMapped);
  const bool upMapped = mapRelayKeyFromHook(0xE7, 0, false, true, false, id, mask, button, phase);
  QCOMPARE(upMapped, downMapped);
  QCOMPARE(phase, Message::KeyPhase::Up);
}
