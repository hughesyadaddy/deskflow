/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "LaunchOwnershipTests.h"

#include "gui/LaunchOwnership.h"

using deskflow::gui::decideLaunchOwnership;
using deskflow::gui::LaunchOwnership;

// The macbookpro incident: the LaunchAgent plist is installed, a BTM login
// item launched a second copy first. That copy has no DESKFLOW_LAUNCHD=1 and
// must step aside when launchd's copy asks.
void LaunchOwnershipTests::BtmCopyWithPlistHonoursQuit()
{
  const auto o = decideLaunchOwnership(false, true, true);
  QVERIFY(!o.isLaunchdProcess);
  QVERIFY(o.honoursQuit);
}

void LaunchOwnershipTests::LaunchdCopyNeverHonoursQuit()
{
  for (const bool plist : {false, true}) {
    for (const bool item : {false, true}) {
      const auto o = decideLaunchOwnership(true, plist, item);
      QVERIFY(o.isLaunchdProcess);
      QVERIFY(!o.honoursQuit);
    }
  }
}

void LaunchOwnershipTests::LaunchdCopyMayTakeOver()
{
  for (const bool plist : {false, true}) {
    for (const bool item : {false, true}) {
      QVERIFY(decideLaunchOwnership(true, plist, item).mayTakeOver);
    }
  }
}

void LaunchOwnershipTests::BtmCopyNeverTakesOver()
{
  for (const bool plist : {false, true}) {
    for (const bool item : {false, true}) {
      const auto o = decideLaunchOwnership(false, plist, item);
      QVERIFY(!o.mayTakeOver);
      QVERIFY(!o.isLaunchdProcess);
    }
  }
}

void LaunchOwnershipTests::PlistInstalledUnregistersLoginItem()
{
  // Either copy removes the login item once the agent is the launcher.
  QCOMPARE(decideLaunchOwnership(false, true, true).loginItem, LaunchOwnership::LoginItem::Unregister);
  QCOMPARE(decideLaunchOwnership(true, true, true).loginItem, LaunchOwnership::LoginItem::Unregister);
  // launchd's own copy without a plist on disk (rendered elsewhere) still wins.
  QCOMPARE(decideLaunchOwnership(true, false, true).loginItem, LaunchOwnership::LoginItem::Unregister);
  // Nothing to unregister when the item is already off.
  QCOMPARE(decideLaunchOwnership(true, true, false).loginItem, LaunchOwnership::LoginItem::None);
  QCOMPARE(decideLaunchOwnership(false, true, false).loginItem, LaunchOwnership::LoginItem::None);
}

void LaunchOwnershipTests::NeverRegisters()
{
  // The enum has no Register value; every combination yields None or Unregister.
  for (const bool env : {false, true}) {
    for (const bool plist : {false, true}) {
      for (const bool item : {false, true}) {
        const auto o = decideLaunchOwnership(env, plist, item);
        QVERIFY(
            o.loginItem == LaunchOwnership::LoginItem::None || o.loginItem == LaunchOwnership::LoginItem::Unregister
        );
        if (!item) {
          QCOMPARE(o.loginItem, LaunchOwnership::LoginItem::None);
        }
      }
    }
  }
}

void LaunchOwnershipTests::NoPlistNoEnvLeavesLoginItemAlone()
{
  // Dev box, no agent: a login item the user set up in System Settings stays.
  const auto o = decideLaunchOwnership(false, false, true);
  QCOMPARE(o.loginItem, LaunchOwnership::LoginItem::None);
  QVERIFY(o.honoursQuit);
  QVERIFY(!o.mayTakeOver);
  QCOMPARE(decideLaunchOwnership(false, false, false).loginItem, LaunchOwnership::LoginItem::None);
}

QTEST_MAIN(LaunchOwnershipTests)
