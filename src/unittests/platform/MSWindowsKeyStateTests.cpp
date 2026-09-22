/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MSWindowsKeyStateTests.h"

#include "platform/MSWindowsKeyState.h"

#include <QTest>

#include <set>

namespace {

using Ledger = MSWindowsKeyState::InjectedModifierMap;

// Stand-in for GetAsyncKeyState: the set of VKs the "OS" reports held.
auto downTable(std::set<WORD> down)
{
  return [down = std::move(down)](WORD vk) { return down.count(vk) != 0; };
}

constexpr ULONGLONG kGrace = MSWindowsKeyState::kInjectedModifierGraceMs;

} // namespace

// TODO(K1 cross-machine vector, needs a Windows build host): with VK_CAPITAL
// toggled on, MSWindowsKeyState::getKeyID / mapKeyFromEvent for the K key
// must yield KeyID 'K' (ToUnicodeEx composes the capital) AND carry
// KeyModifierCapsLock in the reported mask, so a macOS client / the login
// bridge see (id='K', mask=0x1000) and compose it without pressing Shift.
// Mirror of KeyMapTests::mapKey_* and
// OSXKeyStateTests::keyboardEventFlagsCarryShiftForUpperLetterWithCapsOn.

void MSWindowsKeyStateTests::release_emptyLedgerNothingDown_releasesNothing()
{
  const auto release = MSWindowsKeyState::injectedKeysToRelease(Ledger{}, downTable({}));
  QVERIFY(release.empty());
}

void MSWindowsKeyStateTests::release_ledgerEntryPhysicallyDown_isReleased()
{
  // The desk-switch strand: we injected Ctrl down, the UP was dropped, the
  // OS still holds it. At a boundary that is stale by definition.
  Ledger ledger{{VK_LCONTROL, 1000}, {VK_LWIN, 1000}};
  const auto release = MSWindowsKeyState::injectedKeysToRelease(ledger, downTable({VK_LCONTROL, VK_LWIN}));
  QCOMPARE(release, (std::vector<WORD>{VK_LWIN, VK_LCONTROL}));
}

void MSWindowsKeyStateTests::release_ledgerEntryNotDown_isSkipped()
{
  // Ledger says we hold it but the OS does not: nothing to release, and no
  // spurious UP (which could itself register as a tap).
  Ledger ledger{{VK_LMENU, 1000}};
  const auto release = MSWindowsKeyState::injectedKeysToRelease(ledger, downTable({}));
  QVERIFY(release.empty());
}

void MSWindowsKeyStateTests::release_shiftIsAlwaysACandidate()
{
  // Shift is not in the periodic audit table, so the boundary sweep must
  // cover it even when the ledger never recorded an injected Shift.
  const auto release = MSWindowsKeyState::injectedKeysToRelease(Ledger{}, downTable({VK_LSHIFT, VK_RSHIFT}));
  QCOMPARE(release, (std::vector<WORD>{VK_LSHIFT, VK_RSHIFT}));

  // ...but only when actually held.
  const auto none = MSWindowsKeyState::injectedKeysToRelease(Ledger{}, downTable({VK_RCONTROL}));
  QVERIFY(none.empty());
}

void MSWindowsKeyStateTests::release_orderIsAscendingVk()
{
  Ledger ledger{{VK_RMENU, 5}, {VK_LWIN, 5}, {VK_RCONTROL, 5}};
  const auto release =
      MSWindowsKeyState::injectedKeysToRelease(ledger, downTable({VK_RMENU, VK_LWIN, VK_RCONTROL, VK_LSHIFT}));
  // 0x5B (LWIN) < 0xA0 (LSHIFT) < 0xA3 (RCONTROL) < 0xA5 (RMENU).
  QCOMPARE(release, (std::vector<WORD>{VK_LWIN, VK_LSHIFT, VK_RCONTROL, VK_RMENU}));
}

void MSWindowsKeyStateTests::audit_freshEntry_isProtected()
{
  Ledger ledger{{VK_LCONTROL, 10000}};
  const uint32_t bits = MSWindowsKeyState::injectedModifierBits(ledger, 10000 + 100, false);
  QCOMPARE(bits, 1u << MSWindowsKeyState::modifierVkIndex(VK_LCONTROL));
}

void MSWindowsKeyStateTests::audit_entryAtGrace_isStillProtected()
{
  Ledger ledger{{VK_LMENU, 10000}};
  QCOMPARE(
      MSWindowsKeyState::injectedModifierBits(ledger, 10000 + kGrace, false),
      1u << MSWindowsKeyState::modifierVkIndex(VK_LMENU)
  );
}

