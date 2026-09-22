// SPDX-FileCopyrightText: (C) 2026 Deskflow Contributors
// SPDX-License-Identifier: MIT
//
// Pure, framework-free decision logic for deskflow-vhid-bridge, split out so
// it can be unit-tested without a Karabiner device, IOKit, or a login window.
//
//  * Key modifier translation (Deskflow mask -> HID modifier byte), including
//    the rule that a Caps Lock edge never carries latched modifiers.
//  * Caps Lock decision table: desired (from the server's mask) vs the target
//    machine's truth -> emit a toggle edge or skip.
//  * Relative-motion chunking: any delta is split into <= kMaxChunk counts per
//    HID report so motion stays inside the linear part of whatever residual
//    acceleration curve the OS still applies.
//  * Calibration math: emitted counts / observed points -> counts-per-point,
//    and the closed-loop residual carry used by every absolute mouse move.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

namespace bridge_logic {

// Deskflow KeyModifierMask bits (src/lib/deskflow/KeyTypes.h).
constexpr uint32_t kMaskShift = 0x0001;
constexpr uint32_t kMaskControl = 0x0002;
constexpr uint32_t kMaskAlt = 0x0004;
constexpr uint32_t kMaskMeta = 0x0008;
constexpr uint32_t kMaskSuper = 0x0010;
constexpr uint32_t kMaskCapsLock = 0x1000;

// HID boot-keyboard modifier byte bits (USB HID 1.11 §8.3). These equal the
// pqrs::karabiner hid_report::modifier enumerators; duplicated here so this
// header stays SDK-free.
constexpr uint8_t kHidLeftControl = 0x01;
constexpr uint8_t kHidLeftShift = 0x02;
constexpr uint8_t kHidLeftOption = 0x04;
constexpr uint8_t kHidLeftCommand = 0x08;
constexpr uint8_t kHidRightControl = 0x10;
constexpr uint8_t kHidRightShift = 0x20;
constexpr uint8_t kHidRightOption = 0x40;
constexpr uint8_t kHidRightCommand = 0x80;

// Deskflow KeyID and HID usage of Caps Lock.
constexpr uint16_t kKeyIdCapsLock = 0xEFE5;
constexpr uint16_t kUsageCapsLock = 0x39;

// Deskflow modifier mask -> HID modifier byte. Caps Lock is a LOCK, not a
// held modifier, so its mask bit never produces a HID modifier bit.
constexpr uint8_t mask_to_modifier_bits(uint32_t mask)
{
  uint8_t bits = 0;
  if (mask & kMaskShift)
    bits |= kHidLeftShift;
  if (mask & kMaskControl)
    bits |= kHidLeftControl;
  if (mask & kMaskAlt)
    bits |= kHidLeftOption;
  if (mask & (kMaskMeta | kMaskSuper))
    bits |= kHidLeftCommand;
  return bits;
}

// Modifier byte for the entry created by a key-down. A Caps Lock press is an
// edge with NO latched modifiers: the old code stored
// mask_to_modifier_bits(mask) on it, so Shift held at caps time stayed glued
// to the caps entry until Leave (log: id=0xefe5 mask=0x1001 -> mods=0x0002).
constexpr uint8_t key_down_modifier_bits(uint16_t key_id, uint32_t mask)
{
  if (key_id == kKeyIdCapsLock)
    return 0;
  return mask_to_modifier_bits(mask);
}

// True for KeyIDs naming an alphabetic character. Caps Lock affects ONLY
// these on the US layout, so only these need caps-aware shift handling.
constexpr bool keyid_is_letter(uint16_t key_id)
{
  return (key_id >= 'A' && key_id <= 'Z') || (key_id >= 'a' && key_id <= 'z');
}

constexpr bool keyid_is_upper_letter(uint16_t key_id)
{
  return key_id >= 'A' && key_id <= 'Z';
}

// True for KeyIDs that name a character reachable only with shift on the US
// layout: uppercase letters and the shifted symbol row/pairs. The KeyID is
// the character the server wants typed, so shift is implied even when the
// protocol modifier mask lacks it (e.g. uppercase composed via caps lock).
constexpr bool keyid_requires_shift(uint16_t key_id)
{
  if (keyid_is_upper_letter(key_id))
    return true;
  switch (key_id) {
  case '!':
  case '@':
  case '#':
  case '$':
  case '%':
  case '^':
  case '&':
  case '*':
  case '(':
  case ')':
  case '_':
  case '+':
  case '{':
  case '}':
  case '|':
  case ':':
  case '"':
  case '~':
  case '<':
  case '>':
  case '?':
    return true;
  default:
    return false;
  }
}

// Outcome of decide_letter_modifiers: the HID modifier byte to emit with the
// key-down report, the subset of it that belongs on the key's HELD ledger
// entry, and whether the target's Caps Lock must be toggled (one edge) FIRST
// so that byte composes the intended character.
/*!
modifierBits vs heldBits (K4 audit A-1/A-6): Shift that the bridge DERIVES
for a key (the case of a letter, the shifted symbol behind a KeyID) is a
property of that one key-down, not of the key while it stays held. The
bridge ORs every ledger entry into each report, so a derived Shift stored on
the ledger leaked into the next key's report -- with the server's Caps on,
rolling over `k` then `1` typed `k!` -- and survived the server releasing
its real Shift mid-repeat, so the repeated letter kept the wrong case. The
ledger entry therefore carries heldBits (the mask's real modifiers only);
the derived Shift rides on the key-down report alone, and the server's own
Shift key-down/-up entries decide the case of everything after it.
*/
struct LetterDecision
{
  uint8_t modifierBits = 0;
  uint8_t heldBits = 0;
  bool capsEdge = false;
};

// Modifier decision for a key-down.
/*!
The bridge posts raw HID reports; the usage is always the unshifted key
(`K` and `k` are both usage 0x0E) and case is decided by Shift in the
modifier byte and the TARGET's Caps Lock. macOS composes caps+shift as
LOWERCASE, so for letters the Shift decision inverts whenever caps is on.

Inputs: the KeyID (the character the server wants typed), the server's
modifier mask (its Shift bit S and its Caps Lock bit M -- the server's caps
truth at event time), and this machine's caps state when readable.

Letters:
  wantUpper = isUpper(id) || (S && !M)   -- never lowercases an uppercase
                                            KeyID; a base KeyID arriving with
                                            Shift held (Windows ToUnicodeEx
                                            fallbacks, relay-normalised masks)
                                            still uppercases; Shift+Caps
                                            composed lowercase by the server
                                            (S && M, id 'k') stays lowercase.
  capsEdge  = localCaps known && localCaps != M
                                         -- bring the target's lock in line
                                            with the server's BEFORE the key
                                            so M is the truth the byte is
                                            computed against.
  shift     = wantUpper XOR M            -- with caps on, uppercase needs no
                                            shift and lowercase needs one.
  Other mask bits (ctrl/alt/super) pass through; the mask's Shift bit itself
  is NOT propagated for letters -- `shift` replaces it.

  id  | S | M | wantUpper | shift | capsEdge (local known)
  ----+---+---+-----------+-------+-----------------------
  'K' | 0 | 0 |     1     |   1   | local != 0
  'K' | 1 | 0 |     1     |   1   | local != 0
  'K' | 0 | 1 |     1     |   0   | local != 1
  'K' | 1 | 1 |     1     |   0   | local != 1
  'k' | 0 | 0 |     0     |   0   | local != 0
  'k' | 1 | 0 |     1     |   1   | local != 0
  'k' | 0 | 1 |     0     |   1   | local != 1
  'k' | 1 | 1 |     0     |   1   | local != 1   (caps+shift+k -> 'k')

Non-letters: unchanged behaviour -- the mask's modifier bits, plus Shift
when the KeyID itself is a shifted character (keyid_requires_shift); never
a caps edge. Caps Lock's own KeyID yields no modifier bits (it is an edge,
handled by the caller) and no caps edge.
*/
constexpr LetterDecision decide_letter_modifiers(uint16_t id16, uint32_t mask32, std::optional<bool> localCaps)
{
  LetterDecision d;
  if (!keyid_is_letter(id16)) {
    d.heldBits = key_down_modifier_bits(id16, mask32);
    d.modifierBits = d.heldBits;
    if (keyid_requires_shift(id16))
      d.modifierBits |= kHidLeftShift;
    return d;
  }
  const bool serverCaps = (mask32 & kMaskCapsLock) != 0;
  const bool serverShift = (mask32 & kMaskShift) != 0;
  const bool wantUpper = keyid_is_upper_letter(id16) || (serverShift && !serverCaps);
  const bool shift = wantUpper != serverCaps;
  d.capsEdge = localCaps.has_value() && *localCaps != serverCaps;
  // The mask's own Shift is never stored on a letter: `shift` replaces it
  // for this report, and the server's Shift key has its own ledger entry.
  d.heldBits = mask_to_modifier_bits(mask32 & ~kMaskShift);
  d.modifierBits = d.heldBits;
  if (shift)
    d.modifierBits |= kHidLeftShift;
  return d;
}

// Server's intent for the caps lock state after this key event.
/*!
Deskflow's mask reflects the SERVER's caps state at event time. On the caps
key-down itself the server has already toggled, so the mask's caps bit IS the
desired post-press state. Returns the desired lock state.
*/
constexpr bool desired_caps_from_mask(uint32_t mask)
{
  return (mask & kMaskCapsLock) != 0;
}

// Caps Lock decision table.
/*!
  truth (target OS)  | desired (server) | action
  -------------------+------------------+----------------------
  unknown            | any              | emit (best effort: one edge per press)
  off                | on               | emit
  on                 | off              | emit
  off                | off              | skip (already in sync; an edge would desync)
  on                 | on               | skip
*/
constexpr bool caps_edge_needed(std::optional<bool> truth, bool desired)
{
  if (!truth)
    return true;
  return *truth != desired;
}

// Caps Lock SYNC (Enter, mask-only events): no key was pressed, so with the
// truth unknown there is nothing to approximate -- a blind edge would toggle
// the target's real lock and invert every following letter. Edge only when
// the truth is known and differs.
/*!
  truth    | desired | action
  ---------+---------+-------
  unknown  | any     | skip
  off      | on      | emit
  on       | off     | emit
  same     |         | skip
*/
constexpr bool caps_sync_edge_needed(std::optional<bool> truth, bool desired)
{
  return truth.has_value() && *truth != desired;
}

// After the bridge emits a caps edge the OS-side readers (cg-flags,
// IOHIDSystem) can lag the toggle by a few ms; a burst of letters arriving
// in one TCP read would re-read the stale state and edge again. For
// kCapsAssumeMs after an edge the bridge assumes the lock is what it just
// set it to instead of re-reading.
constexpr int64_t kCapsAssumeMs = 50;

constexpr bool caps_assumption_valid(int64_t now_ms, int64_t edge_ms, int64_t hold_ms = kCapsAssumeMs)
{
  return now_ms >= edge_ms && now_ms - edge_ms < hold_ms;
}

// Relative motion chunking. Splits a delta into steps of at most max_chunk
// counts each, preserving sign; 0 yields no steps. 400 -> 50 x 8.
constexpr int kMaxChunk = 8;

inline std::vector<int8_t> chunk_delta(int total, int max_chunk = kMaxChunk)
{
  std::vector<int8_t> steps;
  if (max_chunk <= 0 || max_chunk > 127)
    max_chunk = kMaxChunk;
  while (total != 0) {
    int step = std::clamp(total, -max_chunk, max_chunk);
    steps.push_back(static_cast<int8_t>(step));
    total -= step;
  }
  return steps;
}

// Two axes chunked in lock-step so x and y advance together (diagonal motion
// stays a straight line). Each pair is one HID report.
struct Step
{
  int8_t dx;
  int8_t dy;
};

inline std::vector<Step> chunk_delta_xy(int dx, int dy, int max_chunk = kMaxChunk)
{
  std::vector<Step> steps;
  if (max_chunk <= 0 || max_chunk > 127)
    max_chunk = kMaxChunk;
  while (dx != 0 || dy != 0) {
    int sx = std::clamp(dx, -max_chunk, max_chunk);
    int sy = std::clamp(dy, -max_chunk, max_chunk);
    steps.push_back({static_cast<int8_t>(sx), static_cast<int8_t>(sy)});
    dx -= sx;
    dy -= sy;
  }
  return steps;
}

// Calibration math.
/*!
counts_per_point = emitted HID counts / observed cursor travel in points.
Rejects a measurement where the cursor barely moved (a read that returned
the same location, a clamped edge, or a dead device) rather than producing
a wild scale. Returns nullopt when the sample is unusable.
*/
constexpr double kMinObservedPoints = 5.0;
constexpr double kMinScale = 0.25;
constexpr double kMaxScale = 64.0;

inline std::optional<double> counts_per_point(int emitted_counts, double observed_points)
{
  if (emitted_counts == 0)
    return std::nullopt;
  if (observed_points * static_cast<double>(emitted_counts) <= 0.0) // opposite sign or zero
    return std::nullopt;
  double observed = std::fabs(observed_points);
  if (observed < kMinObservedPoints)
    return std::nullopt;
  double scale = std::fabs(static_cast<double>(emitted_counts)) / observed;
  if (!(scale >= kMinScale && scale <= kMaxScale))
    return std::nullopt;
  return scale;
}

// Combines the two axis measurements (either may be unusable).
inline std::optional<double> combine_axis_scales(std::optional<double> sx, std::optional<double> sy)
{
  if (sx && sy)
    return (*sx + *sy) / 2.0;
  if (sx)
    return sx;
  return sy;
}

// Closed-loop residual carry.
/*!
After an absolute move the cursor is read back; the difference between where
the host wanted it and where it landed (in points) is carried into the next
delta. The carry is bounded so a bad read (WindowServer not up, corner
clamp, a slam) can never turn into a runaway sweep; anything beyond
kMaxResidualPoints is treated as "not a residual" and dropped.
*/
constexpr int kMaxResidualPoints = 48;
constexpr int kResidualRejectPoints = 256;

constexpr int bounded_residual(int wanted, int observed)
{
  int residual = wanted - observed;
  if (residual > kResidualRejectPoints || residual < -kResidualRejectPoints)
    return 0;
  return std::clamp(residual, -kMaxResidualPoints, kMaxResidualPoints);
}

// Host-space points -> HID counts with carry applied, rounding to nearest.
constexpr int points_to_counts(int points, double counts_per_point)
{
  double v = static_cast<double>(points) * counts_per_point;
  return static_cast<int>(v >= 0.0 ? v + 0.5 : v - 0.5);
}

} // namespace bridge_logic
