/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "KeyboardRelayHookPolicyTests.h"

#include "coordination/KeyboardRelayHookPolicy.h"

#include <QTest>

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

QTEST_MAIN(KeyboardRelayHookPolicyTests)
