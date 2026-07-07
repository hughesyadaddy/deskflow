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

QTEST_MAIN(KeyboardRelayHookPolicyTests)
