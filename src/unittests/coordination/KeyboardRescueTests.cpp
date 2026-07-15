/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "KeyboardRescueTests.h"

#include "coordination/KeyboardRescue.h"
#include "deskflow/KeyTypes.h"

using deskflow::coordination::EscTapRescue;
using Clock = EscTapRescue::Clock;

void KeyboardRescueTests::fiveEscWithinWindow_triggers()
{
  EscTapRescue rescue;
  const auto t0 = Clock::now();
  for (int i = 0; i < EscTapRescue::kTaps - 1; ++i) {
    QVERIFY(!rescue.noteEscDown(kKeyEscape, 0, t0 + std::chrono::milliseconds(100 * i)));
  }
  QVERIFY(rescue.noteEscDown(kKeyEscape, 0, t0 + std::chrono::milliseconds(400)));
  QCOMPARE(rescue.count(), 0); // resets after fire
}

void KeyboardRescueTests::gapBeyondWindow_resetsCount()
{
  EscTapRescue rescue;
  const auto t0 = Clock::now();
  QVERIFY(!rescue.noteEscDown(kKeyEscape, 0, t0));
  QVERIFY(!rescue.noteEscDown(kKeyEscape, 0, t0 + std::chrono::milliseconds(100)));
  QCOMPARE(rescue.count(), 2);
  // Gap > 2s from last tap: count restarts at 1 for this Esc.
  QVERIFY(!rescue.noteEscDown(kKeyEscape, 0, t0 + std::chrono::milliseconds(2500)));
  QCOMPARE(rescue.count(), 1);
}

void KeyboardRescueTests::modifiers_doNotCount()
{
  EscTapRescue rescue;
  const auto t0 = Clock::now();
  QVERIFY(!rescue.noteEscDown(kKeyEscape, KeyModifierControl, t0));
  QCOMPARE(rescue.count(), 0);
  QVERIFY(!rescue.noteEscDown(kKeyEscape, KeyModifierShift | KeyModifierAlt | KeyModifierControl, t0));
  QCOMPARE(rescue.count(), 0);
  QVERIFY(!rescue.noteEscDown('a', 0, t0));
  QCOMPARE(rescue.count(), 0);
}

void KeyboardRescueTests::capsLockIgnored_stillCounts()
{
  EscTapRescue rescue;
  const auto t0 = Clock::now();
  QVERIFY(!rescue.noteEscDown(kKeyEscape, KeyModifierCapsLock, t0));
  QCOMPARE(rescue.count(), 1);
}

void KeyboardRescueTests::nonEsc_breaksStreak()
{
  EscTapRescue rescue;
  const auto t0 = Clock::now();
  QVERIFY(!rescue.noteEscDown(kKeyEscape, 0, t0));
  QVERIFY(!rescue.noteEscDown(kKeyEscape, 0, t0 + std::chrono::milliseconds(50)));
  QCOMPARE(rescue.count(), 2);
  QVERIFY(!rescue.noteEscDown('a', 0, t0 + std::chrono::milliseconds(100)));
  QCOMPARE(rescue.count(), 0);
  QVERIFY(!rescue.noteEscDown(kKeyEscape, 0, t0 + std::chrono::milliseconds(150)));
  QCOMPARE(rescue.count(), 1);
}

QTEST_MAIN(KeyboardRescueTests)
