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
inline constexpr uint64_t kFlagSecondaryFn = 0x00800000; // Fn: resynced only on the secondary
inline constexpr uint64_t kFlagNumericPad = 0x00200000;  // NumericPad: rides with Fn on arrows/keypad
inline constexpr uint64_t kFlagAlphaShift = 0x00010000;  // Caps Lock: state, never touched
// NX_DEVICE* bits: LCtl 0x1, LShift 0x2, RShift 0x4, LCmd 0x8, RCmd 0x10,
// LAlt 0x20, RAlt 0x40, RCtl 0x2000.
inline constexpr uint64_t kDeviceModifierBits = 0x207F;
inline constexpr uint64_t kHeldModifierFlags =
    kFlagShift | kFlagControl | kFlagAlternate | kFlagCommand | kDeviceModifierBits;

// ---------------------------------------------------------------------------
// Stray-modifier (stuck-Fn) decision logic, used only on the secondary. Pure
// and numeric so it stays testable; the stuck-Fn incident (2026-10-21) had
// macOS carry SecondaryFn in the HID/session word for 34 s while Deskflow
// held no such key, because every modifier post copied the live Fn bit
// through and re-published it (kIOHIDSetGlobalEventFlags).
// ---------------------------------------------------------------------------

//! kVK_* (numeric) for keys macOS flags with SecondaryFn while they are down:
//! the function keys F1..F20 and the navigation cluster, plus the arrows.
inline bool isFnCarryingKey(uint16_t vk)
{
  switch (vk) {
  // F1..F20 (kVK_F*). The codes are scattered, so listed explicitly.
  case 0x7A: case 0x78: case 0x63: case 0x76: case 0x60: case 0x61: case 0x62:
  case 0x64: case 0x65: case 0x6D: case 0x67: case 0x6F: case 0x69: case 0x6B:
  case 0x71: case 0x6A: case 0x40: case 0x4F: case 0x50: case 0x5A:
  // navigation cluster: Help 0x72, Home 0x73, PageUp 0x74, ForwardDelete
  // 0x75, End 0x77, PageDown 0x79.
  case 0x72: case 0x73: case 0x74: case 0x75: case 0x77: case 0x79:
  // arrows: Left 0x7B, Right 0x7C, Down 0x7D, Up 0x7E.
  case 0x7B: case 0x7C: case 0x7D: case 0x7E:
    return true;
  default:
    return false;
  }
}

//! kVK_* (numeric) for keys macOS flags with NumericPad while they are down:
//! the arrows and the keypad keys (kVK_ANSI_Keypad*).
inline bool isNumericPadKey(uint16_t vk)
{
  switch (vk) {
  // arrows (as above).
  case 0x7B: case 0x7C: case 0x7D: case 0x7E:
  // keypad digits 0..9 and operators (kVK_ANSI_Keypad*).
  case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58:
  case 0x59: case 0x5B: case 0x5C: case 0x41: case 0x43: case 0x45: case 0x47:
  case 0x4B: case 0x4C: case 0x4E: case 0x51:
    return true;
  default:
    return false;
  }
}

//! Modifier bits a stray-modifier scan watches. Caps Lock (0x10000) is
//! deliberately absent: its flag is lock state, never a held key.
inline constexpr uint64_t kStrayWatchBits =
    kFlagSecondaryFn | kFlagControl | kFlagAlternate | kFlagShift | kFlagCommand;

//! Watched bits set in \p osFlags that Deskflow cannot explain. \p explained
//! carries the bits a key is known to hold: heldModifierFlags() for
//! Shift/Ctrl/Opt/Cmd, plus Fn when an Fn-carrying key or the physical fn is
//! held. Caps Lock can never be stray (not in kStrayWatchBits).
inline uint64_t strayModifierBits(uint64_t osFlags, uint64_t explained)
{
  return (osFlags & kStrayWatchBits) & ~explained;
}

//! \p osFlags with the stray bits cleared; NumericPad is additionally cleared
//! unless a NumericPad key is held (it rides with the stray Fn on an arrow).
inline uint64_t cleanedModifierWord(uint64_t osFlags, uint64_t strayBits, bool numericPadKeyHeld)
{
  uint64_t cleaned = osFlags & ~strayBits;
  if (!numericPadKeyHeld) {
    cleaned &= ~kFlagNumericPad;
  }
  return cleaned;
}

//! Keycode to stamp on a stray-modifier resync post. Cosmetic -- the post
//! carries the absolute cleaned word -- but prefers Fn (kVK_Function) and
//! otherwise names a left modifier keycode for whichever stray bit is set.
inline uint16_t strayResyncKeycode(uint64_t strayBits)
{
  if ((strayBits & kFlagSecondaryFn) != 0) {
    return 0x3F; // kVK_Function
  }
  if ((strayBits & kFlagCommand) != 0) {
    return 55; // kVK_Command
  }
  if ((strayBits & kFlagShift) != 0) {
    return 56; // kVK_Shift
  }
  if ((strayBits & kFlagAlternate) != 0) {
    return 58; // kVK_Option
  }
  if ((strayBits & kFlagControl) != 0) {
    return 59; // kVK_Control
  }
  return 0x3F;
}

//! Two-sample dwell for the stray-modifier resync: an unexplained set must
//! survive one full sample unchanged before we act, so a single transient
//! sample (a chord mid-press) never triggers. A different set, or a return to
//! nothing stray, restarts the dwell.
struct StrayDwell
{
  uint64_t lastStray = 0;
  bool armed = false; //!< a non-zero set has been seen at least once

  //! feed a new sample; returns true iff it should trigger a resync now
  bool sample(uint64_t stray)
  {
    if (stray == 0) {
      armed = false;
      lastStray = 0;
      return false;
    }
    const bool same = armed && stray == lastStray;
    lastStray = stray;
    armed = true;
    return same;
  }
};

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

  //! primary resync posts: match on the ordinary-modifier bits only (an IOHID
  //! echo may differ in lock/device noise). Unchanged semantics.
  void expect(uint64_t flags, Clock::time_point now)
  {
    add(flags, kHeldModifierFlags, now);
  }

  //! secondary Fn cleanup/resync posts: match on the FULL word, SecondaryFn
  //! and NumericPad included, so a physical fn edge (Fn set) within the window
  //! is not swallowed as our echo (our cleaned word has Fn cleared).
  void expectFull(uint64_t flags, Clock::time_point now)
  {
    add(flags, ~uint64_t(0), now);
  }

  //! drop one still-live expectation matching \p flags (rollback when the post
  //! that was announced failed to go out)
  void cancel(uint64_t flags, Clock::time_point now)
  {
    std::scoped_lock lock(m_mutex);
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
      if (now - it->at <= kWindow && (flags & it->mask) == it->flags) {
        m_pending.erase(it);
        break;
      }
    }
    m_any.store(!m_pending.empty(), std::memory_order_release);
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
      } else if (!hit && (flags & it->mask) == it->flags) {
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
      if (now - p.at <= kWindow && (flags & p.mask) == p.flags) {
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
    uint64_t flags; //!< expected word already reduced to \c mask
    uint64_t mask;  //!< bits compared against an observed word
    Clock::time_point at;
  };
  void add(uint64_t flags, uint64_t mask, Clock::time_point now)
  {
    std::scoped_lock lock(m_mutex);
    m_pending.push_back({flags & mask, mask, now});
    m_any.store(true, std::memory_order_release);
  }
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
