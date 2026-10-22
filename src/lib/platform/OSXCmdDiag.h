/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 *
 * Temporary [cmd-diag] instrumentation (Oct 19 2026). Logging only — no
 * behavior change. Remove together with the earlier [relay-diag] commit.
 * No runtime flag: always on at INFO, volume bounded by event type + rate limit.
 */
#pragma once

#include "base/Log.h"

#include <CoreGraphics/CGEventSource.h>
#include <CoreGraphics/CGEventTypes.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace deskflow::platform::cmd_diag {

inline constexpr CGEventFlags kCommand = kCGEventFlagMaskCommand;
inline constexpr CGEventFlags kSecondaryFn = kCGEventFlagMaskSecondaryFn;

// Shared boundary clocks (OSXScreen enter/leave). Steady ms; 0 = never.
struct Boundaries {
  std::atomic<int64_t> lastEnterMs{0};
  std::atomic<int64_t> lastLeaveMs{0};
  std::atomic<int> onScreen{0};  // 1 if cursor on this seat
  std::atomic<int> isPrimary{0}; // 1 if primary/server seat
};

inline Boundaries &boundaries()
{
  static Boundaries b;
  return b;
}

inline int64_t steadyMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline void noteEnter(bool primary)
{
  auto &b = boundaries();
  b.lastEnterMs.store(steadyMs(), std::memory_order_relaxed);
  b.onScreen.store(1, std::memory_order_relaxed);
  b.isPrimary.store(primary ? 1 : 0, std::memory_order_relaxed);
}

inline void noteLeave()
{
  auto &b = boundaries();
  b.lastLeaveMs.store(steadyMs(), std::memory_order_relaxed);
  b.onScreen.store(0, std::memory_order_relaxed);
}

inline bool cmdOn(CGEventFlags flags)
{
  return (flags & kCommand) != 0;
}

inline bool fnOn(CGEventFlags flags)
{
  return (flags & kSecondaryFn) != 0;
}

inline CGEventFlags readHid()
{
  return CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState);
}

inline CGEventFlags readSession()
{
  return CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState);
}

// Max ~4 disagree lines/sec globally (normal key spam while stuck).
inline bool allowDisagreeLog()
{
  static std::atomic<int64_t> lastMs{0};
  const int64_t now = steadyMs();
  int64_t prev = lastMs.load(std::memory_order_relaxed);
  if (now - prev < 250) {
    return false;
  }
  return lastMs.compare_exchange_strong(prev, now, std::memory_order_relaxed) || (now - lastMs.load() >= 250);
}

// Max ~8 flagsChanged lines/sec (chords can fire several edges quickly).
inline bool allowFlagsChangedLog()
{
  static std::atomic<int64_t> lastMs{0};
  const int64_t now = steadyMs();
  int64_t prev = lastMs.load(std::memory_order_relaxed);
  if (now - prev < 125) {
    return false;
  }
  return lastMs.compare_exchange_strong(prev, now, std::memory_order_relaxed) || (now - lastMs.load() >= 125);
}

inline void appendBoundary(char *buf, size_t buflen)
{
  const auto &b = boundaries();
  const int64_t now = steadyMs();
  const int64_t enter = b.lastEnterMs.load(std::memory_order_relaxed);
  const int64_t leave = b.lastLeaveMs.load(std::memory_order_relaxed);
  const int64_t enterAgo = enter == 0 ? -1 : (now - enter);
  const int64_t leaveAgo = leave == 0 ? -1 : (now - leave);
  snprintf(
      buf, buflen, "screen=%s on=%d enter_ago_ms=%lld leave_ago_ms=%lld", b.isPrimary.load() ? "primary" : "secondary",
      b.onScreen.load(std::memory_order_relaxed), static_cast<long long>(enterAgo), static_cast<long long>(leaveAgo)
  );
}

/*!
Log one [cmd-diag] line. \p keyLabel is a modifier vk hex ("vk=0x37") or "key"
for non-modifiers — never a character. \p stamped is flags on the posted event
(or the flagsChanged event's flags).
*/
inline void logEvent(
    const char *kind, bool shadowCmd, bool ledgerCmd, CGEventFlags osHid, CGEventFlags osSession, CGEventFlags stamped,
    const char *keyLabel
)
{
  char boundary[128];
  appendBoundary(boundary, sizeof(boundary));
  LOG_INFO(
      "[cmd-diag] %s %s shadow_cmd=%d ledger_cmd=%d os_hid_cmd=%d os_sess_cmd=%d "
      "os_hid_fn=%d os_sess_fn=%d stamped_cmd=%d os_hid=0x%llx os_sess=0x%llx stamped=0x%llx %s",
      kind, keyLabel, shadowCmd ? 1 : 0, ledgerCmd ? 1 : 0, cmdOn(osHid) ? 1 : 0, cmdOn(osSession) ? 1 : 0,
      fnOn(osHid) ? 1 : 0, fnOn(osSession) ? 1 : 0, cmdOn(stamped) ? 1 : 0, static_cast<unsigned long long>(osHid),
      static_cast<unsigned long long>(osSession), static_cast<unsigned long long>(stamped), boundary
  );
}

/*! Snapshot at enter/leave — correlates thrash with Cmd shadow/OS bits. */
inline void logBoundary(const char *kind, bool shadowCmd, bool ledgerCmd)
{
  const CGEventFlags hid = readHid();
  const CGEventFlags sess = readSession();
  logEvent(kind, shadowCmd, ledgerCmd, hid, sess, 0, "vk=boundary");
}

} // namespace deskflow::platform::cmd_diag
