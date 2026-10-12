/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/KeyboardRescue.h"

#include "base/Log.h"

#include <atomic>
#include <cstdlib>
#include <utility>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace deskflow::coordination {

const char *rescueActionName(RescueAction action)
{
  switch (action) {
  case RescueAction::Restart:
    return "restart";
  case RescueAction::StopAll:
    return "stop-all";
  default:
    return "none";
  }
}

//
// EscHoldRescue
//

void EscHoldRescue::noteKeyDown(KeyID id, KeyModifierMask mask, Clock::time_point now)
{
  constexpr KeyModifierMask chordMods = KeyModifierShift | KeyModifierControl | KeyModifierAlt | KeyModifierSuper;
  if (id == kKeyEscape && (mask & chordMods) == 0) {
    if (m_escDown) {
      return; // autorepeat / duplicate Down: the hold keeps its start time
    }
    m_escDown = true;
    m_fired = false;
    m_cancelled = false;
    m_downAt = now;
    return;
  }
  // Any other key (a chord modifier included) joining the hold: not a rescue.
  m_cancelled = true;
}

void EscHoldRescue::noteKeyUp(KeyID id)
{
  if (id == kKeyEscape) {
    m_escDown = false;
    m_fired = false;
    m_cancelled = false;
  }
}

RescueAction EscHoldRescue::settle(Clock::time_point now)
{
  if (!pending() || now - m_downAt < std::chrono::milliseconds(kHoldMs)) {
    return RescueAction::None;
  }
  m_fired = true;
  return RescueAction::Restart;
}

bool EscHoldRescue::pending() const
{
  return m_escDown && !m_fired && !m_cancelled;
}

EscHoldRescue::Clock::time_point EscHoldRescue::deadline() const
{
  return m_downAt + std::chrono::milliseconds(kHoldMs);
}

bool EscHoldRescue::escDown() const
{
  return m_escDown;
}

void EscHoldRescue::reset()
{
  m_escDown = false;
  m_fired = false;
  m_cancelled = false;
}

//
// RescueSettleTimer
//

RescueSettleTimer::RescueSettleTimer(std::function<void()> onDue) : m_onDue(std::move(onDue))
{
}

RescueSettleTimer::~RescueSettleTimer()
{
  {
    std::scoped_lock lock{m_mutex};
    m_stop = true;
  }
  m_wake.notify_all();
  if (m_thread.joinable()) {
    m_thread.join();
  }
}

void RescueSettleTimer::arm(Clock::time_point deadline)
{
  {
    std::scoped_lock lock{m_mutex};
    if (m_stop) {
      return;
    }
    m_deadline = deadline;
    // Lazily started: most processes never see a burst.
    if (!m_thread.joinable()) {
      m_thread = std::thread([this] { loop(); });
    }
  }
  m_wake.notify_all();
}

void RescueSettleTimer::cancel()
{
  {
    std::scoped_lock lock{m_mutex};
    m_deadline.reset();
  }
  m_wake.notify_all();
}

bool RescueSettleTimer::armed() const
{
  std::scoped_lock lock{m_mutex};
  return m_deadline.has_value();
}

void RescueSettleTimer::loop()
{
  std::unique_lock lock{m_mutex};
  while (!m_stop) {
    if (!m_deadline) {
      m_wake.wait(lock, [this] { return m_stop || m_deadline.has_value(); });
      continue;
    }
    const auto deadline = *m_deadline;
    if (Clock::now() < deadline) {
      // A re-arm or cancel changes the deadline; re-evaluate after the wake.
      m_wake.wait_until(lock, deadline, [this, deadline] { return m_stop || m_deadline != deadline; });
      continue;
    }
    m_deadline.reset();
    lock.unlock();
    if (m_onDue) {
      m_onDue();
    }
    lock.lock();
  }
}

//
// ExitWatchdog
//

ExitWatchdog::ExitWatchdog(ExitFn exitFn) : m_exitFn(std::move(exitFn))
{
  if (!m_exitFn) {
    m_exitFn = [](int code, const std::string &reason) { hardExit(code, reason); };
  }
}

ExitWatchdog::~ExitWatchdog()
{
  {
    std::scoped_lock lock{m_mutex};
    m_stop = true;
  }
  m_wake.notify_all();
  if (m_thread.joinable()) {
    m_thread.join();
  }
}

void ExitWatchdog::arm(std::chrono::milliseconds delay, int code, std::string reason)
{
  {
    std::scoped_lock lock{m_mutex};
    if (m_stop) {
      return;
    }
    m_deadline = Clock::now() + delay;
    m_code = code;
    m_reason = std::move(reason);
    if (!m_thread.joinable()) {
      m_thread = std::thread([this] { loop(); });
    }
  }
  m_wake.notify_all();
}

void ExitWatchdog::cancel()
{
  {
    std::scoped_lock lock{m_mutex};
    m_deadline.reset();
  }
  m_wake.notify_all();
}

bool ExitWatchdog::armed() const
{
  std::scoped_lock lock{m_mutex};
  return m_deadline.has_value();
}

