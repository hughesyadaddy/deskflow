/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "coordination/Coordinator.h"
#include "coordination/ElectionState.h"

#include <QString>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

class EventQueue;
class QThread;

//! Rate limiter for auto-mode epoch flips (pure logic, no threads).
/*!
Every role flip tears down and rebuilds a ServerApp/ClientApp, including
the platform screen and event tap. A flapping peer can request a flip
every second; this gate guarantees a minimum dwell time per epoch and
coalesces every flip request that lands inside that window into a single
deferred interrupt. Hysteresis: when an epoch was cut short by a deferred
flip the dwell doubles (up to \c maxDwell, 3x the base: the election role
and the running app diverge for the whole dwell, so the ceiling bounds
how long a machine can be "server by election, client by app"); a quiet
epoch resets it.

Time is injected so the policy is unit-testable; AutoModeRunner drives it
with std::chrono::steady_clock.
*/
class EpochFlipGate
{
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;
  using Duration = std::chrono::milliseconds;

  struct Config
  {
    Duration minDwell{5000};
    Duration maxDwell{15000};
  };

  enum class Action
  {
    Ignored,      //!< no epoch is running; the loop picks the decision up
    InterruptNow, //!< dwell satisfied: interrupt the running app now
    Deferred,     //!< first request inside the dwell window: interrupt at deadline
    Coalesced     //!< a deferred interrupt is already armed; nothing to do
  };

  EpochFlipGate() : EpochFlipGate(Config{})
  {
  }

  explicit EpochFlipGate(Config config) : m_config(config), m_dwell(config.minDwell)
  {
    m_config.maxDwell = std::max(m_config.minDwell, m_config.maxDwell);
  }

  //! An app epoch is about to run its event loop.
  void epochStarted(TimePoint now)
  {
    m_running = true;
    m_start = now;
    m_deferred.reset();
    m_flipInsideDwell = false;
  }

  //! The app epoch returned (any path). Updates hysteresis.
  void epochEnded(TimePoint /*now*/)
  {
    m_running = false;
    m_deferred.reset();
    if (m_flipInsideDwell) {
      m_dwell = std::min(m_config.maxDwell, m_dwell * 2);
    } else {
      m_dwell = m_config.minDwell;
    }
    m_flipInsideDwell = false;
  }

  //! A new role decision arrived. Returns what the caller must do.
  Action requestFlip(TimePoint now)
  {
    if (!m_running) {
      return Action::Ignored;
    }
    if (m_deferred) {
      ++m_coalesced;
      return Action::Coalesced;
    }
    if (now - m_start >= m_dwell) {
      return Action::InterruptNow;
    }
    m_flipInsideDwell = true;
    m_deferred = m_start + m_dwell;
    return Action::Deferred;
  }

  //! Deadline of the armed deferred interrupt, if any.
  std::optional<TimePoint> deferredDeadline() const
  {
    return m_deferred;
  }

  //! Disarm and report whether the deferred interrupt is due at \p now.
  bool takeDeferredIfDue(TimePoint now)
  {
    if (!m_deferred || now < *m_deferred) {
      return false;
    }
    m_deferred.reset();
    return true;
  }

  Duration currentDwell() const
  {
    return m_dwell;
  }

  bool epochRunning() const
  {
    return m_running;
  }

  //! Flip requests absorbed into an already-armed deferred interrupt.
  int coalescedRequests() const
  {
    return m_coalesced;
  }

private:
  Config m_config;
  Duration m_dwell;
  TimePoint m_start{};
  std::optional<TimePoint> m_deferred;
  bool m_running = false;
  bool m_flipInsideDwell = false;
  int m_coalesced = 0;
};

