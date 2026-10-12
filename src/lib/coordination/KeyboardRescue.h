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

//! What the Esc hold asks for.
enum class RescueAction
{
  None,
  //! Esc held for EscHoldRescue::kHoldMs: every seat restarts its local core.
  Restart,
  //! Received over the mesh only (`stopall` line): stop every Deskflow
  //! instance and service on this seat. No keyboard gesture produces it.
  StopAll
};

const char *rescueActionName(RescueAction action);

//! Esc hold-to-rescue: a plain Esc held for kHoldMs asks for a fleet restart.
/*!
The gesture is ONE key held, not a tap count. Tap counting fired on every
Esc-happy workflow (vim, dialogs, a held Esc autorepeating through a hook
that lost the repeat flag) and the 2026-10-08 outages were all 5/12-tap
bursts the user never meant as a rescue. A 3 s hold is deliberate, cannot
be produced by autorepeat (repeats are duplicate Downs, ignored while the
key is down) and cannot be produced by relayed input (the feeders only
report physical local keys).

State machine: a plain (no Shift/Ctrl/Alt/Super) Esc Down with Esc not
already down starts the hold at that time. A duplicate Esc Down while down
is ignored. Esc Up ends the hold. Any OTHER key Down (including a modifier)
cancels it: a chord is not a rescue. settle(now) returns Restart exactly
once per hold, once now - start >= kHoldMs; holding on past that returns
None until the key is released and pressed again.

Pure; the caller injects the clock and arms its own timer at deadline().
*/
class EscHoldRescue
{
public:
  using Clock = std::chrono::steady_clock;
  static constexpr int64_t kHoldMs = 3000;

  //! A key down. Esc (plain) starts the hold; anything else cancels it.
  void noteKeyDown(KeyID id, KeyModifierMask mask, Clock::time_point now);
  //! A key up. Esc ends the hold; other keys are ignored.
  void noteKeyUp(KeyID id);
  //! Poll: Restart once the hold has lasted kHoldMs, else None.
  RescueAction settle(Clock::time_point now);

  //! A hold is in progress and has not fired yet: a settle() is due at deadline().
  bool pending() const;
  Clock::time_point deadline() const;
  //! Esc is currently down (fired or not).
  bool escDown() const;
  void reset();

private:
  bool m_escDown = false;
  bool m_fired = false;
  bool m_cancelled = false; //!< another key joined the hold
  Clock::time_point m_downAt{};
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
//! mesh). The Esc hold means "input is wedged somewhere in the fleet", and
//! the machine that sees the hold is usually NOT the broken one -- so the
//! rescue restarts every connected peer, not just this process.
//! Falls back to a local-only restart when no mesh is running.
using FleetRescueFn = void (*)();
void setFleetRescueHandler(FleetRescueFn fn);
void requestFleetRescue();

//! Local stop-all executor (set by deskflow-core: the platform sequence
//! that stops this seat's GUI, services and finally this core). Receives
//! the seat name for logging. Runs on a detached thread and must never
//! depend on the core event loop.
using LocalStopAllFn = std::function<void(const std::string &seat)>;
void setLocalStopAllHandler(LocalStopAllFn fn);

//! Stop everything on THIS seat (mesh `stopall` line). Logs the WARNING
//! line first, then runs the registered executor once; repeats while a stop is in progress are
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
