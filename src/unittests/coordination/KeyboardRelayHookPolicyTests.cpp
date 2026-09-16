/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "KeyboardRelayHookPolicyTests.h"

#include "coordination/KeyboardRelayHookPolicy.h"

#include <QTest>

#include <algorithm>

using deskflow::coordination::KeyboardRelayHookContext;
using deskflow::coordination::keyboardRelayHookShouldPassThrough;

void KeyboardRelayHookPolicyTests::injectedKeysPassThroughWhileForwarding()
{
  // Regression: Mac host injects Alt+Tab via SendInput on Windows client.
  // Stale fleet routing after UAC must not eat injected shortcuts.
  const KeyboardRelayHookContext ctx{false, true, true, false};
  QVERIFY(keyboardRelayHookShouldPassThrough(ctx));
}

void KeyboardRelayHookPolicyTests::swallowOnlyWhenForwarded()
{
  const KeyboardRelayHookContext notForwarded{false, false, true, false};
  QVERIFY(keyboardRelayHookShouldPassThrough(notForwarded));

  const KeyboardRelayHookContext forwarded{false, false, true, true};
  QVERIFY(!keyboardRelayHookShouldPassThrough(forwarded));
}

void KeyboardRelayHookPolicyTests::passLocalAlwaysPassesThrough()
{
  const KeyboardRelayHookContext ctx{true, false, true, true};
  QVERIFY(keyboardRelayHookShouldPassThrough(ctx));
}

void KeyboardRelayHookPolicyTests::unmappedKeysPassThrough()
{
  const KeyboardRelayHookContext ctx{false, false, false, false};
  QVERIFY(keyboardRelayHookShouldPassThrough(ctx));
}

void KeyboardRelayHookPolicyTests::injectedWinsEvenWhenForwarded()
{
  const KeyboardRelayHookContext ctx{false, true, true, true};
  QVERIFY(keyboardRelayHookShouldPassThrough(ctx));
}


void KeyboardRelayHookPolicyTests::ledger_upFollowsForwardedDownAcrossSwitch()
{
  using deskflow::coordination::KeyboardRelayForwardLedger;
  KeyboardRelayForwardLedger ledger;

  // Down forwarded to the mesh while remote; cursor switches back local
  // mid-hold. The Up must still follow the Down's destination (forward),
  // otherwise the remote target keeps the key held forever.
  ledger.downForwarded(0x5B); // Win
  QVERIFY(ledger.follow(0x5B));
  ledger.release(0x5B);
  QVERIFY(!ledger.follow(0x5B));

  // A later fresh local Down of the same button clears any stale claim.
  ledger.downForwarded(0x12);
  ledger.downLocal(0x12);
  QVERIFY(!ledger.follow(0x12));
}

void KeyboardRelayHookPolicyTests::ledger_localDownKeepsUpLocal()
{
  using deskflow::coordination::KeyboardRelayForwardLedger;
  KeyboardRelayForwardLedger ledger;

  // Down delivered locally (passthrough / unmapped): its Up must not be
  // forwarded even if the cursor is remote by release time.
  ledger.downLocal(0x70); // F1
  QVERIFY(!ledger.follow(0x70));
}


void KeyboardRelayHookPolicyTests::ledger_localDownStaysLocalAfterCursorGoesRemote()
{
  using deskflow::coordination::KeyboardRelayForwardLedger;
  using Destination = KeyboardRelayForwardLedger::Destination;
  KeyboardRelayForwardLedger ledger;

  // THE LOGIN-SCREEN BUG: Shift pressed while the cursor was local (so the
  // Down went to this machine's OS), then the cursor becomes remote before
  // the release. The Up must still be recognised as LOCAL -- forwarding and
  // swallowing it left Shift physically held here, so every following
  // keystroke came out uppercase (in a password box, invisibly).
  ledger.downLocal(0xA0); // VK_LSHIFT
  QCOMPARE(ledger.destination(0xA0), Destination::Local);
  QVERIFY(!ledger.follow(0xA0)); // must not follow the mesh
  ledger.release(0xA0);
  QCOMPARE(ledger.destination(0xA0), Destination::Unknown);

  // A forwarded Down is still distinguishable from both.
  ledger.downForwarded(0xA0);
  QCOMPARE(ledger.destination(0xA0), Destination::Forwarded);
  QVERIFY(ledger.follow(0xA0));

  // Unseen buttons report Unknown (the only case that may fall back to
  // forward-and-swallow).
  QCOMPARE(ledger.destination(0x41), Destination::Unknown);
}

void KeyboardRelayHookPolicyTests::routeUp_ledgerBeatsCursorForForwardedModifier()
{
  using deskflow::coordination::KeyboardRelayForwardLedger;
  using deskflow::coordination::keyboardRelayRouteUp;
  using deskflow::coordination::KeyboardRelayUpRoute;
  using Destination = KeyboardRelayForwardLedger::Destination;

  // S1 (THE stuck Shift/Cmd bug): Shift pressed while the cursor was
  // remote (Down forwarded), released after the cursor came home
  // (passLocal == true by release time). The macOS flagsChanged branch
  // checked passLocal FIRST and released the ledger without forwarding
  // the Up -- Shift stayed held on the peer. The ledger must win.
  QCOMPARE(keyboardRelayRouteUp(Destination::Forwarded, true), KeyboardRelayUpRoute::Forward);
  QCOMPARE(keyboardRelayRouteUp(Destination::Forwarded, false), KeyboardRelayUpRoute::Forward);

  // The mirror case: pressed local, released with the cursor remote.
  QCOMPARE(keyboardRelayRouteUp(Destination::Local, false), KeyboardRelayUpRoute::Local);
  QCOMPARE(keyboardRelayRouteUp(Destination::Local, true), KeyboardRelayUpRoute::Local);
}

