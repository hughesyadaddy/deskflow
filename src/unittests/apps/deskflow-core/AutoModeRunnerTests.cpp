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
using Action = EpochFlipGate::Action;
using namespace std::chrono_literals;

namespace {

struct Decision
{
  Role role = Role::Init;
  std::string server;
  bool operator==(const Decision &other) const
  {
    return role == other.role && server == other.server;
  }
};

//! Model of the AutoModeRunner epoch loop around a real EpochFlipGate.
/*!
The "coordinator" is a single overwritten decision slot (exactly what
Coordinator::decide() does) and the "app factory" is a counter of
constructed epochs. Time is a manual steady_clock so the tests can
walk through the dwell window deterministically.
*/
struct Harness
{
  EpochFlipGate::TimePoint now{EpochFlipGate::Clock::duration{0}};
  EpochFlipGate gate;
  std::optional<Decision> pending;  // Coordinator::m_decision + m_hasDecision
  std::optional<Decision> consumed; // AutoModeRunner::m_consumedDecision
  std::vector<Decision> built;      // fake app factory log
  Decision running;
  bool quitPosted = false;

  Harness() = default;

  explicit Harness(EpochFlipGate::Config config) : gate(config)
  {
  }

  int rebuilds() const
  {
    return static_cast<int>(built.size());
  }

  void startEpoch(Decision decision)
  {
    built.push_back(decision);
    running = decision;
    quitPosted = false;
    gate.epochStarted(now);
  }

  // AutoModeRunner::consumePendingDecisionLocked()
  bool consume()
  {
    if (!pending) {
      return false;
    }
    Decision decision = *pending;
    pending.reset();
    if (decision == running) {
      return false;
    }
    consumed = decision;
    return true;
  }

  // Coordinator::decide() followed by AutoModeRunner::onFlipRequested()
  Action requestFlip(Role role, std::string server = {})
  {
    pending = Decision{role, std::move(server)};
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
      Decision next = *consumed;
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
  QCOMPARE(h.running.server, std::string("a"));
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
  QCOMPARE(h.built.back().server, std::string("last"));
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
  Harness h(EpochFlipGate::Config{5000ms, 30000ms});
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
  QCOMPARE(h.gate.currentDwell(), 20000ms);

  // Keeps doubling up to the ceiling, never beyond.
  h.advance(1s);
  h.requestFlip(Role::Client, "a");
  h.advance(19s);
  QCOMPARE(h.gate.currentDwell(), 30000ms);
  h.advance(1s);
  h.requestFlip(Role::Server);
  h.advance(29s);
  QCOMPARE(h.gate.currentDwell(), 30000ms);
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

void AutoModeRunnerTests::failureBackoffUnchanged()
{
  // The hot-loop guard for failed epochs is a fixed 500 ms sleep and is
  // not subject to the dwell gate: a failed app is restarted by the
  // loop, which sees no running epoch and therefore no rate limit.
  QCOMPARE(AutoModeRunner::kFailureBackoff, 500ms);

  Harness h;
  h.startEpoch({Role::Client, "a"});
  h.advance(100ms);
  // App died on its own (no decision): re-arm the same role immediately.
  h.endEpoch();
  QCOMPARE(h.rebuilds(), 2);
  QCOMPARE(h.running, (Decision{Role::Client, "a"}));
  QCOMPARE(h.gate.currentDwell(), 5000ms); // a failure is not churn

  // A decision arriving while no app runs is not gated.
  h.gate.epochEnded(h.now);
  QCOMPARE(h.gate.requestFlip(h.now), Action::Ignored);
  QVERIFY(!h.gate.deferredDeadline().has_value());
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

QTEST_MAIN(AutoModeRunnerTests)
