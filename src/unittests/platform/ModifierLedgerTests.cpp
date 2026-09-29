/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ModifierLedgerTests.h"

#include "platform/MSWindowsModifierLedger.h"

#include <QTest>

using deskflow::platform::AuditFirstSeen;
using deskflow::platform::forgetReleased;
using deskflow::platform::InjectedModifierMap;
using deskflow::platform::kAuditQuietMs;
using deskflow::platform::staleModifiersToRelease;

namespace {

// Win32 virtual keys, spelled out so this test needs no Windows.h.
constexpr uint16_t kVkLWin = 0x5B;
constexpr uint16_t kVkRWin = 0x5C;
constexpr uint16_t kVkLControl = 0xA2;

// Audit table rows (contract with deskSanitizeStaleModifiers' kModifiers[]).
constexpr uint32_t kRowLWin = 1u << 0;
constexpr uint32_t kRowRWin = 1u << 1;
constexpr uint32_t kRowLMenu = 1u << 2;
constexpr uint32_t kRowLControl = 1u << 4;

constexpr uint64_t kTick = 1000; // the audit period

} // namespace

void ModifierLedgerTests::ledger_afterBoundaryRelease_isEmpty()
{
  // The boundary sweep (enable/enter) releases what the OS reports down and
  // returns those VKs; the ledger-only sweep then attempts every entry. A
  // phantom entry (LWIN+RWIN "held by us", nothing physically down -- the
  // live "injected bits 0x03") must not survive either pass.
  InjectedModifierMap ledger{{kVkLWin, 5000}, {kVkRWin, 5000}};
  const std::vector<uint16_t> auditReleased{}; // OS held nothing
  forgetReleased(ledger, auditReleased);
  QCOMPARE(ledger.size(), 2u);
  const std::vector<uint16_t> attempted{kVkLWin, kVkRWin}; // ledgerKeysToRelease()
  forgetReleased(ledger, attempted);
  QVERIFY(ledger.empty());
}

void ModifierLedgerTests::ledger_afterLeave_isEmpty()
{
  // leave(): raw sanitize(0) releases the LWIN the OS still holds; the
  // ledger sweep then attempts LWIN (already up now) and LCONTROL. Before
  // D1 the raw release erased nothing and the ledger sweep erased only what
  // the desk thread still found down, so LWIN stayed ledgered forever.
  InjectedModifierMap ledger{{kVkLWin, 5000}, {kVkLControl, 5000}};
  forgetReleased(ledger, {kVkLWin}); // returned by the raw sweep
  QCOMPARE(ledger.size(), 1u);
  forgetReleased(ledger, {kVkLControl}); // ledger sweep: attempted, whether or not down
  QVERIFY(ledger.empty());
}

void ModifierLedgerTests::ledger_forgetKeepsUntouchedEntries()
{
  InjectedModifierMap ledger{{kVkLWin, 5000}, {kVkLControl, 5000}};
  forgetReleased(ledger, {kVkRWin, kVkLWin}); // RWIN never ledgered: harmless
  QCOMPARE(ledger.size(), 1u);
  QCOMPARE(ledger.count(kVkLControl), 1u);
}

void ModifierLedgerTests::audit_unledgeredDown_needsTwoTicks()
{
  // A Win the OS reports held that we did not inject is released only on
  // its second consecutive sighting: another injector (PowerToys KBM, an
  // AutoHotkey Win hook) holding it around one of our keys is gone by then.
  AuditFirstSeen seen{};
  AuditFirstSeen released{};
  const uint64_t quiet = 0; // no Win+key ever sent
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, 10 * kTick, quiet, true, released), 0u);
  QCOMPARE(seen[0], 10 * kTick);
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, 11 * kTick, quiet, true, released), kRowLWin);
}