//! Runs deskflow-core in "auto" mode: the coordination epoch loop.
/*!
Owns a Coordinator for the process lifetime and repeatedly constructs,
runs, and destroys a ServerApp or ClientApp per elected role (see
docs/coordination/design.md). One process, one TCC identity, roles flip
in place. Flips are rate-limited by EpochFlipGate: a decision that lands
inside the dwell window is consumed by a deferred timer, and when the
latest decision still names the role that is already running the epoch
is kept (no rebuild at all, unless the decision asks for a restart). A
decision that landed between epochs (the app build gap) was never gated
and interrupts the new epoch at once.
*/
class AutoModeRunner
{
public:
  AutoModeRunner(EventQueue &events, QString processName);
  AutoModeRunner(const AutoModeRunner &) = delete;
  AutoModeRunner &operator=(const AutoModeRunner &) = delete;
  ~AutoModeRunner();

  //! Start the epoch loop on \p coreThread (mirrors App::run()).
  void run(QThread &coreThread);

  //! Graceful shutdown (wired to the IPC stop request).
  void requestQuit();

  int exitCode() const
  {
    return m_exitCode;
  }

  //! Coordinator for read-only health reads (null until the epoch loop has
  //! started it; the object outlives every reader on the main thread).
  const deskflow::coordination::Coordinator *healthCoordinator() const
  {
    return m_healthCoordinator.load();
  }

  //! App epochs started since launch.
  int epochCount() const
  {
    return m_epochCount.load();
  }

  //! Backoff after an epoch ends with a failure code (hot-loop guard).
  static constexpr std::chrono::milliseconds kFailureBackoff{500};

  //! Hysteresis ceiling as a multiple of the base dwell (5 s -> 15 s).
  static constexpr int kMaxDwellMultiplier = 3;

  //! Dwell configuration: DESKFLOW_AUTO_DWELL_MS overrides the 5 s default.
  static EpochFlipGate::Config gateConfigFromEnvironment();

  //! Does \p decision name the epoch that is already running?
  /*!
  True when the loop may drop the decision and keep the running app: same
  role, same server address, not a quit, and not a forced restart (a
  wedged server re-decides its own role precisely to be rebuilt). Pure so
  the epoch-loop tests exercise the production comparison.
  */
  static bool keepsRunningEpoch(
      const deskflow::coordination::RoleDecision &decision, deskflow::coordination::Role runningRole,
      const std::string &runningServer
  )
  {
    return !decision.quit && !decision.restart && decision.role == runningRole &&
           decision.serverAddress == runningServer;
  }

private:
  void epochLoop();
  int runEpoch(deskflow::coordination::Role role, const std::string &serverAddress);

  //! Coordinator interrupt callback: route a decision through the gate.
  void onFlipRequested();
  //! Deferred-interrupt timer body; exits when m_gateStop is set.
  void deferredInterruptThread();
  //! Consume the coordinator's pending decision while an app is running.
  /*!
  Returns true when the running app must be interrupted. When the latest
  decision names the role/address already running (see keepsRunningEpoch),
  the decision is dropped and the epoch continues (lock held by caller).
  */
  bool consumePendingDecisionLocked();
  //! Next decision for the loop: the one the timer consumed, else await.
  deskflow::coordination::RoleDecision takeDecision();
  //! Stop and join the deferred-interrupt thread (idempotent).
  void stopDeferredThread();

  EventQueue &m_events;
  QString m_processName;
  std::unique_ptr<deskflow::coordination::Coordinator> m_coordinator;
  std::atomic<bool> m_appRunning{false};
  std::atomic<int> m_exitCode{0};
  std::atomic<bool> m_quitRequested{false};
  std::atomic<const deskflow::coordination::Coordinator *> m_healthCoordinator{nullptr};
  std::atomic<int> m_epochCount{0};

  // Everything below is guarded by m_gateMutex.
  std::mutex m_gateMutex;
  std::condition_variable m_gateCv;
  EpochFlipGate m_gate;
  bool m_gateStop = false;
  deskflow::coordination::Role m_runningRole = deskflow::coordination::Role::Init;
  std::string m_runningServer;
  std::unique_ptr<deskflow::coordination::RoleDecision> m_consumedDecision;
  std::thread m_deferredThread;
};
