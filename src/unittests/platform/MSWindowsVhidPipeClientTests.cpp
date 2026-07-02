/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MSWindowsVhidPipeClientTests.h"

#include "arch/win32/XArchWindows.h"
#include "platform/MSWindowsVhidPipeClient.h"

using deskflow::platform::mswindows::MSWindowsVhidPipeClient;

void MSWindowsVhidPipeClientTests::virtualKeyToHidUsageMapsCommonKeys()
{
  QCOMPARE(MSWindowsVhidPipeClient::virtualKeyToHidUsage(VK_RETURN).value_or(0), 40);
  QCOMPARE(MSWindowsVhidPipeClient::virtualKeyToHidUsage(VK_TAB).value_or(0), 43);
  QCOMPARE(MSWindowsVhidPipeClient::virtualKeyToHidUsage('A').value_or(0), 4);
  QVERIFY(!MSWindowsVhidPipeClient::virtualKeyToHidUsage(VK_F1).has_value());
}

QTEST_MAIN(MSWindowsVhidPipeClientTests)
