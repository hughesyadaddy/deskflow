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
A burst is a run of plain Esc downs. It ENDS after kSettleMs of silence,
and only then is it decided: fewer than kRestartTaps → nothing,
kRestartTaps..kStopAllTaps-1 → Restart, kStopAllTaps or more → StopAll.
Nothing fires at the 5th press any more, so a user heading for 10 never
triggers a restart on the way.

There is ONE gap constant: a press that lands kSettleMs or later after the
previous one belongs to a new burst, whether or not the settle poll ran in
between (the poll may be late; the press then closes the stale burst and
returns its decision so it is never lost). So the effective join gap is
exactly kSettleMs (700 ms), on the timer and on a late press alike.
*/
class RescueBurst
{
public:
  static constexpr int kRestartTaps = 5;
  static constexpr int kStopAllTaps = 10;
  //! Silence after the last press that ends a burst: decide() is due, and
  //! a press this late starts a new burst.
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
The Coordinator observes Esc on the platform input thread, which may never
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

//! Exits the process unless cancelled before the deadline (one thread).
/*!
Every graceful quit or restart request goes through the core event loop
sooner or later; a wedged loop would swallow it and keep a dead seat
alive. Arm this next to the request: if the loop does not acknowledge in
time the watchdog hard-exits (\c _exit, no static destructors that could
block on the wedge) and launchd KeepAlive / the Windows daemon relaunch
the core. The exit function is injectable for tests.
*/
class ExitWatchdog
{
public:
  using Clock = std::chrono::steady_clock;
  using ExitFn = std::function<void(int code, const std::string &reason)>;

  //! \p exitFn defaults to hardExit().
  explicit ExitWatchdog(ExitFn exitFn = {});
  ExitWatchdog(const ExitWatchdog &) = delete;
  ExitWatchdog &operator=(const ExitWatchdog &) = delete;
  ~ExitWatchdog();

  //! (Re)arm: exit with \p code once \p delay passes without cancel().
  void arm(std::chrono::milliseconds delay, int code, std::string reason);
  void cancel();
  bool armed() const;

private:
  void loop();

  ExitFn m_exitFn;
  mutable std::mutex m_mutex;
  std::condition_variable m_wake;
  std::optional<Clock::time_point> m_deadline;
  int m_code = 0;
  std::string m_reason;
  bool m_stop = false;
  std::thread m_thread;
};

//! Log \p reason and _exit(code) -- from a second thread too, so a logger
//! blocked on the very wedge being escaped cannot keep the process alive.
[[noreturn]] void hardExit(int code, const std::string &reason);

//! Unconditional bounded fallback: _exit(code) after \p delay on a detached
//! thread (the process is supposed to be gone by then). Used after every
//! stop-all quit request.
void armProcessExitFallback(std::chrono::milliseconds delay, int code, std::string reason);
//! Tests only: replace the process exit used by armProcessExitFallback.
using ProcessExitFn = std::function<void(int code, const std::string &reason)>;
void setProcessExitHandlerForTests(ProcessExitFn fn);

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
//! the seat name for logging. Runs on a detached thread and must never
//! depend on the core event loop.
using LocalStopAllFn = std::function<void(const std::string &seat)>;
void setLocalStopAllHandler(LocalStopAllFn fn);

//! Stop everything on THIS seat. Logs the WARNING line first, then runs the
//! registered executor once; repeats while a stop is in progress are
//! ignored (a seat that is already stopping never loops). Returns false
//! when nothing could be started (no executor): the caller may retry on
//! the next burst.
bool requestLocalStopAll(const std::string &seat);
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