void ModifierLedgerTests::audit_unledgeredCtrl_releasedOnTick2_regardlessOfTyping()
{
  // Review D3: a stuck non-ledgered LCONTROL turns every letter into
  // Ctrl+letter, so it must be released on its second sighting even though
  // a Win+key (the only thing the quiet window is about) went out 100 ms
  // ago and the user is typing continuously. Same for the Alt rows.
  AuditFirstSeen seen{};
  AuditFirstSeen released{};
  const uint64_t now = 20 * kTick;
  QCOMPARE(staleModifiersToRelease(kRowLControl, 0, seen, now, now - 100, true, released), 0u);
  QCOMPARE(seen[4], now);
  QCOMPARE(
      staleModifiersToRelease(kRowLControl, 0, seen, now + kTick, now + kTick - 100, true, released), kRowLControl
  );
  QCOMPARE(seen[4], 0u);
  // ...and again after the reset, still under a fresh Super chord.
  QCOMPARE(staleModifiersToRelease(kRowLControl, 0, seen, now + 2 * kTick, now + 2 * kTick - 100, true, released), 0u);
  QCOMPARE(
      staleModifiersToRelease(kRowLControl, 0, seen, now + 3 * kTick, now + 3 * kTick - 100, true, released),
      kRowLControl
  );
  // The Alt rows share the Ctrl rule.
  AuditFirstSeen seenAlt{};
  AuditFirstSeen releasedAlt{};
  QCOMPARE(staleModifiersToRelease(kRowLMenu, 0, seenAlt, now, now - 100, true, releasedAlt), 0u);
  QCOMPARE(staleModifiersToRelease(kRowLMenu, 0, seenAlt, now + kTick, now + kTick - 100, true, releasedAlt), kRowLMenu);
}

void ModifierLedgerTests::audit_unledgeredWin_skippedOnlyAfterSuperChord()
{
  // Win row, two sightings, but a Super-bearing key-down (Win+key we sent
  // while LWIN was ledgered) went out within the quiet window: a target
  // hook is legitimately re-processing that chord -- do not fight it.
  AuditFirstSeen seen{};
  AuditFirstSeen released{};
  const uint64_t now = 20 * kTick;
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, now, now - 100, true, released), 0u);
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, now + kTick, now + kTick - 100, true, released), 0u);
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, now + 2 * kTick, now + 2 * kTick - 100, true, released), 0u);
  QCOMPARE(seen[0], now); // still counting from the first sighting
  // ...no further Super chord: released on the first tick past the window.
  const uint64_t lastChord = now + 2 * kTick - 100;
  QCOMPARE(
      staleModifiersToRelease(kRowLWin, 0, seen, lastChord + kAuditQuietMs, lastChord, true, released), kRowLWin
  );
  QCOMPARE(seen[0], 0u);

  // The same Win row with only PLAIN typing (the caller reports no Super
  // chord at all, 0): released on tick 2 like any other row -- plain letters
  // say nothing about who is holding Win.
  AuditFirstSeen seenPlain{};
  AuditFirstSeen releasedPlain{};
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seenPlain, now, 0, true, releasedPlain), 0u);
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seenPlain, now + kTick, 0, true, releasedPlain), kRowLWin);

  // A Super chord older than the window does not postpone RWIN either.
  AuditFirstSeen seenOld{};
  AuditFirstSeen releasedOld{};
  const uint64_t oldChord = now - kAuditQuietMs;
  QCOMPARE(staleModifiersToRelease(kRowRWin, 0, seenOld, now, oldChord, true, releasedOld), 0u);
  QCOMPARE(staleModifiersToRelease(kRowRWin, 0, seenOld, now + kTick, oldChord, true, releasedOld), kRowRWin);
}

void ModifierLedgerTests::audit_ledgeredDown_neverReleased()
{
  // A row we hold (ledgered) is the server's chord, however long it lasts;
  // the non-ledgered LWIN beside it is released on every second sighting
  // (a release resets its grace, so a persisting/re-pressed Win alternates).
  AuditFirstSeen seen{};
  AuditFirstSeen released{};
  for (uint64_t tick = 1; tick <= 10; ++tick) { // tick 0 would be the "not seen" sentinel
    QCOMPARE(
        staleModifiersToRelease(kRowLWin | kRowLControl, kRowLControl, seen, tick * kTick, 0, true, released),
        tick % 2 == 0 ? kRowLWin : 0u
    );
  }
  QCOMPARE(seen[4], 0u); // LCONTROL never counted as a sighting
}

