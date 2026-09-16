/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

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

namespace deskflow::coordination {
class Coordinator;
struct RoleDecision;
} // namespace deskflow::coordination

//! Rate limiter for auto-mode epoch flips (pure logic, no threads).
/*!
Every role flip tears down and rebuilds a ServerApp/ClientApp, including
the platform screen and event tap. A flapping peer can request a flip
every second; this gate guarantees a minimum dwell time per epoch and
coalesces every flip request that lands inside that window into a single
deferred interrupt. Hysteresis: when an epoch was cut short by a deferred
flip the dwell doubles (up to \c maxDwell); a quiet epoch resets it.

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
    Duration maxDwell{30000};
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
is kept (no rebuild at all).
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

  //! Backoff after an epoch ends with a failure code (hot-loop guard).
  static constexpr std::chrono::milliseconds kFailureBackoff{500};

  //! Dwell configuration: DESKFLOW_AUTO_DWELL_MS overrides the 5 s default.
  static EpochFlipGate::Config gateConfigFromEnvironment();

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
  decision names the role/address already running, the decision is
  dropped and the epoch continues (lock held by caller).
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
