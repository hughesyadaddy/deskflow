/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

// K8 epoch rebind: the server wedge probe must never restart an epoch that
// is still starting, never count strikes against a listener that has not
// answered yet in this epoch, restart at most once per cool-down, and
// probe the loopback of the family the listener actually bound.

#include "coordination/WedgeDetector.h"

#include <QTest>

#include <string>

using deskflow::coordination::probeHostForListenInterface;
using deskflow::coordination::WedgeDetector;

class WedgeDetectorTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void notProbedUntilTheEpochReportsListening();
  void strikesCountOnlyAfterTheListenerAnswered();
  void twoStrikesRestartThenCoolDown();
  void restartDisarmsUntilTheNewEpochAnswers();
  void listenerReleaseResetsState();
  void probeHostFollowsBoundFamily();
};

void WedgeDetectorTests::notProbedUntilTheEpochReportsListening()
{
  WedgeDetector w;
  QVERIFY(!w.shouldProbe());
  // Probe results that arrive anyway (racing a teardown) are ignored.
  QVERIFY(!w.recordProbe(false, 0.0));
  QVERIFY(!w.recordProbe(false, 30.0));
  QCOMPARE(w.strikes(), 0);

  w.setListening(true);
  QVERIFY(w.shouldProbe());
  w.setListening(false);
  QVERIFY(!w.shouldProbe());
}

void WedgeDetectorTests::strikesCountOnlyAfterTheListenerAnswered()
{
  // 2026-09-25 07:58: the probe fired into a server epoch that was still
  // waiting for a display and restarted it. A listener that never answered
  // is "still starting or bound elsewhere", never a wedge.
  WedgeDetector w;
  w.setListening(true);
  for (int i = 0; i < 10; ++i) {
    QVERIFY(!w.recordProbe(false, 30.0 * i));
  }
  QVERIFY(!w.armed());
  QCOMPARE(w.strikes(), 0);

  QVERIFY(!w.recordProbe(true, 300.0));
  QVERIFY(w.armed());
  QVERIFY(!w.recordProbe(false, 330.0));
  QCOMPARE(w.strikes(), 1);
  QVERIFY(w.recordProbe(false, 360.0));
}

void WedgeDetectorTests::twoStrikesRestartThenCoolDown()
{
  WedgeDetector w{WedgeDetector::Config{2, 60.0}};
  w.setListening(true);
  QVERIFY(!w.recordProbe(true, 0.0));
  QVERIFY(!w.recordProbe(false, 30.0));
  QVERIFY(w.recordProbe(false, 60.0));
  QCOMPARE(w.lastRestartAt(), 60.0);

  // The epoch is rebuilt: listener down, then up, then it answers.
  w.setListening(false);
  w.setListening(true);
  QVERIFY(!w.recordProbe(true, 61.0));
  // Fails again at once: two strikes land inside the cool-down -> no
  // second restart until 60 s after the first one.
  QVERIFY(!w.recordProbe(false, 90.0));
  QVERIFY(!w.recordProbe(false, 119.0));
  QCOMPARE(w.strikes(), 2);
  QVERIFY(w.recordProbe(false, 120.0));
  QCOMPARE(w.lastRestartAt(), 120.0);
}

void WedgeDetectorTests::restartDisarmsUntilTheNewEpochAnswers()
{
  WedgeDetector w;
  w.setListening(true);
  QVERIFY(!w.recordProbe(true, 0.0));
  QVERIFY(!w.recordProbe(false, 30.0));
  QVERIFY(w.recordProbe(false, 60.0));
  QVERIFY(!w.armed());
  QCOMPARE(w.strikes(), 0);
  // Nothing re-confirmed: failures after the restart are not strikes even
  // long after the cool-down (the rebuilt epoch never bound -> a bind
  // failure, handled by the epoch loop's backoff/exit, not by us).
  QVERIFY(!w.recordProbe(false, 200.0));
  QVERIFY(!w.recordProbe(false, 230.0));
  QVERIFY(!w.recordProbe(false, 260.0));
  QCOMPARE(w.strikes(), 0);
}

void WedgeDetectorTests::listenerReleaseResetsState()
{
  WedgeDetector w;
  w.setListening(true);
  QVERIFY(!w.recordProbe(true, 0.0));
  QVERIFY(!w.recordProbe(false, 30.0));
  QCOMPARE(w.strikes(), 1);
  w.setListening(false);
  QVERIFY(!w.armed());
  QCOMPARE(w.strikes(), 0);
  w.setListening(true);
  // A single failure right after the new listener came up is not a strike.
  QVERIFY(!w.recordProbe(false, 100.0));
  QCOMPARE(w.strikes(), 0);
}

void WedgeDetectorTests::probeHostFollowsBoundFamily()
{
  // NetworkAddress(port) binds INADDR_ANY (IPv4): loopback v4.
  QCOMPARE(probeHostForListenInterface(""), std::string("127.0.0.1"));
  QCOMPARE(probeHostForListenInterface("*"), std::string("127.0.0.1"));
  QCOMPARE(probeHostForListenInterface("0.0.0.0"), std::string("127.0.0.1"));
  QCOMPARE(probeHostForListenInterface(" 0.0.0.0 "), std::string("127.0.0.1"));
  // A v6 any-address listener would never answer a v4 probe.
  QCOMPARE(probeHostForListenInterface("::"), std::string("::1"));
  QCOMPARE(probeHostForListenInterface("[::]"), std::string("::1"));
  QCOMPARE(probeHostForListenInterface("::0"), std::string("::1"));
  // A specific interface is probed where it listens.
  QCOMPARE(probeHostForListenInterface("192.168.1.5"), std::string("192.168.1.5"));
  QCOMPARE(probeHostForListenInterface("[fd00::5]"), std::string("fd00::5"));
  QCOMPARE(probeHostForListenInterface("macbookpro.lan"), std::string("macbookpro.lan"));
}

QTEST_MAIN(WedgeDetectorTests)

#include "WedgeDetectorTests.moc"
