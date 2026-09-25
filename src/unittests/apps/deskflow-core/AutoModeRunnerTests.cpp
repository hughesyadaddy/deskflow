/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "AutoModeRunnerTests.h"

#include "AutoModeRunner.h"

#include <QTest>

#include <optional>
#include <string>
#include <vector>

using deskflow::coordination::Role;
using deskflow::coordination::RoleDecision;
using Action = EpochFlipGate::Action;
using namespace std::chrono_literals;

namespace {

//! Model of the AutoModeRunner epoch loop around a real EpochFlipGate.
/*!
The "coordinator" is a single overwritten decision slot (exactly what
Coordinator::decide() does) and the "app factory" is a counter of
constructed epochs. The keep-or-rebuild comparison is the production
AutoModeRunner::keepsRunningEpoch. Time is a manual steady_clock so the
tests can walk through the dwell window deterministically.
*/
struct Harness
{
  EpochFlipGate::TimePoint now{EpochFlipGate::Clock::duration{0}};
  EpochFlipGate gate;
  std::optional<RoleDecision> pending;  // Coordinator::m_decision + m_hasDecision
  std::optional<RoleDecision> consumed; // AutoModeRunner::m_consumedDecision
  std::vector<RoleDecision> built;      // fake app factory log
  RoleDecision running;
  bool quitPosted = false;

  Harness() = default;

  explicit Harness(EpochFlipGate::Config config) : gate(config)
  {
  }

  int rebuilds() const
  {
    return static_cast<int>(built.size());
  }

  // AutoModeRunner::runEpoch() up to the app's event loop.
  void startEpoch(RoleDecision decision)
  {
    built.push_back(decision);
    running = decision;
    quitPosted = false;
    gate.epochStarted(now);
    // A decision that landed in the build gap interrupts at once.
    if (pending && consume()) {
      quitPosted = true;
      endEpoch();
    }
  }

  // AutoModeRunner::consumePendingDecisionLocked()
  bool consume()
  {
    if (!pending) {
      return false;
    }
    RoleDecision decision = *pending;
    pending.reset();
    if (AutoModeRunner::keepsRunningEpoch(decision, running.role, running.serverAddress)) {
      return false;
    }
    consumed = decision;
    return true;
  }

  // Coordinator::decide() followed by AutoModeRunner::onFlipRequested()
  Action requestFlip(Role role, std::string server = {}, bool restart = false)
  {
    pending = RoleDecision{role, std::move(server), false, restart};
    const Action action = gate.requestFlip(now);
    if (action == Action::InterruptNow && consume()) {
      quitPosted = true;
      endEpoch();
    }
    return action;
  }

  // Advance the clock; the deferred-interrupt thread fires when due.
  void advance(EpochFlipGate::Duration by)
  {
    now += by;
    if (gate.takeDeferredIfDue(now) && consume()) {
      quitPosted = true;
      endEpoch();
    }
  }

  // The app returned: epochLoop() picks the next decision.
  void endEpoch()
  {
    gate.epochEnded(now);
    if (consumed) {
      RoleDecision next = *consumed;
      consumed.reset();
      startEpoch(next);
      return;
    }
    // notifyEpochEnded(): re-arm the same role.
    startEpoch(running);
  }
};

} // namespace

void AutoModeRunnerTests::flipsWithinDwellProduceOneRebuild()
{
  Harness h;
  h.startEpoch({Role::Server, {}});
  QCOMPARE(h.rebuilds(), 1);

  h.advance(1s);
  QCOMPARE(h.requestFlip(Role::Client, "a"), Action::Deferred);
  h.advance(1s);
  QCOMPARE(h.requestFlip(Role::Server), Action::Coalesced);
  h.advance(1s);
  QCOMPARE(h.requestFlip(Role::Client, "b"), Action::Coalesced);
  h.advance(1s);
  QCOMPARE(h.requestFlip(Role::Client, "a"), Action::Coalesced);

  // Four flips, still the original epoch.
  QCOMPARE(h.rebuilds(), 1);
  QCOMPARE(h.gate.coalescedRequests(), 3);
  QVERIFY(h.gate.deferredDeadline().has_value());

  h.advance(999ms);
  QCOMPARE(h.rebuilds(), 1); // not yet due
  h.advance(1ms);
  QCOMPARE(h.rebuilds(), 2); // one rebuild for the whole window
  QCOMPARE(h.running.role, Role::Client);
  QCOMPARE(h.running.serverAddress, std::string("a"));
}

