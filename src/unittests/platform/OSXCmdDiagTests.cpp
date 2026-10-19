/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 *
 * Logging-helper unit tests for temporary [cmd-diag]. No HID inject, no pasteboard.
 */
#include "OSXCmdDiagTests.h"

#include "platform/OSXCmdDiag.h"

#include <ApplicationServices/ApplicationServices.h>
#include <QTest>

using namespace deskflow::platform::cmd_diag;

void OSXCmdDiagTests::cmdOn_detectsCommandBit()
{
  QVERIFY(!cmdOn(0));
  QVERIFY(cmdOn(kCGEventFlagMaskCommand));
  QVERIFY(cmdOn(kCGEventFlagMaskCommand | kCGEventFlagMaskShift));
  QVERIFY(!cmdOn(kCGEventFlagMaskShift | kCGEventFlagMaskControl));
}

void OSXCmdDiagTests::noteEnterLeave_updatesBoundaryFields()
{
  noteEnter(false);
  QCOMPARE(boundaries().onScreen.load(), 1);
  QCOMPARE(boundaries().isPrimary.load(), 0);
  QVERIFY(boundaries().lastEnterMs.load() > 0);

  noteLeave();
  QCOMPARE(boundaries().onScreen.load(), 0);
  QVERIFY(boundaries().lastLeaveMs.load() > 0);

  noteEnter(true);
  QCOMPARE(boundaries().isPrimary.load(), 1);
}

QTEST_MAIN(OSXCmdDiagTests)
#include "OSXCmdDiagTests.moc"
