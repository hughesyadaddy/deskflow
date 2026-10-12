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

using deskflow::coordination::EscHoldRescue;
using deskflow::coordination::ExitWatchdog;
using deskflow::coordination::RescueAction;
using deskflow::coordination::RescueSettleTimer;
using Clock = EscHoldRescue::Clock;

namespace {

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

namespace {
const auto kHold = std::chrono::milliseconds(EscHoldRescue::kHoldMs);
const auto kJustShort = std::chrono::milliseconds(EscHoldRescue::kHoldMs - 1);
} // namespace

void KeyboardRescueTests::hold_releasedBeforeDeadline_none()
{
  EscHoldRescue rescue;
  const auto t0 = Clock::now();
  rescue.noteKeyDown(kKeyEscape, 0, t0);
  QVERIFY(rescue.pending());
  QVERIFY(rescue.escDown());
  QCOMPARE(rescue.settle(t0 + kJustShort), RescueAction::None);
  rescue.noteKeyUp(kKeyEscape);
  QVERIFY(!rescue.pending());
  QVERIFY(!rescue.escDown());
  // A late poll after the release never fires: the hold ended short.
  QCOMPARE(rescue.settle(t0 + kHold * 4), RescueAction::None);
}

void KeyboardRescueTests::hold_heldPastDeadline_restartOnce()
{
  EscHoldRescue rescue;
  const auto t0 = Clock::now();
  rescue.noteKeyDown(kKeyEscape, 0, t0);
  QVERIFY(rescue.deadline() == t0 + kHold);
  QCOMPARE(rescue.settle(t0 + kJustShort), RescueAction::None);
  QVERIFY(rescue.pending());
  QCOMPARE(rescue.settle(t0 + kHold), RescueAction::Restart);
  // Fired: no second decision while the key stays down, however long.
  QVERIFY(!rescue.pending());
  QVERIFY(rescue.escDown());
  QCOMPARE(rescue.settle(t0 + kHold * 10), RescueAction::None);
  // Release and hold again: a fresh rescue.
  rescue.noteKeyUp(kKeyEscape);
  const auto t1 = t0 + kHold * 20;
  rescue.noteKeyDown(kKeyEscape, 0, t1);
  QVERIFY(rescue.pending());
  QCOMPARE(rescue.settle(t1 + kJustShort), RescueAction::None);
  QCOMPARE(rescue.settle(t1 + kHold), RescueAction::Restart);
}

void KeyboardRescueTests::hold_duplicateDownKeepsStart()
{
  // Autorepeat (or a feeder that delivers repeats) is more Downs while the
  // key is down: they neither restart the clock nor count as taps.
  EscHoldRescue rescue;
  const auto t0 = Clock::now();
  rescue.noteKeyDown(kKeyEscape, 0, t0);
  for (int i = 1; i <= 50; ++i) {
    rescue.noteKeyDown(kKeyEscape, 0, t0 + std::chrono::milliseconds(50 * i));
  }
  QVERIFY(rescue.deadline() == t0 + kHold);
  QCOMPARE(rescue.settle(t0 + kHold), RescueAction::Restart);
}

void KeyboardRescueTests::hold_chordedEscDoesNotStart()
{
  EscHoldRescue rescue;
  const auto t0 = Clock::now();
  for (const KeyModifierMask mask :
       {KeyModifierShift, KeyModifierControl, KeyModifierAlt, KeyModifierSuper,
        static_cast<KeyModifierMask>(KeyModifierShift | KeyModifierControl)}) {
    rescue.noteKeyDown(kKeyEscape, mask, t0);
    QVERIFY(!rescue.pending());
    QVERIFY(!rescue.escDown());
    QCOMPARE(rescue.settle(t0 + kHold), RescueAction::None);
    rescue.noteKeyUp(kKeyEscape);
  }
}

void KeyboardRescueTests::hold_lockModifiersStillStart()
{
  EscHoldRescue rescue;
  const auto t0 = Clock::now();
  rescue.noteKeyDown(kKeyEscape, KeyModifierCapsLock | KeyModifierNumLock, t0);
  QVERIFY(rescue.pending());
  QCOMPARE(rescue.settle(t0 + kHold), RescueAction::Restart);
}

void KeyboardRescueTests::hold_otherKeyCancels()
{
  EscHoldRescue rescue;
  const auto t0 = Clock::now();
  rescue.noteKeyDown(kKeyEscape, 0, t0);
  rescue.noteKeyDown('a', 0, t0 + std::chrono::seconds(1));
  QVERIFY(!rescue.pending());
  QVERIFY(rescue.escDown()); // still physically down, just not a rescue
  QCOMPARE(rescue.settle(t0 + kHold), RescueAction::None);
  // Cancelled stays cancelled until Esc is released...
  rescue.noteKeyUp('a');
  QCOMPARE(rescue.settle(t0 + kHold * 2), RescueAction::None);
  // ...and the next hold starts clean.
  rescue.noteKeyUp(kKeyEscape);
  const auto t1 = t0 + kHold * 3;
  rescue.noteKeyDown(kKeyEscape, 0, t1);
  QVERIFY(rescue.pending());
  QCOMPARE(rescue.settle(t1 + kHold), RescueAction::Restart);
}

void KeyboardRescueTests::hold_modifierJoiningCancels()
{
  // Shift pressed while Esc is held: the user is chording, not rescuing.
  EscHoldRescue rescue;
  const auto t0 = Clock::now();
  rescue.noteKeyDown(kKeyEscape, 0, t0);
  rescue.noteKeyDown(kKeyShift_L, KeyModifierShift, t0 + std::chrono::milliseconds(200));
  QVERIFY(!rescue.pending());
  QCOMPARE(rescue.settle(t0 + kHold), RescueAction::None);
}

void KeyboardRescueTests::hold_otherKeyUpIgnored()
{
  // The Up of a key pressed BEFORE the hold started (typing, then Esc) is
  // not a cancellation.
  EscHoldRescue rescue;
  const auto t0 = Clock::now();
  rescue.noteKeyDown(kKeyEscape, 0, t0);
  rescue.noteKeyUp('a');
  rescue.noteKeyUp(kKeyShift_L);
  QVERIFY(rescue.pending());
  QCOMPARE(rescue.settle(t0 + kHold), RescueAction::Restart);
}

void KeyboardRescueTests::hold_resetClears()
{
  EscHoldRescue rescue;
  const auto t0 = Clock::now();
  rescue.noteKeyDown(kKeyEscape, 0, t0);
  rescue.reset();
  QVERIFY(!rescue.pending());
  QVERIFY(!rescue.escDown());
  QCOMPARE(rescue.settle(t0 + kHold), RescueAction::None);
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
