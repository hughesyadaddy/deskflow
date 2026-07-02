/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MSWindowsCursorVisibilityTests.h"

#include "platform/MSWindowsCursorVisibility.h"

using deskflow::platform::mswindows::setCursorVisibility;

void MSWindowsCursorVisibilityTests::showCursorSucceeds()
{
  QVERIFY(setCursorVisibility(true));
}

void MSWindowsCursorVisibilityTests::hideThenShowCursorSucceeds()
{
  QVERIFY(setCursorVisibility(false));
  QVERIFY(setCursorVisibility(true));
}

QTEST_MAIN(MSWindowsCursorVisibilityTests)