void MSWindowsKeyStateTests::audit_entryPastGrace_isNoLongerProtected()
{
  // A DOWN with no UP for longer than the grace no longer vouches for the
  // key: the audit may release it. A fresher sibling keeps its protection.
  Ledger ledger{{VK_LMENU, 10000}, {VK_LWIN, 10000 + kGrace}};
  const uint32_t bits = MSWindowsKeyState::injectedModifierBits(ledger, 10000 + kGrace + 1, false);
  QCOMPARE(bits, 1u << MSWindowsKeyState::modifierVkIndex(VK_LWIN));
}

void MSWindowsKeyStateTests::audit_entryPastGrace_stillVouchesWhileEntered()
{
  // K4 audit MED-1: while the screen is entered the ledger vouches for
  // every entry however old -- a macOS server never repeats modifiers, so
  // a Ctrl held 2.5 s must not be released under the user by the 1 s audit.
  Ledger ledger{{VK_LCONTROL, 10000}};
  const uint32_t bits = MSWindowsKeyState::injectedModifierBits(ledger, 10000 + 2500, true);
  QCOMPARE(bits, 1u << MSWindowsKeyState::modifierVkIndex(VK_LCONTROL));
  // ... and at a boundary the grace still applies
  QCOMPARE(MSWindowsKeyState::injectedModifierBits(ledger, 10000 + 2500, false), 0u);
}

void MSWindowsKeyStateTests::ledgerRelease_emptyLedgerIgnoresPhysicalShift()
{
  // K4 audit MED-2: the secondary's leave() uses the ledger-only release,
  // which never lists Shift unless WE injected it -- so a Shift the user is
  // physically holding while the cursor leaves is not a candidate at all.
  const Ledger empty;
  QVERIFY(MSWindowsKeyState::ledgerKeysToRelease(empty, 0).empty());
  // (the full sweep, by contrast, always probes Shift)
  QCOMPARE(MSWindowsKeyState::injectedKeysToRelease(empty, downTable({VK_LSHIFT})), std::vector<WORD>{VK_LSHIFT});
}

void MSWindowsKeyStateTests::ledgerRelease_keepsExcludedModifier()
{
  // K4 audit MED-3: keep=Shift leaves a ledgered Shift (re-asserted on
  // enter for an ongoing drag) in place and releases only the rest.
  Ledger ledger{{VK_LSHIFT, 10000}, {VK_LCONTROL, 10000}, {VK_LWIN, 10000}};
  const std::vector<WORD> expected{VK_LWIN, VK_LCONTROL}; // ascending VK
  QCOMPARE(MSWindowsKeyState::ledgerKeysToRelease(ledger, KeyModifierShift), expected);
  const std::vector<WORD> all{VK_LSHIFT, VK_LWIN, VK_LCONTROL};
  QCOMPARE(MSWindowsKeyState::ledgerKeysToRelease(ledger, 0), all);
}

void MSWindowsKeyStateTests::audit_shiftNeverReachesAuditTable()
{
  // Shift is tracked (so the boundary sweep can see it) but its bits sit
  // above the six-row desk audit table, which loops only over its own rows.
  QVERIFY(MSWindowsKeyState::modifierVkIndex(VK_LSHIFT) >= 6);
  QVERIFY(MSWindowsKeyState::modifierVkIndex(VK_RSHIFT) >= 6);
  QCOMPARE(MSWindowsKeyState::modifierVkIndex(VK_SHIFT), -1);
}

void MSWindowsKeyStateTests::audit_bitOrderMatchesDeskTable()
{
  // Contract with deskSanitizeStaleModifiers' kModifiers[] order.
  QCOMPARE(MSWindowsKeyState::modifierVkIndex(VK_LWIN), 0);
  QCOMPARE(MSWindowsKeyState::modifierVkIndex(VK_RWIN), 1);
  QCOMPARE(MSWindowsKeyState::modifierVkIndex(VK_LMENU), 2);
  QCOMPARE(MSWindowsKeyState::modifierVkIndex(VK_RMENU), 3);
  QCOMPARE(MSWindowsKeyState::modifierVkIndex(VK_LCONTROL), 4);
  QCOMPARE(MSWindowsKeyState::modifierVkIndex(VK_RCONTROL), 5);
  QCOMPARE(MSWindowsKeyState::modifierVkIndex(VK_CAPITAL), -1);
}

QTEST_MAIN(MSWindowsKeyStateTests)
