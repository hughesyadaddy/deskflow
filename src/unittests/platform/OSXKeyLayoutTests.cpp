/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "OSXKeyLayoutTests.h"

#include "OSXMainQueueTestHarness.h"
#include "platform/OSXKeyLayout.h"

#include <CoreFoundation/CoreFoundation.h>
#include <memory>

void OSXKeyLayoutTests::copyUnicodeKeyLayoutOnMainQueue_offMainThread()
{
  using Snapshot = deskflow::platform::osx::UnicodeKeyLayoutSnapshot;
  const auto offMainOpt = deskflow::test::osx::runOffMainThreadWithMainQueuePump([]() {
    return std::make_shared<Snapshot>(deskflow::platform::osx::copyUnicodeKeyLayoutOnMainQueue());
  });
  QVERIFY2(offMainOpt.has_value(), "off-main-thread layout fetch did not complete within timeout");

  const Snapshot &snapshot = **offMainOpt;
  QVERIFY2(snapshot.layoutBytes != nullptr, "expected Unicode key layout data");
  QVERIFY2(CFDataGetLength(snapshot.layoutBytes) > 0, "expected non-empty layout data");
  QVERIFY2(snapshot.keyboardType != 0, "expected keyboard type from LMGetKbdType");
}

void OSXKeyLayoutTests::copyUnicodeKeyLayoutOnMainQueue_matchesMainThread()
{
  using Snapshot = deskflow::platform::osx::UnicodeKeyLayoutSnapshot;
  const Snapshot onMain = deskflow::platform::osx::copyUnicodeKeyLayoutOnMainQueue();
  const auto offMainOpt = deskflow::test::osx::runOffMainThreadWithMainQueuePump([]() {
    return std::make_shared<Snapshot>(deskflow::platform::osx::copyUnicodeKeyLayoutOnMainQueue());
  });
  QVERIFY2(offMainOpt.has_value(), "off-main-thread layout fetch did not complete within timeout");
  const Snapshot &offMain = **offMainOpt;

  QCOMPARE(offMain.keyboardType, onMain.keyboardType);
  QVERIFY2(onMain.layoutBytes != nullptr && offMain.layoutBytes != nullptr, "expected layout data");
  QCOMPARE(CFDataGetLength(offMain.layoutBytes), CFDataGetLength(onMain.layoutBytes));
  QCOMPARE(CFEqual(offMain.layoutBytes, onMain.layoutBytes), true);
}

QTEST_MAIN(OSXKeyLayoutTests)
