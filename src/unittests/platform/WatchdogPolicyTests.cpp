/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "WatchdogPolicyTests.h"

#include "common/ExitCodes.h"
#include "platform/MSWindowsWatchdog.h"

#include <QTest>

using namespace deskflow::platform::watchdog;

void WatchdogPolicyTests::isFastExit_shortUptime()
{
  QVERIFY(isFastExit(s_exitFailed, 0));
  QVERIFY(isFastExit(s_exitSuccess, 500));
  QVERIFY(isFastExit(s_exitFailed, kFastExitUptimeMs - 1));
  QVERIFY(!isFastExit(s_exitFailed, kFastExitUptimeMs));
}

void WatchdogPolicyTests::isFastExit_duplicateInstanceRegardlessOfUptime()
{
  // Exit 5 never ran the core; its lifetime says nothing about stability.
  QVERIFY(isFastExit(s_exitDuplicate, 0));
  QVERIFY(isFastExit(s_exitDuplicate, 60 * 60 * 1000));
}

void WatchdogPolicyTests::isFastExit_longUptimeCleanOrCrash()
{
  QVERIFY(!isFastExit(s_exitSuccess, 10 * 1000));
  QVERIFY(!isFastExit(s_exitFailed, 10 * 1000));
  QVERIFY(!isFastExit(s_exitTerminated, 3 * 60 * 1000));
}

void WatchdogPolicyTests::nextRestartDelay_longUptimeRestartsNow()
{
  // A core that ran for a while and died is relaunched at once (the
  // pre-existing behaviour for genuine crashes).
  QCOMPARE(nextRestartDelayMs(s_exitFailed, 0, 5 * 60 * 1000), 0);
  QCOMPARE(nextRestartDelayMs(s_exitSuccess, 0, kFastExitUptimeMs), 0);
  QCOMPARE(nextRestartDelayMs(s_exitTerminated, 0, 10 * 1000), 0);
}

void WatchdogPolicyTests::nextRestartDelay_duplicateInstanceWaits30s()
{
  // "another core owns the machine": no point retrying every second.
  QCOMPARE(kDuplicateInstanceDelayMs, 30000);
  QCOMPARE(nextRestartDelayMs(s_exitDuplicate, 1, 300), kDuplicateInstanceDelayMs);
  QCOMPARE(nextRestartDelayMs(s_exitDuplicate, 2, 300), kDuplicateInstanceDelayMs);
  // ...even if it somehow took a long time to fail.
  QCOMPARE(nextRestartDelayMs(s_exitDuplicate, 1, 10 * 1000), kDuplicateInstanceDelayMs);
}

void WatchdogPolicyTests::nextRestartDelay_fastExitsBackOffExponentially()
{
  // 1 s, 2 s, 4 s, 8 s for the 1st..4th consecutive fast exit.
  QCOMPARE(nextRestartDelayMs(s_exitFailed, 1, 100), 1000);
  QCOMPARE(nextRestartDelayMs(s_exitFailed, 2, 100), 2000);
  QCOMPARE(nextRestartDelayMs(s_exitFailed, 3, 100), 4000);
  QCOMPARE(nextRestartDelayMs(s_exitFailed, 4, 100), 8000);
  // A clean exit that comes back too fast is still a fast exit.
  QCOMPARE(nextRestartDelayMs(s_exitSuccess, 2, 100), 2000);
  // Counter of 0 (caller forgot to count this exit) behaves like the first.
  QCOMPARE(nextRestartDelayMs(s_exitFailed, 0, 100), 1000);
}

void WatchdogPolicyTests::nextRestartDelay_backoffIsCapped()
{
  QCOMPARE(kMaxBackoffMs, 30000);
  QCOMPARE(nextRestartDelayMs(s_exitFailed, 5, 100), 16000);
  QCOMPARE(nextRestartDelayMs(s_exitFailed, 6, 100), kMaxBackoffMs);
  for (int n = 1; n < 2000; ++n) {
    const int delay = nextRestartDelayMs(s_exitFailed, n, 100);
    QVERIFY(delay >= kBaseBackoffMs);
    QVERIFY(delay <= kMaxBackoffMs);
  }
}

void WatchdogPolicyTests::nextRestartDelay_neverGivesUp()
{
  // With no GUI around nothing would ever re-request a start, so a crash loop
  // keeps retrying at the cap rather than parking the core forever.
  QCOMPARE(nextRestartDelayMs(s_exitFailed, 5, 100), 16000);
  QCOMPARE(nextRestartDelayMs(s_exitFailed, 1000, 100), kMaxBackoffMs);
  QCOMPARE(nextRestartDelayMs(s_exitDuplicate, 1000, 100), kDuplicateInstanceDelayMs);
  QCOMPARE(nextRestartDelayMs(s_exitFailed, 1000, 60 * 1000), 0);
}

void WatchdogPolicyTests::nextRestartDelay_isConstexpr()
{
  // Pure function: usable at compile time, so it has no hidden state.
  static_assert(nextRestartDelayMs(s_exitFailed, 0, 100000) == 0);
  static_assert(nextRestartDelayMs(s_exitDuplicate, 1, 0) == kDuplicateInstanceDelayMs);
  static_assert(nextRestartDelayMs(s_exitFailed, 3, 0) == 4000);
  static_assert(nextRestartDelayMs(s_exitFailed, 1000, 0) == kMaxBackoffMs);
  static_assert(isFastExit(s_exitDuplicate, 1000000));
  QVERIFY(true);
}

QTEST_MAIN(WatchdogPolicyTests)
