/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace deskflow::platform {

//! 'DSKR' in kCGEventSourceUserData: the primary's own modifier resync post.
inline constexpr int64_t kModifierResyncMarker = 0x44534B52;

// CGEventFlags bits (numeric so this header stays pure and testable).
inline constexpr uint64_t kFlagShift = 0x00020000;
inline constexpr uint64_t kFlagControl = 0x00040000;
inline constexpr uint64_t kFlagAlternate = 0x00080000;
inline constexpr uint64_t kFlagCommand = 0x00100000;
inline constexpr uint64_t kFlagSecondaryFn = 0x00800000;
inline constexpr uint64_t kFlagAlphaShift = 0x00010000; // Caps Lock: state, never touched
// NX_DEVICE* left/right bits: LCtl 0x1, LShift 0x2, RShift 0x4, LCmd 0x8,
// RCmd 0x10, LAlt 0x20, RAlt 0x40, RCtl 0x2000.
inline constexpr uint64_t kDeviceModifierBits = 0x207F;
inline constexpr uint64_t kHeldModifierFlags =
    kFlagShift | kFlagControl | kFlagAlternate | kFlagCommand | kFlagSecondaryFn | kDeviceModifierBits;

//! Primary re-entry reconcile. While the cursor was on another screen the
//! primary's event tap swallowed every flagsChanged, so the session (what
//! apps see) can still hold a modifier the hardware released (the Cmd held
//! across the crossing and released on the other screen -> stuck Cmd), or
//! miss one the user is still physically holding. HID system state is the
//! truth. Returns the flags to post to make the session match it (held
//! modifiers from HID, everything else -- Caps Lock included -- untouched),
//! or nullopt when they already agree.
inline std::optional<uint64_t> primaryModifierResyncFlags(uint64_t hidFlags, uint64_t sessionFlags)
{
  if ((hidFlags & kHeldModifierFlags) == (sessionFlags & kHeldModifierFlags)) {
    return std::nullopt;
  }
  return (sessionFlags & ~kHeldModifierFlags) | (hidFlags & kHeldModifierFlags);
}

struct ModifierResyncStep
{
  uint16_t keycode; //!< kVK_* of the modifier this flagsChanged describes
  uint64_t flags;   //!< full flags word after this step
};

//! The resync as one flagsChanged per disagreeing modifier (apps read the
//! keycode of a flagsChanged to learn WHICH modifier moved), applied in a
//! fixed order, each carrying the cumulative flags. The last step's flags
//! equal primaryModifierResyncFlags(). Empty when already in agreement.
inline std::vector<ModifierResyncStep> primaryModifierResyncSteps(uint64_t hidFlags, uint64_t sessionFlags)
{
  struct Row
  {
    uint64_t generic;
    uint64_t device;
    uint16_t keycode;
  };
  // kVK_Command 55, kVK_Shift 56, kVK_Option 58, kVK_Control 59, kVK_Function 63
  static constexpr Row kRows[] = {
      {kFlagCommand, 0x0008 | 0x0010, 55},  {kFlagShift, 0x0002 | 0x0004, 56},
      {kFlagAlternate, 0x0020 | 0x0040, 58}, {kFlagControl, 0x0001 | 0x2000, 59},
      {kFlagSecondaryFn, 0, 63},
  };
  std::vector<ModifierResyncStep> steps;
  uint64_t cur = sessionFlags;
  for (const auto &row : kRows) {
    const uint64_t bits = row.generic | row.device;
    if ((cur & bits) == (hidFlags & bits)) {
      continue;
    }
    cur = (cur & ~bits) | (hidFlags & bits);
    steps.push_back({row.keycode, cur});
  }
  // any stray device bit not owned by a row above
  if (const auto final = primaryModifierResyncFlags(hidFlags, sessionFlags); final && *final != cur) {
    steps.push_back({steps.empty() ? uint16_t{55} : steps.back().keycode, *final});
  }
  return steps;
}

} // namespace deskflow::platform
