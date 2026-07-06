/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <cstdint>
#include <limits>

namespace deskflow::platform {

//! Max queued same-type move messages superseded per injection.
/*!
Bounds the drain loop in the desk thread: under a sustained flood the head
of the queue can stay a move indefinitely, and an uncapped drain would
delay the injection itself instead of bounding the backlog.
*/
inline constexpr int kMaxCoalescedMoves = 64;

//! Clamp a summed relative-move delta back to the int32 SendInput range.
/*!
Relative deltas are summed (never dropped -- they are the in-game aim
path); a pathological backlog of extreme deltas must not wrap.
*/
inline int32_t clampMoveDelta(int64_t delta)
{
  if (delta > std::numeric_limits<int32_t>::max()) {
    return std::numeric_limits<int32_t>::max();
  }
  if (delta < std::numeric_limits<int32_t>::min()) {
    return std::numeric_limits<int32_t>::min();
  }
  return static_cast<int32_t>(delta);
}

} // namespace deskflow::platform