void AutoModeRunnerTests::latestRequestedRoleWins()
{
  Harness h;
  h.startEpoch({Role::Server, {}});
  h.advance(500ms);
  h.requestFlip(Role::Client, "first");
  h.advance(500ms);
  h.requestFlip(Role::Client, "second");
  h.advance(500ms);
  h.requestFlip(Role::Client, "last");

  h.advance(5s);
  QCOMPARE(h.rebuilds(), 2);
  QCOMPARE(h.built.back().serverAddress, std::string("last"));
}

void AutoModeRunnerTests::flapBackToRunningRoleKeepsEpoch()
{
  Harness h;
  h.startEpoch({Role::Client, "srv"});
  h.advance(1s);
  h.requestFlip(Role::Server);
  h.advance(1s);
  h.requestFlip(Role::Client, "srv"); // peer flapped straight back

  h.advance(10s);
  // Latest decision equals the running epoch: no teardown at all.
  QCOMPARE(h.rebuilds(), 1);
  QVERIFY(!h.pending.has_value());
  QVERIFY(!h.consumed.has_value());
  QVERIFY(!h.gate.deferredDeadline().has_value());
}

void AutoModeRunnerTests::flipAfterDwellInterruptsImmediately()
{
  Harness h;
  h.startEpoch({Role::Server, {}});
  h.advance(5s);
  QCOMPARE(h.requestFlip(Role::Client, "a"), Action::InterruptNow);
  QCOMPARE(h.rebuilds(), 2);
  QCOMPARE(h.running.role, Role::Client);

  // Exactly at the dwell boundary counts as satisfied; just before does not.
  h.advance(4999ms);
  QCOMPARE(h.requestFlip(Role::Server), Action::Deferred);
  QCOMPARE(h.rebuilds(), 2);
}

void AutoModeRunnerTests::hysteresisDoublesDwellUnderChurn()
{
  // The ceiling is 3x the base dwell: the election role and the running
  // app disagree for the whole deferred window, so 15 s is the worst case
  // a machine can be server-by-election while its ClientApp still runs.
  QCOMPARE(EpochFlipGate::Config{}.maxDwell, 15000ms);
  QCOMPARE(AutoModeRunner::kMaxDwellMultiplier, 3);

  Harness h(EpochFlipGate::Config{5000ms, 15000ms});
  h.startEpoch({Role::Server, {}});
  QCOMPARE(h.gate.currentDwell(), 5000ms);

  // Epoch 1 cut by a deferred flip -> dwell doubles.
  h.advance(1s);
  h.requestFlip(Role::Client, "a");
  h.advance(4s);
  QCOMPARE(h.rebuilds(), 2);
  QCOMPARE(h.gate.currentDwell(), 10000ms);

  // Epoch 2: a flip at +6 s is inside the 10 s window now.
  h.advance(6s);
  QCOMPARE(h.requestFlip(Role::Server), Action::Deferred);
  QCOMPARE(h.gate.deferredDeadline().value(), h.now + 4s);
  h.advance(4s);
  QCOMPARE(h.rebuilds(), 3);
  QCOMPARE(h.gate.currentDwell(), 15000ms); // 20 s capped to the ceiling

  // Stays at the ceiling, never beyond.
  h.advance(1s);
  QCOMPARE(h.requestFlip(Role::Client, "a"), Action::Deferred);
  QCOMPARE(h.gate.deferredDeadline().value(), h.now + 14s);
  h.advance(14s);
  QCOMPARE(h.gate.currentDwell(), 15000ms);
  h.advance(1s);
  h.requestFlip(Role::Server);
  h.advance(14s);
  QCOMPARE(h.gate.currentDwell(), 15000ms);
  QCOMPARE(h.rebuilds(), 5);
}

