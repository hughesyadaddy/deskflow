/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/KeyboardRescue.h"

#include "base/Log.h"

#include <atomic>
#include <utility>

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
// RescueBurst
//

RescueAction RescueBurst::actionFor(int count)
{
  if (count >= kStopAllTaps) {
    return RescueAction::StopAll;
  }
  if (count >= kRestartTaps) {
    return RescueAction::Restart;
  }
  return RescueAction::None;
}

RescueAction RescueBurst::observeEsc(int64_t nowMs)
{
  RescueAction closed = RescueAction::None;
  if (m_count > 0 && nowMs - m_lastMs > kMaxGapMs) {
    // The previous burst ended without a settle poll (late timer): its
    // decision belongs to it, not to the press that starts the next one.
    closed = actionFor(m_count);
    m_count = 0;
  }
  ++m_count;
  m_lastMs = nowMs;
  return closed;
}

RescueAction RescueBurst::decide(int64_t nowMs)
{
  if (m_count == 0 || nowMs - m_lastMs < kSettleMs) {
    return RescueAction::None;
  }
  const RescueAction action = actionFor(m_count);
  m_count = 0;
  return action;
}

void RescueBurst::breakBurst()
{
  m_count = 0;
}

int RescueBurst::count() const
{
  return m_count;
}

bool RescueBurst::pending() const
{
  return m_count > 0;
}

int64_t RescueBurst::deadlineMs() const
{
  return m_lastMs + kSettleMs;
}

//
// EscTapRescue
//

int64_t EscTapRescue::toMs(Clock::time_point t)
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

RescueAction EscTapRescue::noteKeyDown(KeyID id, KeyModifierMask mask, Clock::time_point now)
{
  constexpr KeyModifierMask chordMods = KeyModifierShift | KeyModifierControl | KeyModifierAlt | KeyModifierSuper;
  if (id != kKeyEscape || (mask & chordMods) != 0) {
    // Non-Esc (or Esc with chord mods) breaks the streak; Caps/Num on plain Esc still count.
    m_burst.breakBurst();
    return RescueAction::None;
  }
  if (!m_origin) {
    m_origin = now;
  }
  return m_burst.observeEsc(toMs(now) - toMs(*m_origin));
}

RescueAction EscTapRescue::settle(Clock::time_point now)
{
  if (!m_origin) {
    return RescueAction::None;
  }
  return m_burst.decide(toMs(now) - toMs(*m_origin));
}

bool EscTapRescue::swallowing() const
{
  return m_burst.count() >= RescueBurst::kRestartTaps;
}

bool EscTapRescue::pending() const
{
  return m_burst.pending();
}

EscTapRescue::Clock::time_point EscTapRescue::deadline() const
{
  const auto origin = m_origin.value_or(Clock::time_point{});
  return origin + std::chrono::milliseconds(m_burst.deadlineMs());
}

int EscTapRescue::count() const
{
  return m_burst.count();
}

void EscTapRescue::reset()
{
  m_burst.breakBurst();
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
// Process-wide hooks
//

namespace {

LocalCoreRestartFn g_localCoreRestartHandler = nullptr;
FleetRescueFn g_fleetRescueHandler = nullptr;
FleetStopAllFn g_fleetStopAllHandler = nullptr;

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
  LOG_WARN("keyboard rescue: 5x Esc — no local core restart handler registered");
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

void setFleetStopAllHandler(FleetStopAllFn fn)
{
  g_fleetStopAllHandler = fn;
}

void requestFleetStopAll()
{
  if (g_fleetStopAllHandler != nullptr) {
    g_fleetStopAllHandler();
    return;
  }
  // No mesh: stop this seat alone (the gesture still means "make it stop").
  requestLocalStopAll("this seat");
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

void requestLocalStopAll(const std::string &seat)
{
  if (g_stopAllInProgress.exchange(true)) {
    LOG_INFO("[rescue] stop-all already in progress on %s; ignoring repeat", seat.c_str());
    return;
  }
  // The WARNING line goes first on every seat, before anything is touched.
  LOG_WARN("[rescue] 10x Esc: stopping ALL Deskflow instances and services on %s", seat.c_str());
  LocalStopAllFn handler;
  {
    std::scoped_lock lock{g_stopAllMutex};
    handler = g_localStopAllHandler;
  }
  if (!handler) {
    LOG_ERR("[rescue] no local stop-all executor registered on %s; nothing stopped", seat.c_str());
    g_stopAllInProgress = false;
    return;
  }
  // Off the caller's thread: this may run inside an OS input hook or the
  // core event loop, and the executor spawns processes and waits.
  std::thread([handler, seat] { handler(seat); }).detach();
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
