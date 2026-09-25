/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/KeyTypes.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace deskflow::coordination {

//! What an Esc burst asks for once it has ended.
enum class RescueAction
{
  None,
  //! 5..9 taps: every seat restarts its local core (the classic rescue).
  Restart,
  //! 10+ taps: every seat stops every Deskflow instance and service.
  StopAll
};

const char *rescueActionName(RescueAction action);

//! Pure Esc-burst counter with an injected millisecond clock.
/*!
A burst is a run of plain Esc downs where each press lands at most
kMaxGapMs after the previous one. The decision is made only when the burst
has ENDED (no Esc for kSettleMs): fewer than kRestartTaps → nothing,
kRestartTaps..kStopAllTaps-1 → Restart, kStopAllTaps or more → StopAll.
Nothing fires at the 5th press any more, so a user heading for 10 never
triggers a restart on the way.

Two entry points consume a finished burst: decide() (the settle timer) and
observeEsc() (a late press that finds the previous burst already stale --
the poll may be late, the decision must not be lost). Whichever runs first
owns the decision; the other sees None.
*/
class RescueBurst
{
public:
  static constexpr int kRestartTaps = 5;
  static constexpr int kStopAllTaps = 10;
  //! A press later than this after the previous one starts a NEW burst.
  static constexpr int64_t kMaxGapMs = 800;
  //! Silence after the last press that ends a burst (decide() is due).
  static constexpr int64_t kSettleMs = 700;

  //! A plain Esc down at \p nowMs. Returns the decision of a previous burst
  //! this press found stale (usually None).
  RescueAction observeEsc(int64_t nowMs);

  //! Settle poll: the decision when the burst has ended, else None.
  RescueAction decide(int64_t nowMs);

  //! A non-Esc (or chord-modified Esc) press: the streak is abandoned.
  void breakBurst();

  //! Presses in the current (open) burst.
  int count() const;
  //! True while a burst is open and a decide() is still due.
  bool pending() const;
  //! When decide() becomes due (valid while pending()).
  int64_t deadlineMs() const;

  //! Mapping from burst length to action.
  static RescueAction actionFor(int count);

private:
  int m_count = 0;
  int64_t m_lastMs = 0;
};

//! Key-event adapter over RescueBurst (steady clock, modifier filtering).
/*!
Call only for KeyDown / KeyPhase::Down (not Repeat). Chord modifiers
(Shift/Ctrl/Alt/Super) break the streak; Caps/NumLock are ignored.
*/
struct EscTapRescue
{
  using Clock = std::chrono::steady_clock;

  //! Feed a key down; returns the decision of a stale burst this press
  //! closed (usually None). The caller decides swallowing from count().
  RescueAction noteKeyDown(KeyID id, KeyModifierMask mask, Clock::time_point now = Clock::now());

  //! Settle poll (see RescueBurst::decide).
  RescueAction settle(Clock::time_point now = Clock::now());

  //! A burst of at least kRestartTaps is a rescue in progress: the Esc that
  //! completes it and every later one in the burst are swallowed.
  bool swallowing() const;

  bool pending() const;
  Clock::time_point deadline() const;
  int count() const;
  void reset();

  static int64_t toMs(Clock::time_point t);

private:
  RescueBurst m_burst;
  //! Clock origin so the burst sees small non-negative ms values.
  std::optional<Clock::time_point> m_origin;
};

//! Fires a callback once a (re-armable) deadline passes; one worker thread.
/*!
The Coordinator observes Esc inside the OS keyboard hook, which may never
run again after the last tap; something has to wake up kSettleMs later.
The callback runs on the timer thread and must not block for long.
*/
class RescueSettleTimer
{
public:
  using Clock = std::chrono::steady_clock;

  explicit RescueSettleTimer(std::function<void()> onDue);
  RescueSettleTimer(const RescueSettleTimer &) = delete;
  RescueSettleTimer &operator=(const RescueSettleTimer &) = delete;
  ~RescueSettleTimer();

  //! (Re)arm: the callback fires once \p deadline passes (a later arm
  //! replaces an earlier deadline).
  void arm(Clock::time_point deadline);
  void cancel();
  bool armed() const;

private:
  void loop();

  std::function<void()> m_onDue;
  mutable std::mutex m_mutex;
  std::condition_variable m_wake;
  std::optional<Clock::time_point> m_deadline;
  bool m_stop = false;
  std::thread m_thread;
};

//! Process-wide soft-restart hook (set by deskflow-core to call Core IPC).
using LocalCoreRestartFn = void (*)();
void setLocalCoreRestartHandler(LocalCoreRestartFn fn);
void requestLocalCoreRestart();

//! Process-wide FLEET rescue hook (set by the Coordinator, which owns the
//! mesh). The 5-Esc gesture means "input is wedged somewhere in the fleet",
//! and the machine that sees the taps is usually NOT the broken one -- so
//! the rescue restarts every connected peer, not just this process.
//! Falls back to a local-only restart when no mesh is running.
using FleetRescueFn = void (*)();
void setFleetRescueHandler(FleetRescueFn fn);
void requestFleetRescue();

//! Process-wide FLEET stop-all hook (Coordinator): 10x Esc stops every
//! Deskflow instance and service on every seat. Falls back to a local-only
//! stop when no mesh is running.
using FleetStopAllFn = void (*)();
void setFleetStopAllHandler(FleetStopAllFn fn);
void requestFleetStopAll();

//! Local stop-all executor (set by deskflow-core: the platform sequence
//! that stops this seat's GUI, services and finally this core). Receives
//! the seat name for logging. Runs on a detached thread.
using LocalStopAllFn = std::function<void(const std::string &seat)>;
void setLocalStopAllHandler(LocalStopAllFn fn);

//! Stop everything on THIS seat. Logs the WARNING line first, then runs the
//! registered executor once; repeats while a stop is in progress are
//! ignored (a seat that is already stopping never loops).
void requestLocalStopAll(const std::string &seat);
bool stopAllInProgress();
//! Tests only: forget an in-progress stop so the next request runs again.
void resetStopAllStateForTests();

//! Graceful "exit this core process" (set by deskflow-core: the auto-mode
//! runner's requestQuit or App::quit). Guarded so a late caller after
//! deskflow-core cleared it (process unwinding) is a no-op.
using LocalCoreQuitFn = std::function<void()>;
void setLocalCoreQuitHandler(LocalCoreQuitFn fn);
void requestLocalCoreQuit();

} // namespace deskflow::coordination
