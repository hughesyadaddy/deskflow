/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <ApplicationServices/ApplicationServices.h>

#include <atomic>
#include <chrono>
#include <cstdint>

namespace deskflow::platform {

//! 'DSKF' in kCGEventSourceUserData; Mouser's tap early-returns on it.
inline constexpr int64_t kInjectedEventMarker = 0x44534B46;

inline void markInjectedEvent(CGEventRef event)
{
  CGEventSetIntegerValueField(event, kCGEventSourceUserData, kInjectedEventMarker);
}

inline bool isInjectedEvent(CGEventRef event)
{
  // 'DSKR' (kModifierResyncMarker, OSXModifierResync.h) is ours too.
  const int64_t tag = CGEventGetIntegerValueField(event, kCGEventSourceUserData);
  return tag == kInjectedEventMarker || tag == 0x44534B52;
}

//! Last key this process injected (IOHID posts carry no marker): the Esc x5
//! dropout observer sits at HID level and must not count our own posts.
inline std::atomic<int64_t> g_lastInjectedKeyMs{0};
inline std::atomic<int> g_lastInjectedKeyCode{-1};

inline int64_t injectedKeyClockMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline void noteInjectedKey(int keycode)
{
  g_lastInjectedKeyCode.store(keycode, std::memory_order_relaxed);
  g_lastInjectedKeyMs.store(injectedKeyClockMs(), std::memory_order_release);
}

inline bool wasJustInjected(int keycode, int64_t windowMs = 250)
{
  const int64_t at = g_lastInjectedKeyMs.load(std::memory_order_acquire);
  return g_lastInjectedKeyCode.load(std::memory_order_relaxed) == keycode && injectedKeyClockMs() - at < windowMs;
}

} // namespace deskflow::platform