void AutoModeRunnerTests::quietEpochResetsDwell()
{
  Harness h;
  h.startEpoch({Role::Server, {}});
  h.advance(1s);
  h.requestFlip(Role::Client, "a");
  h.advance(4s);
  QCOMPARE(h.gate.currentDwell(), 10000ms);

  // A long, undisturbed epoch that flips after the window resets dwell.
  h.advance(60s);
  QCOMPARE(h.requestFlip(Role::Server), Action::InterruptNow);
  QCOMPARE(h.gate.currentDwell(), 5000ms);
}

void AutoModeRunnerTests::failureBackoffEscalatesAndGivesUp()
{
  // A failed epoch is restarted by the loop (no running epoch, so the
  // dwell gate does not apply) but never hot-looped: consecutive failures
  // back off 1 s -> 2 -> 4 -> 8 -> 16 (capped at 30 s) and the fifth one
  // gives up so launchd replaces the process. 2026-09-25: the old fixed
  // 500 ms retry rebuilt a server epoch against its own leaked listener
  // 3400 times in 30 min and never let the supervisor in.
  QCOMPARE(AutoModeRunner::kFailureBackoff, 1000ms);
  QCOMPARE(AutoModeRunner::kMaxFailureBackoff, 30000ms);
  QCOMPARE(AutoModeRunner::kMaxConsecutiveFailures, 5);

  EpochFailurePolicy policy{EpochFailurePolicy::Config{
      AutoModeRunner::kFailureBackoff, AutoModeRunner::kMaxFailureBackoff, AutoModeRunner::kMaxConsecutiveFailures
  }};
  const std::vector<std::chrono::milliseconds> expected{1000ms, 2000ms, 4000ms, 8000ms, 16000ms};
  for (int i = 0; i < 5; ++i) {
    const auto verdict = policy.epochEnded(1, 40ms);
    QCOMPARE(verdict.consecutiveFailures, i + 1);
    QCOMPARE(verdict.backoff, expected[static_cast<size_t>(i)]);
    QCOMPARE(verdict.giveUp, i + 1 >= 5);
  }

  // The cap: with a higher give-up threshold the backoff stays at 30 s.
  EpochFailurePolicy patient{EpochFailurePolicy::Config{1000ms, 30000ms, 100}};
  std::chrono::milliseconds last{0};
  for (int i = 0; i < 10; ++i) {
    last = patient.epochEnded(4, 0ms).backoff;
  }
  QCOMPARE(last, 30000ms);
  QCOMPARE(patient.consecutiveFailures(), 10);

  // The flip gate itself is untouched by failures: a failed app is
  // re-armed with the same role by the loop, which sees no running epoch
  // and therefore no rate limit.
  Harness h;
  h.startEpoch({Role::Client, "a"});
  h.advance(100ms);
  // App died on its own (no decision): re-arm the same role immediately.
  h.endEpoch();
  QCOMPARE(h.rebuilds(), 2);
  QCOMPARE(h.running.role, Role::Client);
  QCOMPARE(h.running.serverAddress, std::string("a"));
  QCOMPARE(h.gate.currentDwell(), 5000ms); // a failure is not churn

  // A decision arriving while no app runs is not gated.
  h.gate.epochEnded(h.now);
  QCOMPARE(h.gate.requestFlip(h.now), Action::Ignored);
  QVERIFY(!h.gate.deferredDeadline().has_value());
}

