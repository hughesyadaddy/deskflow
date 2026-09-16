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
