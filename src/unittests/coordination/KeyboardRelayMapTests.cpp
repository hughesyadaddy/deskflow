/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "KeyboardRelayMapTests.h"

#include "coordination/CoordinationProtocol.h"
#include "coordination/KeyboardRelayMap.h"
#include "deskflow/KeyTypes.h"
#include "OSXMainQueueTestHarness.h"

#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>

using deskflow::coordination::Message;

namespace {

struct RelayMapResult
{
  bool mapped = false;
  Message::KeyPhase phase = Message::KeyPhase::Down;
  KeyID id = kKeyNone;
  KeyModifierMask mask = 0;
  KeyButton button = 0;
};

RelayMapResult mapRelayKey(CGEventRef event)
{
  RelayMapResult result;
  result.mapped = deskflow::coordination::mapRelayKeyFromCgEvent(
      event, result.phase, result.id, result.mask, result.button
  );
  return result;
}

} // namespace

void KeyboardRelayMapTests::mapRelayKeyFromCgEventOffMainThread_keyDown()
{
  deskflow::test::osx::ScopedCGEvent event(CGEventCreateKeyboardEvent(nullptr, kVK_Space, true));
  QVERIFY2(event.get() != nullptr, "CGEventCreateKeyboardEvent failed");

  const auto offMainOpt = deskflow::test::osx::runOffMainThreadWithMainQueuePump([&]() {
    return mapRelayKey(event.get());
  });
  QVERIFY2(offMainOpt.has_value(), "off-main-thread task did not complete within timeout");
  const RelayMapResult offMain = *offMainOpt;

  QVERIFY2(offMain.mapped, "space key down should map");
  QCOMPARE(offMain.phase, Message::KeyPhase::Down);
  QCOMPARE(offMain.id, static_cast<KeyID>(' '));
  QCOMPARE(offMain.button, static_cast<KeyButton>(kVK_Space));
}

void KeyboardRelayMapTests::mapRelayKeyFromCgEventOffMainThread_keyUp()
{
  deskflow::test::osx::ScopedCGEvent event(CGEventCreateKeyboardEvent(nullptr, kVK_Space, false));
  QVERIFY2(event.get() != nullptr, "CGEventCreateKeyboardEvent failed");

  const auto offMainOpt = deskflow::test::osx::runOffMainThreadWithMainQueuePump([&]() {
    return mapRelayKey(event.get());
  });
  QVERIFY2(offMainOpt.has_value(), "off-main-thread task did not complete within timeout");
  const RelayMapResult offMain = *offMainOpt;

  QVERIFY2(offMain.mapped, "key up should succeed without TIS");
  QCOMPARE(offMain.phase, Message::KeyPhase::Up);
  QCOMPARE(offMain.id, kKeyNone);
  QCOMPARE(offMain.button, static_cast<KeyButton>(kVK_Space));
}

void KeyboardRelayMapTests::mapRelayKeyFromCgEventOffMainThread_autorepeat()
{
  deskflow::test::osx::ScopedCGEvent event(CGEventCreateKeyboardEvent(nullptr, kVK_Space, true));
  QVERIFY2(event.get() != nullptr, "CGEventCreateKeyboardEvent failed");
  CGEventSetIntegerValueField(event.get(), kCGKeyboardEventAutorepeat, 1);

  const auto offMainOpt = deskflow::test::osx::runOffMainThreadWithMainQueuePump([&]() {
    return mapRelayKey(event.get());
  });
  QVERIFY2(offMainOpt.has_value(), "off-main-thread task did not complete within timeout");
  const RelayMapResult offMain = *offMainOpt;

  QVERIFY2(offMain.mapped, "autorepeat should map even when id resolves to none");
  QCOMPARE(offMain.phase, Message::KeyPhase::Repeat);
  QCOMPARE(offMain.button, static_cast<KeyButton>(kVK_Space));
}

void KeyboardRelayMapTests::mapRelayKeyFromCgEventOffMainThread_matchesMainThread()
{
  deskflow::test::osx::ScopedCGEvent event(CGEventCreateKeyboardEvent(nullptr, kVK_Space, true));
  QVERIFY2(event.get() != nullptr, "CGEventCreateKeyboardEvent failed");

  const RelayMapResult onMain = mapRelayKey(event.get());
  const auto offMainOpt = deskflow::test::osx::runOffMainThreadWithMainQueuePump([&]() {
    return mapRelayKey(event.get());
  });
  QVERIFY2(offMainOpt.has_value(), "off-main-thread task did not complete within timeout");
  const RelayMapResult offMain = *offMainOpt;

  QCOMPARE(offMain.mapped, onMain.mapped);
  QCOMPARE(offMain.phase, onMain.phase);
  QCOMPARE(offMain.id, onMain.id);
  QCOMPARE(offMain.mask, onMain.mask);
  QCOMPARE(offMain.button, onMain.button);
}

QTEST_MAIN(KeyboardRelayMapTests)
