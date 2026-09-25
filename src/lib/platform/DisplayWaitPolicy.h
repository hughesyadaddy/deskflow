/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <chrono>
#include <functional>

namespace deskflow::platform {

//! Bounded retry schedule for "no valid display yet" at screen construction.
/*!
During a display hot-plug, lid open/close or wake the OS reports no active
display for a moment (macOS: CGGetActiveDisplayList returns 0 displays).
That used to be fatal for the whole app epoch (DisplayInvalidException:
"failed to initialize screen shape") -- on 2026-09-25 07:58 a promotion to
server hit exactly that window, the epoch slept 10 s, and the wedge probe
restarted it once more. Waiting for the display and only then starting the
epoch turns it into a short delay. Pure so the schedule is unit-tested;
the platform screen drives it with a real sleep.
*/
struct DisplayWaitPolicy
{
  //! Time between probes.
  std::chrono::milliseconds interval{500};
  //! Give up (and let the caller fail the epoch) after this much waiting.
  std::chrono::milliseconds maxWait{30000};
  //! Progress line cadence ("waiting for a valid display").
  std::chrono::milliseconds logEvery{5000};

  //! Probes attempted before giving up: the first one plus one per interval.
  int maxAttempts() const
  {
    return 1 + static_cast<int>(maxWait / interval);
  }
};

//! Run \p probe until it succeeds or \p policy is exhausted.
/*!
\p sleep is called with the interval between attempts; \p log receives the
elapsed wait before a retry, at most once per \c logEvery (and always
before the first retry). Returns true when a probe succeeded, false when
\c maxWait elapsed with every probe failing.
*/
inline bool waitForValidDisplay(
    const DisplayWaitPolicy &policy, const std::function<bool()> &probe,
    const std::function<void(std::chrono::milliseconds)> &sleep,
    const std::function<void(std::chrono::milliseconds elapsed)> &log
)
{
  std::chrono::milliseconds elapsed{0};
  std::chrono::milliseconds nextLog{0};
  while (true) {
    if (probe()) {
      return true;
    }
    if (elapsed >= policy.maxWait) {
      return false;
    }
    if (log && elapsed >= nextLog) {
      log(elapsed);
      nextLog = elapsed + policy.logEvery;
    }
    sleep(policy.interval);
    elapsed += policy.interval;
  }
}

} // namespace deskflow::platform