void ModifierLedgerTests::audit_boundary_releasesImmediately()
{
  // enable/enter/leave: the server holds nothing here, so no grace and no
  // quiet window -- every non-ledgered held row goes at once.
  AuditFirstSeen seen{};
  AuditFirstSeen released{};
  const uint64_t now = 30 * kTick;
  QCOMPARE(
      staleModifiersToRelease(kRowLWin | kRowRWin, 0, seen, now, now - 10, false, released), kRowLWin | kRowRWin
  );
  QCOMPARE(seen[0], 0u);
  QCOMPARE(seen[1], 0u);
}

void ModifierLedgerTests::audit_upBetweenTicks_resetsGrace()
{
  // Seen once, up on the next tick, down again: a fresh first sighting.
  AuditFirstSeen seen{};
  AuditFirstSeen released{};
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, 1 * kTick, 0, true, released), 0u);
  QCOMPARE(staleModifiersToRelease(0, 0, seen, 2 * kTick, 0, true, released), 0u);
  QCOMPARE(seen[0], 0u);
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, 3 * kTick, 0, true, released), 0u);
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, 4 * kTick, 0, true, released), kRowLWin);
}

void ModifierLedgerTests::audit_releaseResetsGraceForRepress()
{
  // After a release the row's grace restarts, so a re-press by another hook
  // is again given a tick rather than released on sight -- unchanged by the
  // 2026-09-29 fast-track below, which only ever skips the QUIET WINDOW,
  // never this first-sighting tick.
  AuditFirstSeen seen{};
  AuditFirstSeen released{};
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, 1 * kTick, 0, true, released), 0u);
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, 2 * kTick, 0, true, released), kRowLWin);
  QCOMPARE(seen[0], 0u);
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, 3 * kTick, 0, true, released), 0u);
}

void ModifierLedgerTests::audit_recentlyReleasedWin_skipsQuietWindowOnRepeat()
{
  // The exact 2026-09-29 live pattern: LWin released once (no chord active,
  // so this first release is unaffected by anything new here), then stuck
  // again right away -- this time WITH an active Super-chord quiet window
  // that would normally hold a fresh Win occurrence off for a further
  // kAuditQuietMs. Because this row was released by us moments ago, the
  // quiet window is skipped on its second sighting -- but the first
  // sighting still only registers, exactly like any other occurrence.
  AuditFirstSeen seen{};
  AuditFirstSeen released{};
  const uint64_t now = 40 * kTick;
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, now, 0, true, released), 0u);
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, now + kTick, 0, true, released), kRowLWin);
  QCOMPARE(released[0], now + kTick);

  const uint64_t chordAt = now + kTick + 50; // a Super+key we sent, well within the quiet window below
  // First sighting of the NEW occurrence, 500 ms after the release: still
  // just registers, same as audit_releaseResetsGraceForRepress -- the
  // fast-track never skips this.
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, now + kTick + 500, chordAt, true, released), 0u);
  // Second sighting, 1.5 s after the release (comfortably under
  // kAuditQuietMs=2000): an ordinary (not-recently-released) Win row would
  // be held off here because chordAt is well inside its own quiet window.
  // This one releases immediately instead, because recentlyReleasedMs[0]
  // is still fresh from the release 1.5 s ago.
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, now + kTick + 1500, chordAt, true, released), kRowLWin);
}

void ModifierLedgerTests::audit_recentlyReleasedExpires_afterQuietWindow()
{
  // The fast-track is itself time-bounded: once kAuditQuietMs has passed
  // since our release, a fresh occurrence goes back to being treated as
  // any other Win row, quiet window and all.
  AuditFirstSeen seen{};
  AuditFirstSeen released{};
  const uint64_t now = 50 * kTick;
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, now, 0, true, released), 0u);
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, now + kTick, 0, true, released), kRowLWin);
  const uint64_t releasedAt = now + kTick;

  // Well past the fast-track window, with an active chord: full quiet
  // window applies again, exactly like a row with no release history.
  const uint64_t later = releasedAt + kAuditQuietMs + kTick;
  const uint64_t chordAt = later - 100;
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, later, chordAt, true, released), 0u);
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, later + kTick, chordAt, true, released), 0u);
}

QTEST_MAIN(ModifierLedgerTests)
