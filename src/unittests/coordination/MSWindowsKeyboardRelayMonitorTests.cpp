/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MSWindowsKeyboardRelayMonitorTests.h"

#include "coordination/KeyboardRelayMonitor.h"

using deskflow::coordination::createKeyboardRelayMonitor;

void MSWindowsKeyboardRelayMonitorTests::createMonitorReturnsNonNull()
{
  auto monitor = createKeyboardRelayMonitor();
  QVERIFY(monitor != nullptr);
}

void MSWindowsKeyboardRelayMonitorTests::destroyMonitorWithoutStartDoesNotHang()
{
  auto monitor = createKeyboardRelayMonitor();
  monitor->stop();
}

QTEST_MAIN(MSWindowsKeyboardRelayMonitorTests)
