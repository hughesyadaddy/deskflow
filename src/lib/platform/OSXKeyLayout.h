/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <Carbon/Carbon.h>

namespace deskflow::platform::osx {

/// Unicode keyboard layout bytes plus keyboard type, fetched on the main queue.
struct UnicodeKeyLayoutSnapshot
{
  CFDataRef layoutBytes = nullptr;
  UInt32 keyboardType = 0;

  UnicodeKeyLayoutSnapshot() = default;
  ~UnicodeKeyLayoutSnapshot();

  UnicodeKeyLayoutSnapshot(UnicodeKeyLayoutSnapshot &&other) noexcept;
  UnicodeKeyLayoutSnapshot &operator=(UnicodeKeyLayoutSnapshot &&other) noexcept;

  UnicodeKeyLayoutSnapshot(const UnicodeKeyLayoutSnapshot &) = delete;
  UnicodeKeyLayoutSnapshot &operator=(const UnicodeKeyLayoutSnapshot &) = delete;
};

/// Returns the active keyboard layout's Unicode key layout data on the main dispatch queue.
///
/// macOS 14+ asserts the main queue inside Text Input Source (TIS) APIs. Call from CGEventTap
/// or other background threads only through this helper.
///
/// Uses `TISCopyCurrentKeyboardLayoutInputSource` (the active keyboard *layout*, not the broader
/// input source that may include IME state). Both local key mapping and fleet keyboard relay use
/// this so UCKeyTranslate output matches what the user would get when typing locally.
UnicodeKeyLayoutSnapshot copyUnicodeKeyLayoutOnMainQueue();

} // namespace deskflow::platform::osx