void KeyboardRelayHookPolicyTests::routeUp_unseenDownFollowsCursor()
{
  using deskflow::coordination::KeyboardRelayForwardLedger;
  using deskflow::coordination::keyboardRelayRouteUp;
  using deskflow::coordination::KeyboardRelayUpRoute;
  using Destination = KeyboardRelayForwardLedger::Destination;

  // Only a Down the hook never saw (monitor restart mid-hold) falls back
  // to the cursor: forward best-effort when remote, else local.
  QCOMPARE(keyboardRelayRouteUp(Destination::Unknown, false), KeyboardRelayUpRoute::ForwardUnknown);
  QCOMPARE(keyboardRelayRouteUp(Destination::Unknown, true), KeyboardRelayUpRoute::Local);
}

void KeyboardRelayHookPolicyTests::ledger_boundaryFlushReportsForwardedAndResyncMarksLocal()
{
  using deskflow::coordination::KeyboardRelayForwardLedger;
  using Destination = KeyboardRelayForwardLedger::Destination;
  KeyboardRelayForwardLedger ledger;

  ledger.downForwarded(0xA0); // Shift held on the peer
  ledger.downForwarded(0x5B); // Win held on the peer
  ledger.downLocal(0x41);     // 'a' held here

  // stop(): only the forwarded holds need an Up on the peer.
  const auto held = ledger.forwardedButtons();
  QCOMPARE(held.size(), static_cast<size_t>(2));
  QVERIFY(std::find(held.begin(), held.end(), 0xA0) != held.end());
  QVERIFY(std::find(held.begin(), held.end(), 0x5B) != held.end());
  QVERIFY(std::find(held.begin(), held.end(), 0x41) == held.end());

  // Lane failure / rescue: forwarded holds become Local so their Up passes
  // to the local OS (harmless) instead of being swallowed for a peer that
  // cannot be told; local holds are untouched.
  ledger.releaseAllForwardedLocally();
  QCOMPARE(ledger.destination(0xA0), Destination::Local);
  QCOMPARE(ledger.destination(0x5B), Destination::Local);
  QCOMPARE(ledger.destination(0x41), Destination::Local);
  QVERIFY(ledger.forwardedButtons().empty());

  ledger.clear();
  QVERIFY(ledger.empty());
  QCOMPARE(ledger.destination(0xA0), Destination::Unknown);
}

void KeyboardRelayHookPolicyTests::modifierShadow_tracksSwallowedShiftAndCapsToggle()
{
  using deskflow::coordination::KeyboardRelayModifierShadow;
  KeyboardRelayModifierShadow shadow;
  QCOMPARE(shadow.mask(), 0u);

  // A forwarded (swallowed) Left Shift Down is invisible to
  // GetAsyncKeyState; the shadow is what makes the next letter map as 'A'.
  shadow.note(KeyboardRelayModifierShadow::kVkLShift, true, false);
  QCOMPARE(shadow.mask(), static_cast<KeyModifierMask>(KeyModifierShift));
  // Repeats do not change anything; a second side keeps the bit on release
  // of the first.
  shadow.note(KeyboardRelayModifierShadow::kVkLShift, true, true);
  shadow.note(KeyboardRelayModifierShadow::kVkRShift, true, false);
  shadow.note(KeyboardRelayModifierShadow::kVkLShift, false, false);
  QCOMPARE(shadow.mask(), static_cast<KeyModifierMask>(KeyModifierShift));
  shadow.note(KeyboardRelayModifierShadow::kVkRShift, false, false);
  QCOMPARE(shadow.mask(), 0u);

  shadow.note(KeyboardRelayModifierShadow::kVkLControl, true, false);
  shadow.note(KeyboardRelayModifierShadow::kVkRMenu, true, false);
  shadow.note(KeyboardRelayModifierShadow::kVkLWin, true, false);
  QCOMPARE(shadow.mask(), static_cast<KeyModifierMask>(KeyModifierControl | KeyModifierAlt | KeyModifierSuper));
  shadow.reset();
  QCOMPARE(shadow.mask(), 0u);

  // CapsLock: a swallowed VK_CAPITAL Down never reaches the OS toggle, so
  // the shadow flips per Down edge (not on repeat, not on Up).
  shadow.seedCapsLock(false);
  shadow.note(KeyboardRelayModifierShadow::kVkCapital, true, false);
  QVERIFY(shadow.capsLock());
  QCOMPARE(shadow.mask(), static_cast<KeyModifierMask>(KeyModifierCapsLock));
  shadow.note(KeyboardRelayModifierShadow::kVkCapital, true, true);
  shadow.note(KeyboardRelayModifierShadow::kVkCapital, false, false);
  QVERIFY(shadow.capsLock());
  shadow.note(KeyboardRelayModifierShadow::kVkCapital, true, false);
  QVERIFY(!shadow.capsLock());
  // Non-modifier keys are ignored.
  shadow.note(0x41, true, false);
  QCOMPARE(shadow.mask(), 0u);
}

QTEST_MAIN(KeyboardRelayHookPolicyTests)