void ExitWatchdog::loop()
{
  std::unique_lock lock{m_mutex};
  while (!m_stop) {
    if (!m_deadline) {
      m_wake.wait(lock, [this] { return m_stop || m_deadline.has_value(); });
      continue;
    }
    const auto deadline = *m_deadline;
    if (Clock::now() < deadline) {
      m_wake.wait_until(lock, deadline, [this, deadline] { return m_stop || m_deadline != deadline; });
      continue;
    }
    m_deadline.reset();
    const int code = m_code;
    const std::string reason = m_reason;
    lock.unlock();
    m_exitFn(code, reason);
    lock.lock();
  }
}

//
// Process exit helpers
//

namespace {

std::mutex g_processExitMutex;
ProcessExitFn g_processExitForTests;

} // namespace

void hardExit(int code, const std::string &reason)
{
  // The logger may be blocked on the very wedge being escaped: a second
  // thread exits regardless one second later.
  std::thread([code] {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    _exit(code);
  }).detach();
  LOG_ERR("[rescue] %s; exiting with %d so the supervisor relaunches this core", reason.c_str(), code);
  _exit(code);
}

void setProcessExitHandlerForTests(ProcessExitFn fn)
{
  std::scoped_lock lock{g_processExitMutex};
  g_processExitForTests = std::move(fn);
}

void armProcessExitFallback(std::chrono::milliseconds delay, int code, std::string reason)
{
  std::thread([delay, code, reason = std::move(reason)] {
    std::this_thread::sleep_for(delay);
    ProcessExitFn exitFn;
    {
      std::scoped_lock lock{g_processExitMutex};
      exitFn = g_processExitForTests;
    }
    if (exitFn) {
      exitFn(code, reason);
      return;
    }
    hardExit(code, reason);
  }).detach();
}

//
// Process-wide hooks
//

namespace {

LocalCoreRestartFn g_localCoreRestartHandler = nullptr;
FleetRescueFn g_fleetRescueHandler = nullptr;

std::mutex g_stopAllMutex;
LocalStopAllFn g_localStopAllHandler;
std::atomic<bool> g_stopAllInProgress{false};

std::mutex g_quitMutex;
LocalCoreQuitFn g_localCoreQuitHandler;

} // namespace

void setLocalCoreRestartHandler(LocalCoreRestartFn fn)
{
  g_localCoreRestartHandler = fn;
}

void requestLocalCoreRestart()
{
  if (g_localCoreRestartHandler != nullptr) {
    g_localCoreRestartHandler();
    return;
  }
  LOG_WARN("keyboard rescue: Esc held -- no local core restart handler registered");
}

void setFleetRescueHandler(FleetRescueFn fn)
{
  g_fleetRescueHandler = fn;
}

void requestFleetRescue()
{
  if (g_fleetRescueHandler != nullptr) {
    g_fleetRescueHandler();
    return;
  }
  // No mesh (server/client mode, or coordinator not started): the local
  // restart is still the best available rescue.
  requestLocalCoreRestart();
}

void setLocalStopAllHandler(LocalStopAllFn fn)
{
  std::scoped_lock lock{g_stopAllMutex};
  g_localStopAllHandler = std::move(fn);
}

bool stopAllInProgress()
{
  return g_stopAllInProgress.load();
}

void resetStopAllStateForTests()
{
  g_stopAllInProgress = false;
}

bool requestLocalStopAll(const std::string &seat)
{
  if (g_stopAllInProgress.exchange(true)) {
    LOG_INFO("[rescue] stop-all already in progress on %s; ignoring repeat", seat.c_str());
    return true;
  }
  // The WARNING line goes first on every seat, before anything is touched.
  LOG_WARN("[rescue] stop-all: stopping ALL Deskflow instances and services on %s", seat.c_str());
  LocalStopAllFn handler;
  {
    std::scoped_lock lock{g_stopAllMutex};
    handler = g_localStopAllHandler;
  }
  if (!handler) {
    LOG_ERR("[rescue] no local stop-all executor registered on %s; nothing stopped", seat.c_str());
    g_stopAllInProgress = false;
    return false;
  }
  // Off the caller's thread: this may run inside an OS input hook or the
  // core event loop, and the executor spawns processes and waits. The
  // executor never depends on the event loop (a wedged loop is the case
  // this gesture exists for).
  std::thread([handler, seat] { handler(seat); }).detach();
  return true;
}

void setLocalCoreQuitHandler(LocalCoreQuitFn fn)
{
  std::scoped_lock lock{g_quitMutex};
  g_localCoreQuitHandler = std::move(fn);
}

void requestLocalCoreQuit()
{
  // Invoked under the lock: deskflow-core clears the handler (same lock)
  // before the runner it references is destroyed, so a stop-all thread
  // that outlives the event loop can never call into a dead object.
  std::scoped_lock lock{g_quitMutex};
  if (g_localCoreQuitHandler) {
    g_localCoreQuitHandler();
    return;
  }
  LOG_DEBUG("[rescue] no local core quit handler registered (already unwinding?)");
}

} // namespace deskflow::coordination
