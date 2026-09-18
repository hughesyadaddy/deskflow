/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <ApplicationServices/ApplicationServices.h>

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
  return CGEventGetIntegerValueField(event, kCGEventSourceUserData) == kInjectedEventMarker;
}

} // namespace deskflow::platform
