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
  const uint64_t quiet = 0; // never typed
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, 10 * kTick, quiet, true), 0u);
  QCOMPARE(seen[0], 10 * kTick);
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, 11 * kTick, quiet, true), kRowLWin);
}

void ModifierLedgerTests::audit_unledgeredDown_notReleasedWhileTyping()
{
  // Two sightings but a key-down injected within the quiet window: the hook
  // is legitimately re-processing our keys -- do not fight it every second.
  AuditFirstSeen seen{};
  const uint64_t now = 20 * kTick;
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, now, now - 100, true), 0u);
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, now + kTick, now + kTick - 100, true), 0u);
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, now + 2 * kTick, now + 2 * kTick - 100, true), 0u);
  // ...the keyboard goes quiet: released on the next tick after the window.
  const uint64_t lastKey = now + 2 * kTick - 100;
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, lastKey + kAuditQuietMs, lastKey, true), kRowLWin);
}

void ModifierLedgerTests::audit_ledgeredDown_neverReleased()
{
  // A row we hold (ledgered) is the server's chord, however long it lasts;
  // the non-ledgered LWIN beside it is released on every second sighting
  // (a release resets its grace, so a persisting/re-pressed Win alternates).
  AuditFirstSeen seen{};
  for (uint64_t tick = 1; tick <= 10; ++tick) { // tick 0 would be the "not seen" sentinel
    QCOMPARE(
        staleModifiersToRelease(kRowLWin | kRowLControl, kRowLControl, seen, tick * kTick, 0, true),
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
  const uint64_t now = 30 * kTick;
  QCOMPARE(staleModifiersToRelease(kRowLWin | kRowRWin, 0, seen, now, now - 10, false), kRowLWin | kRowRWin);
  QCOMPARE(seen[0], 0u);
  QCOMPARE(seen[1], 0u);
}

void ModifierLedgerTests::audit_upBetweenTicks_resetsGrace()
{
  // Seen once, up on the next tick, down again: a fresh first sighting.
  AuditFirstSeen seen{};
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, 1 * kTick, 0, true), 0u);
  QCOMPARE(staleModifiersToRelease(0, 0, seen, 2 * kTick, 0, true), 0u);
  QCOMPARE(seen[0], 0u);
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, 3 * kTick, 0, true), 0u);
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, 4 * kTick, 0, true), kRowLWin);
}

void ModifierLedgerTests::audit_releaseResetsGraceForRepress()
{
  // After a release the row's grace restarts, so a re-press by another hook
  // is again given a tick rather than released on sight.
  AuditFirstSeen seen{};
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, 1 * kTick, 0, true), 0u);
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, 2 * kTick, 0, true), kRowLWin);
  QCOMPARE(seen[0], 0u);
  QCOMPARE(staleModifiersToRelease(kRowLWin, 0, seen, 3 * kTick, 0, true), 0u);
}

QTEST_MAIN(ModifierLedgerTests)
