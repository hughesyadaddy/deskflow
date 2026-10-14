/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "EscDropoutTests.h"

#include "coordination/EscDropout.h"

#include <QTest>

using deskflow::coordination::EscTapBurst;
using namespace std::chrono_literals;

namespace {
int tapN(EscTapBurst &b, int n, EscTapBurst::Clock::time_point &t, std::chrono::milliseconds gap)
{
  int fired = 0;
  for (int i = 0; i < n; ++i) {
    fired += b.noteKeyDown(true, false, t) ? 1 : 0;
    t += gap;
  }
  return fired;
}
} // namespace

void EscDropoutTests::fiveQuickEscTriggersOnce()
{
  EscTapBurst b;
  auto t = EscTapBurst::Clock::now();
  QCOMPARE(tapN(b, 5, t, 200ms), 1);
  // burst reset: the next 4 do not re-fire
  QCOMPARE(tapN(b, 4, t, 200ms), 0);
}

void EscDropoutTests::fourEscDoesNotTrigger()
{
  EscTapBurst b;
  auto t = EscTapBurst::Clock::now();
  QCOMPARE(tapN(b, 4, t, 100ms), 0);
}

void EscDropoutTests::otherKeyResetsBurst()
{
  EscTapBurst b;
  auto t = EscTapBurst::Clock::now();
  QCOMPARE(tapN(b, 3, t, 100ms), 0);
  QVERIFY(!b.noteKeyDown(false, false, t)); // e.g. typing a letter
  QCOMPARE(tapN(b, 4, t, 100ms), 0);
  QCOMPARE(tapN(b, 1, t, 100ms), 1);
}

void EscDropoutTests::slowTapsDoNotTrigger()
{
  EscTapBurst b;
  auto t = EscTapBurst::Clock::now();
  QCOMPARE(tapN(b, 10, t, 400ms), 0); // 5 taps span 1.6 s > 1.5 s window
}

void EscDropoutTests::heldEscRepeatsDoNotCount()
{
  EscTapBurst b;
  auto t = EscTapBurst::Clock::now();
  QVERIFY(!b.noteKeyDown(true, false, t));
  for (int i = 0; i < 20; ++i) {
    QVERIFY(!b.noteKeyDown(true, true, t + std::chrono::milliseconds(30 * i)));
  }
}

void EscDropoutTests::deadlineParse()
{
  using deskflow::coordination::dropoutSecondsLeft;
  QCOMPARE(dropoutSecondsLeft("1000\n", 940), int64_t{60});
  QCOMPARE(dropoutSecondsLeft("1000", 1000), int64_t{0});
  QCOMPARE(dropoutSecondsLeft("garbage", 0), int64_t{0});
  QCOMPARE(dropoutSecondsLeft("999999", 0), int64_t{75}); // clamped
}

QTEST_MAIN(EscDropoutTests)
