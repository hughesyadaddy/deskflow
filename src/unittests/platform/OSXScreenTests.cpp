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

void OSXScreenTests::wheelLinesFromEvent_integerWinsBelowOneLine()
{
  // a slow notch on a plain wheel: FixedPt 0.1 but DeltaAxis 1 -> one line
  QCOMPARE(OSXScreen::wheelLinesFromEvent(0.1, 1), 1.0);
  QCOMPARE(OSXScreen::wheelLinesFromEvent(-0.1, -1), -1.0);
  // fast notch carries a fraction worth keeping
  QCOMPARE(OSXScreen::wheelLinesFromEvent(2.5, 3), 2.5);
  QCOMPARE(OSXScreen::wheelLinesFromEvent(-1.0, -1), -1.0);
  // hi-res sub-notch tick: integer rounds to zero, the fraction survives
  QCOMPARE(OSXScreen::wheelLinesFromEvent(0.0125, 0), 0.0125);
  QCOMPARE(OSXScreen::wheelLinesFromEvent(-0.0125, 0), -0.0125);
  QCOMPARE(OSXScreen::wheelLinesFromEvent(0.0, 0), 0.0);
}

QTEST_MAIN(OSXScreenTests)
