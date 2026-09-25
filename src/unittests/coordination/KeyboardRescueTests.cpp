/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "KeyboardRescueTests.h"

#include "coordination/KeyboardRescue.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

using deskflow::coordination::EscTapRescue;
using deskflow::coordination::ExitWatchdog;
using deskflow::coordination::RescueAction;
using deskflow::coordination::RescueBurst;
using deskflow::coordination::RescueSettleTimer;
using Clock = EscTapRescue::Clock;

namespace {

constexpr int64_t kSpacingMs = 100;

//! Tap Esc \p taps times, kSpacingMs apart from \p startMs. Every press
//! must report None (nothing fires on a press); returns the last press time.
int64_t tap(RescueBurst &burst, int taps, int64_t startMs = 0)
{
  int64_t at = startMs;
  for (int i = 0; i < taps; ++i) {
    at = startMs + i * kSpacingMs;
    if (burst.observeEsc(at) != RescueAction::None) {
      return -1;
    }
  }
  return at;
}

RescueAction settleAfter(RescueBurst &burst, int64_t lastMs)
{
  return burst.decide(lastMs + RescueBurst::kSettleMs);
}

template <typename Condition> bool waitFor(Condition condition, int timeoutMs)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (!condition()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

} // namespace

void KeyboardRescueTests::burst_fourTaps_none()
{
  RescueBurst burst;
  const auto last = tap(burst, 4);
  QVERIFY(last >= 0);
  QCOMPARE(burst.count(), 4);
  QCOMPARE(settleAfter(burst, last), RescueAction::None);
  QVERIFY(!burst.pending());
}

void KeyboardRescueTests::burst_fiveTaps_restart()
{
  RescueBurst burst;
  const auto last = tap(burst, 5);
  QVERIFY(last >= 0);
  QCOMPARE(settleAfter(burst, last), RescueAction::Restart);
  QCOMPARE(burst.count(), 0);
}

void KeyboardRescueTests::burst_sevenTaps_restart()
{
  RescueBurst burst;
  const auto last = tap(burst, 7);
  QVERIFY(last >= 0);
  QCOMPARE(settleAfter(burst, last), RescueAction::Restart);
}

void KeyboardRescueTests::burst_tenTaps_stop()
{
  RescueBurst burst;
  const auto last = tap(burst, 10);
  QVERIFY(last >= 0);
  QCOMPARE(settleAfter(burst, last), RescueAction::StopAll);
  QCOMPARE(burst.count(), 0);
}

void KeyboardRescueTests::burst_twelveTaps_stop()
{
  RescueBurst burst;
  const auto last = tap(burst, 12);
  QVERIFY(last >= 0);
  QCOMPARE(settleAfter(burst, last), RescueAction::StopAll);
}

void KeyboardRescueTests::burst_spacedBeyondGap_none()
{
  // Ten presses, each 900 ms after the previous: ten bursts of one.
  RescueBurst burst;
  int64_t at = 0;
  for (int i = 0; i < 10; ++i) {
    at = i * (RescueBurst::kSettleMs + 200);
    QCOMPARE(burst.observeEsc(at), RescueAction::None);
    QCOMPARE(burst.count(), 1);
  }
  QCOMPARE(settleAfter(burst, at), RescueAction::None);
}

void KeyboardRescueTests::burst_exactlyTenNeverYieldsRestart()
{
  // A user heading for 10 must not be restarted on the way: no press and
  // no early poll may ever answer Restart.
  RescueBurst burst;
  for (int i = 0; i < RescueBurst::kStopAllTaps; ++i) {
    const int64_t at = i * kSpacingMs;
    QCOMPARE(burst.observeEsc(at), RescueAction::None);
    QCOMPARE(burst.decide(at), RescueAction::None);
    QCOMPARE(burst.decide(at + RescueBurst::kSettleMs - 1), RescueAction::None);
  }
  const int64_t last = (RescueBurst::kStopAllTaps - 1) * kSpacingMs;
  QCOMPARE(burst.decide(last + RescueBurst::kSettleMs), RescueAction::StopAll);
  QCOMPARE(burst.decide(last + RescueBurst::kSettleMs + 1000), RescueAction::None);
}

void KeyboardRescueTests::burst_pauseAtFiveThenContinue_restartThenFreshCount()
{
  RescueBurst burst;
  const auto last = tap(burst, 5);
  QVERIFY(last >= 0);
  // Silence for kSettleMs: the settle poll decides the five.
  QCOMPARE(burst.decide(last + RescueBurst::kSettleMs), RescueAction::Restart);
  QVERIFY(!burst.pending());
  // The user keeps going: a fresh burst, counted from one.
  const int64_t resume = last + RescueBurst::kSettleMs + 50;
  QCOMPARE(burst.observeEsc(resume), RescueAction::None);
  QCOMPARE(burst.count(), 1);
  QCOMPARE(burst.observeEsc(resume + kSpacingMs), RescueAction::None);
  QCOMPARE(burst.count(), 2);
  QCOMPARE(settleAfter(burst, resume + kSpacingMs), RescueAction::None);
}

void KeyboardRescueTests::burst_decideBeforeSettle_none()
{
  RescueBurst burst;
  const auto last = tap(burst, 6);
  QVERIFY(last >= 0);
  QVERIFY(burst.pending());
  QCOMPARE(burst.deadlineMs(), last + RescueBurst::kSettleMs);
  QCOMPARE(burst.decide(last + RescueBurst::kSettleMs - 1), RescueAction::None);
  QCOMPARE(burst.count(), 6); // still open
  QCOMPARE(burst.decide(last + RescueBurst::kSettleMs), RescueAction::Restart);
}

void KeyboardRescueTests::burst_latePressClosesStaleBurst()
{
  // The settle poll never ran (late timer); the next press, beyond the
  // gap, still owes the previous burst its decision and starts anew.
  RescueBurst burst;
  const auto last = tap(burst, 10);
  QVERIFY(last >= 0);
  // The effective join gap is exactly kSettleMs, on the timer and on a
  // late press alike: at kSettleMs the burst is over.
  QCOMPARE(burst.observeEsc(last + RescueBurst::kSettleMs), RescueAction::StopAll);
  QCOMPARE(burst.count(), 1);
  // One ms inside the gap the press just joins the burst.
  RescueBurst joined;
  const auto joinedLast = tap(joined, 4);
  QCOMPARE(joined.observeEsc(joinedLast + RescueBurst::kSettleMs - 1), RescueAction::None);
  QCOMPARE(joined.count(), 5);
}

void KeyboardRescueTests::burst_breakAbandonsCount()
{
  RescueBurst burst;
  const auto last = tap(burst, 7);
  QVERIFY(last >= 0);
  burst.breakBurst();
  QVERIFY(!burst.pending());
  QCOMPARE(settleAfter(burst, last), RescueAction::None);
}

void KeyboardRescueTests::escTap_modifiersDoNotCount()
{
  EscTapRescue rescue;
  const auto t0 = Clock::now();
  QCOMPARE(rescue.noteKeyDown(kKeyEscape, KeyModifierControl, t0), RescueAction::None);
  QCOMPARE(rescue.count(), 0);
  QCOMPARE(rescue.noteKeyDown(kKeyEscape, KeyModifierShift | KeyModifierAlt | KeyModifierControl, t0), RescueAction::None);
  QCOMPARE(rescue.count(), 0);
  QCOMPARE(rescue.noteKeyDown('a', 0, t0), RescueAction::None);
  QCOMPARE(rescue.count(), 0);
  QVERIFY(!rescue.pending());
}

void KeyboardRescueTests::escTap_capsLockIgnored_stillCounts()
{
  EscTapRescue rescue;
  const auto t0 = Clock::now();
  QCOMPARE(rescue.noteKeyDown(kKeyEscape, KeyModifierCapsLock, t0), RescueAction::None);
  QCOMPARE(rescue.count(), 1);
  QVERIFY(rescue.pending());
  QVERIFY(rescue.deadline() == t0 + std::chrono::milliseconds(RescueBurst::kSettleMs));
}

void KeyboardRescueTests::escTap_nonEscBreaksStreak()
{
  EscTapRescue rescue;
  const auto t0 = Clock::now();
  for (int i = 0; i < 6; ++i) {
    QCOMPARE(rescue.noteKeyDown(kKeyEscape, 0, t0 + std::chrono::milliseconds(50 * i)), RescueAction::None);
  }
  QCOMPARE(rescue.count(), 6);
  QCOMPARE(rescue.noteKeyDown('a', 0, t0 + std::chrono::milliseconds(300)), RescueAction::None);
  QCOMPARE(rescue.count(), 0);
  QCOMPARE(rescue.settle(t0 + std::chrono::seconds(5)), RescueAction::None);
  // The streak restarts from one.
  QCOMPARE(rescue.noteKeyDown(kKeyEscape, 0, t0 + std::chrono::milliseconds(350)), RescueAction::None);
  QCOMPARE(rescue.count(), 1);
}

void KeyboardRescueTests::escTap_swallowsFromFifthTap()
{
  EscTapRescue rescue;
  const auto t0 = Clock::now();
  for (int i = 0; i < RescueBurst::kRestartTaps - 1; ++i) {
    rescue.noteKeyDown(kKeyEscape, 0, t0 + std::chrono::milliseconds(100 * i));
    QVERIFY(!rescue.swallowing());
  }
  rescue.noteKeyDown(kKeyEscape, 0, t0 + std::chrono::milliseconds(400));
  QVERIFY(rescue.swallowing());
  rescue.noteKeyDown(kKeyEscape, 0, t0 + std::chrono::milliseconds(500));
  QVERIFY(rescue.swallowing());
  QCOMPARE(rescue.settle(t0 + std::chrono::milliseconds(500 + RescueBurst::kSettleMs)), RescueAction::Restart);
  QVERIFY(!rescue.swallowing());
  QVERIFY(!rescue.pending());
}

void KeyboardRescueTests::settleTimer_firesOnceAfterDeadline()
{
  std::atomic<int> fired{0};
  RescueSettleTimer timer([&fired] { ++fired; });
  QVERIFY(!timer.armed());
  const auto armedAt = Clock::now();
  timer.arm(armedAt + std::chrono::milliseconds(60));
  QVERIFY(timer.armed());
  QVERIFY(waitFor([&fired] { return fired.load() == 1; }, 2000));
  QVERIFY(Clock::now() - armedAt >= std::chrono::milliseconds(60));
  QVERIFY(!timer.armed());
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  QCOMPARE(fired.load(), 1);
}

void KeyboardRescueTests::settleTimer_rearmReplacesDeadline()
{
  std::atomic<int> fired{0};
  RescueSettleTimer timer([&fired] { ++fired; });
  timer.arm(Clock::now() + std::chrono::milliseconds(40));
  timer.arm(Clock::now() + std::chrono::milliseconds(200));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  QCOMPARE(fired.load(), 0); // the earlier deadline was replaced, not kept
  QVERIFY(waitFor([&fired] { return fired.load() == 1; }, 2000));
}

void KeyboardRescueTests::settleTimer_cancelSuppresses()
{
  std::atomic<int> fired{0};
  RescueSettleTimer timer([&fired] { ++fired; });
  timer.arm(Clock::now() + std::chrono::milliseconds(40));
  timer.cancel();
  QVERIFY(!timer.armed());
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  QCOMPARE(fired.load(), 0);
}

void KeyboardRescueTests::exitWatchdog_firesAfterDelayWithCodeAndReason()
{
  std::mutex mutex;
  int code = -1;
  std::string reason;
  std::atomic<int> fired{0};
  ExitWatchdog watchdog([&](int exitCode, const std::string &why) {
    std::scoped_lock lock{mutex};
    code = exitCode;
    reason = why;
    ++fired;
  });
  QVERIFY(!watchdog.armed());
  const auto armedAt = Clock::now();
  watchdog.arm(std::chrono::milliseconds(60), 7, "loop wedged");
  QVERIFY(watchdog.armed());
  QVERIFY(waitFor([&fired] { return fired.load() == 1; }, 2000));
  QVERIFY(Clock::now() - armedAt >= std::chrono::milliseconds(60));
  QVERIFY(!watchdog.armed());
  std::scoped_lock lock{mutex};
  QCOMPARE(code, 7);
  QCOMPARE(reason, std::string("loop wedged"));
}

void KeyboardRescueTests::exitWatchdog_cancelSuppresses()
{
  std::atomic<int> fired{0};
  ExitWatchdog watchdog([&fired](int, const std::string &) { ++fired; });
  watchdog.arm(std::chrono::milliseconds(40), 1, "x");
  watchdog.cancel();
  QVERIFY(!watchdog.armed());
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  QCOMPARE(fired.load(), 0);
}

void KeyboardRescueTests::exitWatchdog_rearmReplacesDeadline()
{
  std::atomic<int> fired{0};
  ExitWatchdog watchdog([&fired](int, const std::string &) { ++fired; });
  watchdog.arm(std::chrono::milliseconds(40), 1, "x");
  watchdog.arm(std::chrono::milliseconds(200), 1, "x");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  QCOMPARE(fired.load(), 0);
  QVERIFY(waitFor([&fired] { return fired.load() == 1; }, 2000));
}

void KeyboardRescueTests::processExitFallback_usesInjectedExit()
{
  std::atomic<int> code{-1};
  deskflow::coordination::setProcessExitHandlerForTests([&code](int exitCode, const std::string &) {
    code = exitCode;
  });
  deskflow::coordination::armProcessExitFallback(std::chrono::milliseconds(20), 0, "test");
  QVERIFY(waitFor([&code] { return code.load() == 0; }, 2000));
  deskflow::coordination::setProcessExitHandlerForTests({});
}

QTEST_MAIN(KeyboardRescueTests)
