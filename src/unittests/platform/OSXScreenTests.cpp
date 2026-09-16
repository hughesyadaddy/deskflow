/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "OSXScreenTests.h"

#include "platform/OSXScreen.h"

#include <QTest>

void OSXScreenTests::clipboardChangeCountGate()
{
  long last = -1;
  QVERIFY(OSXScreen::clipboardChangeCountAdvanced(last, 7));
  QCOMPARE(last, 7L);
  QVERIFY(!OSXScreen::clipboardChangeCountAdvanced(last, 7));
  QVERIFY(OSXScreen::clipboardChangeCountAdvanced(last, 9));
  QCOMPARE(last, 9L);
  QVERIFY(OSXScreen::clipboardChangeCountAdvanced(last, 3));
  QCOMPARE(last, 3L);
}

QTEST_MAIN(OSXScreenTests)