void AutoModeRunnerTests::failurePolicyResetsOnCleanExitAndLongRun()
{
  EpochFailurePolicy policy{EpochFailurePolicy::Config{1000ms, 30000ms, 5, 60000ms}};
  policy.epochEnded(1, 10ms);
  policy.epochEnded(1, 10ms);
  QCOMPARE(policy.consecutiveFailures(), 2);

  // A clean end (role flip, quit) wipes the streak and sleeps nothing.
  const auto clean = policy.epochEnded(0, 10ms);
  QCOMPARE(clean.backoff, 0ms);
  QVERIFY(!clean.giveUp);
  QCOMPARE(policy.consecutiveFailures(), 0);

  // A crash after a long healthy run is a fresh streak of one, never the
  // fifth strike of failures that happened hours ago.
  policy.epochEnded(1, 10ms);
  policy.epochEnded(1, 10ms);
  policy.epochEnded(1, 10ms);
  policy.epochEnded(1, 10ms);
  QCOMPARE(policy.consecutiveFailures(), 4);
  const auto afterLongRun = policy.epochEnded(1, 2h);
  QCOMPARE(afterLongRun.consecutiveFailures, 1);
  QCOMPARE(afterLongRun.backoff, 1000ms);
  QVERIFY(!afterLongRun.giveUp);
}

void AutoModeRunnerTests::deferredIsDisarmedWhenEpochEnds()
{
  Harness h;
  h.startEpoch({Role::Server, {}});
  h.advance(1s);
  h.requestFlip(Role::Client, "a");
  QVERIFY(h.gate.deferredDeadline().has_value());

  // App exits on its own before the deadline: the deferred interrupt
  // must not outlive the epoch and kill its successor.
  h.consumed.reset();
  h.pending.reset();
  h.endEpoch();
  QVERIFY(!h.gate.deferredDeadline().has_value());
  QVERIFY(!h.gate.takeDeferredIfDue(h.now + 10s));
}

void AutoModeRunnerTests::restartDecisionForRunningRoleRebuilds()
{
  // The wedge detector re-decides Server while a Server epoch runs; the
  // "same role, keep it" shortcut must not swallow that restart.
  RoleDecision same{Role::Server, {}, false, false};
  RoleDecision restart{Role::Server, {}, false, true};
  QVERIFY(AutoModeRunner::keepsRunningEpoch(same, Role::Server, {}));
  QVERIFY(!AutoModeRunner::keepsRunningEpoch(restart, Role::Server, {}));
  QVERIFY(!AutoModeRunner::keepsRunningEpoch(RoleDecision{Role::Server, {}, true, false}, Role::Server, {}));

  // After the dwell: interrupts and rebuilds the very same role at once.
  Harness h;
  h.startEpoch({Role::Server, {}});
  h.advance(6s);
  QCOMPARE(h.requestFlip(Role::Server, {}, true), Action::InterruptNow);
  QCOMPARE(h.rebuilds(), 2);
  QCOMPARE(h.running.role, Role::Server);

  // Inside the dwell: deferred like any flip, then still rebuilt.
  h.advance(1s);
  QCOMPARE(h.requestFlip(Role::Server, {}, true), Action::Deferred);
  h.advance(4s);
  QCOMPARE(h.rebuilds(), 3);
  QCOMPARE(h.running.role, Role::Server);

  // A plain same-role decision against the restarted epoch is still kept.
  h.advance(11s);
  QCOMPARE(h.requestFlip(Role::Server), Action::InterruptNow);
  QCOMPARE(h.rebuilds(), 3);
}

void AutoModeRunnerTests::decisionInBuildGapInterruptsImmediately()
{
  // A decision made while no epoch ran (between takeDecision() and the
  // app's event loop) was never rate-limited: the fresh epoch yields to
  // it at once instead of a full dwell later.
  Harness h;
  h.pending = RoleDecision{Role::Client, "a", false, false};
  h.startEpoch({Role::Server, {}});
  QCOMPARE(h.rebuilds(), 2);
  QCOMPARE(h.running.role, Role::Client);
  QCOMPARE(h.running.serverAddress, std::string("a"));
  QVERIFY(!h.pending.has_value());
  QVERIFY(!h.gate.deferredDeadline().has_value());
  QCOMPARE(h.gate.currentDwell(), 5000ms); // not counted as churn

  // The same role landing in the gap is dropped, never a hot loop.
  h.pending = RoleDecision{Role::Client, "a", false, false};
  h.endEpoch();
  QCOMPARE(h.rebuilds(), 3);
  QVERIFY(!h.pending.has_value());
  QCOMPARE(h.running.role, Role::Client);
}

QTEST_MAIN(AutoModeRunnerTests)
