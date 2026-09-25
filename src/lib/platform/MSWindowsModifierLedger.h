/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

// Pure decision logic of the Windows injected-modifier ledger and of the
// 1 s stale-modifier audit. No Win32 dependency: this header compiles and
// is unit-tested on every platform (see ModifierLedgerTests), while
// MSWindowsKeyState / MSWindowsDesks only wire it to GetAsyncKeyState and
// SendInput.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace deskflow::platform {

//! Injected-modifier ledger: VK -> tick (ms) of the most recent injected
//! DOWN (or repeat) that has not been followed by an injected UP.
/*!
uint16_t/uint64_t are exactly WORD/ULONGLONG on Windows, so
MSWindowsKeyState::InjectedModifierMap is this type.
*/
using InjectedModifierMap = std::map<uint16_t, uint64_t>;

//! Rows of the desk-side audit table (deskSanitizeStaleModifiers'
//! kModifiers[]): LWIN RWIN LMENU RMENU LCONTROL RCONTROL. Bit i of every
//! mask below refers to row i. Shift is deliberately not a row.
inline constexpr size_t kAuditModifierRows = 6;

//! Per-row tick (ms) at which a NON-ledgered OS-down modifier was first
//! seen by the audit; 0 = not currently seen. Owned by the desk thread.
using AuditFirstSeen = std::array<uint64_t, kAuditModifierRows>;

//! Quiet window for the entered audit, WIN ROWS ONLY: a non-ledgered
//! LWIN/RWIN is not released while a key-down was injected on this screen
//! under a ledgered Super within this many ms -- another injector
//! (PowerToys Keyboard Manager, AutoHotkey) re-processing our Win+key
//! legitimately holds Win around it, and fighting it every second is
//! exactly the "constantly sticking" / Start-menu flapping seen live
//! 2026-09-25. Alt/Ctrl rows get no quiet window (see
//! staleModifiersToRelease): a stuck non-ledgered LCONTROL must be released
//! WHILE the user types, or every letter becomes Ctrl+letter for as long
//! as they keep typing.
inline constexpr uint64_t kAuditQuietMs = 2000;

//! Audit rows that the quiet window applies to: LWIN (0) and RWIN (1).
inline constexpr uint32_t kAuditQuietRows = (1u << 0) | (1u << 1);

//! D1: the ledger forgets EVERY VK whose release was attempted, whether or
//! not the OS still reported it down.
/*!
Before this, releaseInjectedKeys() erased only the VKs the desk thread
found down, and the raw audit releases (leave, boundary sweep) erased
nothing -- leaving phantom entries (seen live as "injected bits 0x03") that,
while entered, vouch forever and make the audit IGNORE a physically stuck
Win. A ledger entry exists to defend a chord WE are holding; once we have
tried to release it, it defends nothing.
*/
inline void forgetReleased(InjectedModifierMap &ledger, const std::vector<uint16_t> &attempted)
{
  for (const uint16_t vk : attempted) {
    ledger.erase(vk);
  }
}

//! D3: which audit rows to release this tick.
/*!
\p osDownMask   bit i set: the OS reports row i's VK held.
\p ledgerBits   bit i set: this client injected row i's VK and has not
                released it (it vouches; never released here).
\p firstSeen    per-row first-seen stamp, updated in place.
\p nowMs        current tick.
\p lastSuperChordMs tick of the last NON-modifier key-down injected on this
                screen while the ledger held LWIN/RWIN, i.e. the last
                Win+key we sent (0 = never). NOT the last key-down of any
                kind: see below.
\p entered      true while the server is driving this screen.

At a boundary (\p entered false) the protocol guarantees the server holds
nothing here, so every non-ledgered OS-down row is released at once (the
pre-existing contract of enable/enter/leave). While entered EVERY row needs
two consecutive non-ledgered-down sightings (this tick and an earlier one).
The LWIN/RWIN rows additionally wait until no Win+key was injected within
kAuditQuietMs: a target-side hook (PowerToys KBM, AHK) re-processing our
Win+key holds Win around it, and that is the only injector-held Win we
must not fight. The quiet window is deliberately NOT applied to the Alt/Ctrl
rows and NOT keyed to plain key-downs: a stuck non-ledgered LCONTROL/LMENU
would otherwise never be released while the user keeps typing (each letter
becoming Ctrl+letter), and a plain letter after a chord says nothing about
who is holding Win. A row that is up, or ledgered, resets its first-seen
stamp.
*/
inline uint32_t staleModifiersToRelease(
    uint32_t osDownMask, uint32_t ledgerBits, AuditFirstSeen &firstSeen, uint64_t nowMs, uint64_t lastSuperChordMs,
    bool entered
)
{
  uint32_t release = 0;
  for (size_t i = 0; i < kAuditModifierRows; ++i) {
    const uint32_t bit = 1u << i;
    const bool candidate = (osDownMask & bit) != 0 && (ledgerBits & bit) == 0;
    if (!candidate) {
      firstSeen[i] = 0;
      continue;
    }
    if (!entered) {
      release |= bit; // boundary: nothing the server holds can be here
      firstSeen[i] = 0;
      continue;
    }
    if (firstSeen[i] == 0) {
      firstSeen[i] = nowMs; // first sighting: give it a tick
      continue;
    }
    if ((bit & kAuditQuietRows) != 0 && lastSuperChordMs != 0 && nowMs - lastSuperChordMs < kAuditQuietMs) {
      continue; // Win row, Win+key just sent: a hook may still hold Win around it
    }
    release |= bit;
    firstSeen[i] = 0; // a re-press earns a fresh two-tick grace
  }
  return release;
}

} // namespace deskflow::platform
