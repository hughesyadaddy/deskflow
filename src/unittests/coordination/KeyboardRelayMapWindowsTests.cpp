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

QTEST_MAIN(KeyboardRelayMapWindowsTests)
