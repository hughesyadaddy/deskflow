/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

// K8: "failed to initialize screen shape" must not be fatal at start/wake.
// The platform screen polls for a valid display on this schedule and only
// then starts the epoch (pure logic; OSXScreen supplies the real probe).

#include "platform/DisplayWaitPolicy.h"

#include <QTest>

#include <chrono>
#include <vector>

using deskflow::platform::DisplayWaitPolicy;
using deskflow::platform::waitForValidDisplay;
using namespace std::chrono_literals;

class DisplayWaitPolicyTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void defaultsPollEveryHalfSecondForThirtySeconds();
  void immediateDisplayNeedsNoWait();
  void displayAppearingLaterIsWaitedFor();
  void givesUpAfterMaxWaitWithPacedLogging();
};

namespace {

struct Recorder
{
  std::vector<std::chrono::milliseconds> sleeps;
  std::vector<std::chrono::milliseconds> logs;
  int probes = 0;
};

} // namespace

void DisplayWaitPolicyTests::defaultsPollEveryHalfSecondForThirtySeconds()
{
  const DisplayWaitPolicy policy;
  QCOMPARE(policy.interval, 500ms);
  QCOMPARE(policy.maxWait, 30000ms);
  QCOMPARE(policy.maxAttempts(), 61);
}

void DisplayWaitPolicyTests::immediateDisplayNeedsNoWait()
{
  Recorder r;
  const bool ok = waitForValidDisplay(
      DisplayWaitPolicy{},
      [&] {
        ++r.probes;
        return true;
      },
      [&](std::chrono::milliseconds d) { r.sleeps.push_back(d); },
      [&](std::chrono::milliseconds e) { r.logs.push_back(e); }
  );
  QVERIFY(ok);
  QCOMPARE(r.probes, 1);
  QVERIFY(r.sleeps.empty());
  QVERIFY(r.logs.empty());
}

void DisplayWaitPolicyTests::displayAppearingLaterIsWaitedFor()
{
  // 07:58:24 the display list was empty; the hot-plug settled ~3 s later.
  Recorder r;
  const bool ok = waitForValidDisplay(
      DisplayWaitPolicy{},
      [&] {
        ++r.probes;
        return r.probes >= 7; // valid after 3 s
      },
      [&](std::chrono::milliseconds d) { r.sleeps.push_back(d); },
      [&](std::chrono::milliseconds e) { r.logs.push_back(e); }
  );
  QVERIFY(ok);
  QCOMPARE(r.probes, 7);
  QCOMPARE(r.sleeps.size(), size_t{6});
  for (const auto s : r.sleeps) {
    QCOMPARE(s, 500ms);
  }
  // One progress line before the first retry, the next not before 5 s.
  QCOMPARE(r.logs, (std::vector<std::chrono::milliseconds>{0ms}));
}

void DisplayWaitPolicyTests::givesUpAfterMaxWaitWithPacedLogging()
{
  Recorder r;
  const bool ok = waitForValidDisplay(
      DisplayWaitPolicy{},
      [&] {
        ++r.probes;
        return false;
      },
      [&](std::chrono::milliseconds d) { r.sleeps.push_back(d); },
      [&](std::chrono::milliseconds e) { r.logs.push_back(e); }
  );
  QVERIFY(!ok);
  QCOMPARE(r.probes, 61);
  QCOMPARE(r.sleeps.size(), size_t{60}); // exactly 30 s of waiting
  QCOMPARE(r.logs, (std::vector<std::chrono::milliseconds>{0ms, 5000ms, 10000ms, 15000ms, 20000ms, 25000ms}));

  // A shorter policy is honoured (the epoch loop's backoff paces retries).
  Recorder quick;
  QVERIFY(!waitForValidDisplay(
      DisplayWaitPolicy{100ms, 300ms, 1000ms}, [&] { return false; },
      [&](std::chrono::milliseconds d) { quick.sleeps.push_back(d); }, nullptr
  ));
  QCOMPARE(quick.sleeps.size(), size_t{3});
}

QTEST_MAIN(DisplayWaitPolicyTests)

#include "DisplayWaitPolicyTests.moc"
