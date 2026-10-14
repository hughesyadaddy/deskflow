/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace deskflow::platform {

//! 'DSKR' in kCGEventSourceUserData: a primary modifier-resync post. Counts
//! as injected everywhere (see isInjectedEvent in OSXInjectedEvent.h).
inline constexpr int64_t kModifierResyncMarker = 0x44534B52;

// CGEventFlags bits (numeric so this header stays pure and testable).
inline constexpr uint64_t kFlagShift = 0x00020000;
inline constexpr uint64_t kFlagControl = 0x00040000;
inline constexpr uint64_t kFlagAlternate = 0x00080000;
inline constexpr uint64_t kFlagCommand = 0x00100000;
inline constexpr uint64_t kFlagSecondaryFn = 0x00800000; // Fn: NOT resynced (like Caps)
inline constexpr uint64_t kFlagAlphaShift = 0x00010000;  // Caps Lock: state, never touched
// NX_DEVICE* bits: LCtl 0x1, LShift 0x2, RShift 0x4, LCmd 0x8, RCmd 0x10,
// LAlt 0x20, RAlt 0x40, RCtl 0x2000.
inline constexpr uint64_t kDeviceModifierBits = 0x207F;
inline constexpr uint64_t kHeldModifierFlags =
    kFlagShift | kFlagControl | kFlagAlternate | kFlagCommand | kDeviceModifierBits;

//! Primary re-entry reconcile target. While the cursor was on another screen
//! the primary's tap swallowed every flagsChanged, so the session (what apps
//! see) can still hold a modifier the hardware released (Cmd held across the
//! crossing, released on the client -> stuck Cmd) or miss one still held.
//! HID system state is the truth for Cmd/Shift/Option/Ctrl (left+right).
//! Caps Lock and Fn are untouched. NOTE: a session-only synthetic modifier
//! (e.g. a Keyboard Maestro macro holding Cmd at that instant) is cleared too
//! -- deliberate: nothing physical holds it, and it is logged.
inline std::optional<uint64_t> primaryModifierResyncFlags(uint64_t hidFlags, uint64_t sessionFlags)
{
  if ((hidFlags & kHeldModifierFlags) == (sessionFlags & kHeldModifierFlags)) {
    return std::nullopt;
  }
  return (sessionFlags & ~kHeldModifierFlags) | (hidFlags & kHeldModifierFlags);
}

struct ModifierResyncStep
{
  uint16_t keycode; //!< kVK_* of the side-specific modifier this step moves
  bool keyDown;     //!< true iff that modifier's bit is set in HID truth
  uint64_t flags;   //!< absolute flag word after this step
};

namespace resync_detail {
struct Side
{
  uint64_t device;
  uint16_t keycode;
};
struct Family
{
  uint64_t generic;
  Side left;
  Side right;
};
// kVK_Command 55 / RightCommand 54, Shift 56 / RightShift 60,
// Option 58 / RightOption 61, Control 59 / RightControl 62
inline constexpr Family kFamilies[] = {
    {kFlagCommand, {0x0008, 55}, {0x0010, 54}},
    {kFlagShift, {0x0002, 56}, {0x0004, 60}},
    {kFlagAlternate, {0x0020, 58}, {0x0040, 61}},
    {kFlagControl, {0x0001, 59}, {0x2000, 62}},
};
} // namespace resync_detail

//! One step per changed side (left and right each get their own step when
//! both moved), each carrying the cumulative ABSOLUTE flags; the generic bit
//! of a family is taken from HID once its last side step is applied. The
//! last step's flags == primaryModifierResyncFlags(). Empty when in agreement.
inline std::vector<ModifierResyncStep> primaryModifierResyncSteps(uint64_t hidFlags, uint64_t sessionFlags)
{
  std::vector<ModifierResyncStep> steps;
  uint64_t cur = sessionFlags;
  for (const auto &fam : resync_detail::kFamilies) {
    const uint64_t famBits = fam.generic | fam.left.device | fam.right.device;
    if ((cur & famBits) == (hidFlags & famBits)) {
      continue;
    }
    const resync_detail::Side sides[] = {fam.left, fam.right};
    std::vector<resync_detail::Side> changed;
    for (const auto &side : sides) {
      if ((cur & side.device) != (hidFlags & side.device)) {
        changed.push_back(side);
      }
    }
    if (changed.empty()) {
      changed.push_back(fam.left); // generic-only disagreement
    }
    for (size_t i = 0; i < changed.size(); ++i) {
      cur = (cur & ~changed[i].device) | (hidFlags & changed[i].device);
      if (i + 1 == changed.size()) {
        cur = (cur & ~fam.generic) | (hidFlags & fam.generic);
      } else {
        // intermediate: generic follows whether any side is still down
        const bool any = (cur & (fam.left.device | fam.right.device)) != 0;
        cur = any ? (cur | fam.generic) : (cur & ~fam.generic);
      }
      const bool down = (hidFlags & changed[i].device) != 0 ||
                        (changed[i].device == fam.left.device && (hidFlags & fam.generic) != 0 &&
                         (hidFlags & (fam.left.device | fam.right.device)) == 0);
      steps.push_back({changed[i].keycode, down, cur});
    }
  }
  return steps;
}

//! Recognises the tap echo of our own IOHIDPostEvent resync posts (IOHID
//! posts cannot carry kCGEventSourceUserData). Process-wide: every tap in
//! this process (primary tap, relay monitor, local input monitor) asks it.
class ResyncEchoFilter
{
public:
  using Clock = std::chrono::steady_clock;
  static constexpr std::chrono::milliseconds kWindow{500};

  void expect(uint64_t flags, Clock::time_point now)
  {
    std::scoped_lock lock(m_mutex);
    m_pending.push_back({flags & kHeldModifierFlags, now});
    m_any.store(true, std::memory_order_release);
  }

  //! true (and consumed) if a flagsChanged with these flags is our own echo
  bool consume(uint64_t flags, Clock::time_point now)
  {
    if (!m_any.load(std::memory_order_acquire)) {
      return false;
    }
    std::scoped_lock lock(m_mutex);
    bool hit = false;
    for (auto it = m_pending.begin(); it != m_pending.end();) {
      if (now - it->at > kWindow) {
        it = m_pending.erase(it);
      } else if (!hit && it->flags == (flags & kHeldModifierFlags)) {
        it = m_pending.erase(it);
        hit = true;
      } else {
        ++it;
      }
    }
    m_any.store(!m_pending.empty(), std::memory_order_release);
    return hit;
  }

  //! non-consuming check, for secondary observers (relay/local monitors)
  bool matches(uint64_t flags, Clock::time_point now)
  {
    if (!m_any.load(std::memory_order_acquire)) {
      return false;
    }
    std::scoped_lock lock(m_mutex);
    for (const auto &p : m_pending) {
      if (now - p.at <= kWindow && p.flags == (flags & kHeldModifierFlags)) {
        return true;
      }
    }
    return false;
  }

  static ResyncEchoFilter &instance()
  {
    static ResyncEchoFilter s;
    return s;
  }

private:
  struct Pending
  {
    uint64_t flags;
    Clock::time_point at;
  };
  std::mutex m_mutex;
  std::vector<Pending> m_pending;
  std::atomic<bool> m_any{false};
};

//! Synchronous resync driver with injectable I/O (tests fake all three).
//! Before EVERY post it re-reads HID and session state and recomputes; a
//! step whose disagreement has meanwhile resolved itself (user re-pressed
//! or released) is never posted. Bounded so a session that never adopts
//! our posts cannot spin.
struct PrimaryModifierResync
{
  std::function<uint64_t()> readHid;
  std::function<uint64_t()> readSession;
  //! posts NX_FLAGSCHANGED (IOHID, kIOHIDSetGlobalEventFlags, absolute flags)
  std::function<bool(uint16_t keycode, bool keyDown, uint64_t flags)> postFlags;
  ResyncEchoFilter *echo = &ResyncEchoFilter::instance();

  //! returns the steps actually posted
  std::vector<ModifierResyncStep> run() const
  {
    std::vector<ModifierResyncStep> posted;
    std::set<uint16_t> done;
    for (int guard = 0; guard < 8; ++guard) {
      const auto steps = primaryModifierResyncSteps(readHid(), readSession());
      const ModifierResyncStep *next = nullptr;
      for (const auto &s : steps) {
        if (!done.contains(s.keycode)) {
          next = &s;
          break;
        }
      }
      if (next == nullptr) {
        break;
      }
      done.insert(next->keycode);
      if (echo != nullptr) {
        echo->expect(next->flags, ResyncEchoFilter::Clock::now());
      }
      if (postFlags(next->keycode, next->keyDown, next->flags)) {
        posted.push_back(*next);
      }
    }
    return posted;
  }
};

} // namespace deskflow::platform
