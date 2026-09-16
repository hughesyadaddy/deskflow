/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "BridgeCalibration.h"

#include <QTest>

#include <cstdlib>
#include <numeric>
#include <optional>

using namespace bridge_logic;

//! Pure-logic tests for deskflow-vhid-bridge (no HID device, no IOKit).
class BridgeCalibrationTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  // -- key modifiers -------------------------------------------------------
  void maskToModifierBits_data();
  void maskToModifierBits();
  void capsEntryNeverCarriesShift();
  void capsMaskBitNeverBecomesModifier();
  void desiredCapsFromMask();

  // -- caps decision table -------------------------------------------------
  void capsDecisionTable_data();
  void capsDecisionTable();

  // -- chunking ------------------------------------------------------------
  void chunk400Is50x8();
  void chunkPreservesSign();
  void chunkZero();
  void chunkRemainder();
  void chunkXyLockstep();
  void chunkBadMaxFallsBack();

  // -- calibration math ----------------------------------------------------
  void countsPerPointBasic();
  void countsPerPointRejectsTinyOrWrongSign();
  void countsPerPointRejectsAbsurdScale();
  void combineAxes();
  void pointsToCountsRounds();

  // -- residual carry ------------------------------------------------------
  void residualBounded();
  void residualRejectsHuge();
  void residualZeroWhenOnTarget();
};

void BridgeCalibrationTests::maskToModifierBits_data()
{
  QTest::addColumn<uint32_t>("mask");
  QTest::addColumn<uint8_t>("bits");
  QTest::newRow("none") << uint32_t{0} << uint8_t{0};
  QTest::newRow("shift") << kMaskShift << kHidLeftShift;
  QTest::newRow("control") << kMaskControl << kHidLeftControl;
  QTest::newRow("alt") << kMaskAlt << kHidLeftOption;
  QTest::newRow("meta") << kMaskMeta << kHidLeftCommand;
  QTest::newRow("super") << kMaskSuper << kHidLeftCommand;
  QTest::newRow("shift+ctrl") << (kMaskShift | kMaskControl) << uint8_t(kHidLeftShift | kHidLeftControl);
  QTest::newRow("capslock-only") << kMaskCapsLock << uint8_t{0};
  QTest::newRow("shift+caps (log 0x1001)") << (kMaskShift | kMaskCapsLock) << kHidLeftShift;
}

void BridgeCalibrationTests::maskToModifierBits()
{
  QFETCH(uint32_t, mask);
  QFETCH(uint8_t, bits);
  QCOMPARE(mask_to_modifier_bits(mask), bits);
}

void BridgeCalibrationTests::capsEntryNeverCarriesShift()
{
  // The exact case from the hackintosh log: caps down with Shift held
  // (mask 0x1001) latched Shift onto the caps entry until Leave.
  QCOMPARE(key_down_modifier_bits(kKeyIdCapsLock, kMaskShift | kMaskCapsLock), uint8_t{0});
  QCOMPARE(key_down_modifier_bits(kKeyIdCapsLock, kMaskShift | kMaskControl | kMaskAlt | kMaskMeta), uint8_t{0});
  QCOMPARE(key_down_modifier_bits(kKeyIdCapsLock, 0), uint8_t{0});
  // A normal key still translates its mask.
  QCOMPARE(key_down_modifier_bits('k', kMaskShift), kHidLeftShift);
}

void BridgeCalibrationTests::capsMaskBitNeverBecomesModifier()
{
  for (uint32_t mask : {kMaskCapsLock, kMaskCapsLock | kMaskShift, kMaskCapsLock | kMaskControl}) {
    const uint8_t bits = mask_to_modifier_bits(mask);
    QCOMPARE(bits, mask_to_modifier_bits(mask & ~kMaskCapsLock));
  }
}

void BridgeCalibrationTests::desiredCapsFromMask()
{
  QVERIFY(!desired_caps_from_mask(0));
  QVERIFY(!desired_caps_from_mask(kMaskShift));
  QVERIFY(desired_caps_from_mask(kMaskCapsLock));
  QVERIFY(desired_caps_from_mask(kMaskCapsLock | kMaskShift));
}

void BridgeCalibrationTests::capsDecisionTable_data()
{
  QTest::addColumn<int>("truth"); // -1 unknown, 0 off, 1 on
  QTest::addColumn<bool>("desired");
  QTest::addColumn<bool>("emit");
  QTest::newRow("unknown/off -> emit") << -1 << false << true;
  QTest::newRow("unknown/on -> emit") << -1 << true << true;
  QTest::newRow("off/on -> emit") << 0 << true << true;
  QTest::newRow("on/off -> emit") << 1 << false << true;
  QTest::newRow("off/off -> skip") << 0 << false << false;
  QTest::newRow("on/on -> skip") << 1 << true << false;
}

void BridgeCalibrationTests::capsDecisionTable()
{
  QFETCH(int, truth);
  QFETCH(bool, desired);
  QFETCH(bool, emit);
  std::optional<bool> t;
  if (truth >= 0)
    t = (truth == 1);
  QCOMPARE(caps_edge_needed(t, desired), emit);
}

void BridgeCalibrationTests::chunk400Is50x8()
{
  const auto steps = chunk_delta(400);
  QCOMPARE(steps.size(), size_t{50});
  for (int8_t s : steps)
    QCOMPARE(int(s), 8);
  QCOMPARE(std::accumulate(steps.begin(), steps.end(), 0), 400);
}

void BridgeCalibrationTests::chunkPreservesSign()
{
  const auto steps = chunk_delta(-400);
  QCOMPARE(steps.size(), size_t{50});
  for (int8_t s : steps)
    QCOMPARE(int(s), -8);
  QCOMPARE(std::accumulate(steps.begin(), steps.end(), 0), -400);
}

void BridgeCalibrationTests::chunkZero()
{
  QVERIFY(chunk_delta(0).empty());
}

void BridgeCalibrationTests::chunkRemainder()
{
  const auto steps = chunk_delta(19);
  QCOMPARE(steps.size(), size_t{3});
  QCOMPARE(int(steps[0]), 8);
  QCOMPARE(int(steps[1]), 8);
  QCOMPARE(int(steps[2]), 3);
  const auto neg = chunk_delta(-3);
  QCOMPARE(neg.size(), size_t{1});
  QCOMPARE(int(neg[0]), -3);
}

void BridgeCalibrationTests::chunkXyLockstep()
{
  // +400 x, +300 y (the calibration probe): 50 reports, y runs out first.
  const auto steps = chunk_delta_xy(400, 300);
  QCOMPARE(steps.size(), size_t{50});
  int sx = 0, sy = 0;
  for (const Step &s : steps) {
    QVERIFY(std::abs(int(s.dx)) <= kMaxChunk);
    QVERIFY(std::abs(int(s.dy)) <= kMaxChunk);
    sx += s.dx;
    sy += s.dy;
  }
  QCOMPARE(sx, 400);
  QCOMPARE(sy, 300);
  QCOMPARE(int(steps[37].dy), 4); // 37*8 = 296, remainder 4
  QCOMPARE(int(steps[38].dy), 0);
  // Mixed signs stay independent per axis.
  const auto mixed = chunk_delta_xy(-9, 9);
  QCOMPARE(mixed.size(), size_t{2});
  QCOMPARE(int(mixed[0].dx), -8);
  QCOMPARE(int(mixed[0].dy), 8);
  QCOMPARE(int(mixed[1].dx), -1);
  QCOMPARE(int(mixed[1].dy), 1);
  // The slam path uses 127-count reports.
  QCOMPARE(chunk_delta_xy(254, 0, 127).size(), size_t{2});
}

void BridgeCalibrationTests::chunkBadMaxFallsBack()
{
  QCOMPARE(chunk_delta(16, 0).size(), size_t{2});
  QCOMPARE(chunk_delta(16, 200).size(), size_t{2});
  QCOMPARE(chunk_delta(16, -5).size(), size_t{2});
}

void BridgeCalibrationTests::countsPerPointBasic()
{
  // 400 counts moved the cursor 50 points -> 8 counts/point (2x Retina, x4).
  const auto s = counts_per_point(400, 50.0);
  QVERIFY(s.has_value());
  QCOMPARE(*s, 8.0);
  // Accelerated: 400 counts moved 200 points -> 2.
  QCOMPARE(*counts_per_point(400, 200.0), 2.0);
  // Negative probe with negative travel is fine.
  QCOMPARE(*counts_per_point(-300, -37.5), 8.0);
}

void BridgeCalibrationTests::countsPerPointRejectsTinyOrWrongSign()
{
  QVERIFY(!counts_per_point(0, 50.0));
  QVERIFY(!counts_per_point(400, 0.0));
  QVERIFY(!counts_per_point(400, 4.9));   // below kMinObservedPoints
  QVERIFY(!counts_per_point(400, -50.0)); // cursor went the wrong way
}

void BridgeCalibrationTests::countsPerPointRejectsAbsurdScale()
{
  QVERIFY(!counts_per_point(400, 4000.0)); // 0.1 c/pt < kMinScale
  QVERIFY(!counts_per_point(32000, 5.0));  // 6400 c/pt > kMaxScale
  QVERIFY(counts_per_point(320, 5.0));     // 64 == kMaxScale, allowed
}

void BridgeCalibrationTests::combineAxes()
{
  QCOMPARE(*combine_axis_scales(8.0, 6.0), 7.0);
  QCOMPARE(*combine_axis_scales(8.0, std::nullopt), 8.0);
  QCOMPARE(*combine_axis_scales(std::nullopt, 6.0), 6.0);
  QVERIFY(!combine_axis_scales(std::nullopt, std::nullopt));
}

void BridgeCalibrationTests::pointsToCountsRounds()
{
  QCOMPARE(points_to_counts(10, 8.0), 80);
  QCOMPARE(points_to_counts(3, 2.5), 8); // 7.5 -> 8
  QCOMPARE(points_to_counts(-3, 2.5), -8);
  QCOMPARE(points_to_counts(1, 0.4), 0);
  QCOMPARE(points_to_counts(2, 0.4), 1);
}

void BridgeCalibrationTests::residualBounded()
{
  // Wanted 500, landed at 490 -> carry +10.
  QCOMPARE(bounded_residual(500, 490), 10);
  QCOMPARE(bounded_residual(490, 500), -10);
  // Beyond the carry bound but below the reject threshold -> clamped.
  QCOMPARE(bounded_residual(600, 500), kMaxResidualPoints);
  QCOMPARE(bounded_residual(500, 600), -kMaxResidualPoints);
  QCOMPARE(bounded_residual(500 + kMaxResidualPoints, 500), kMaxResidualPoints);
}

void BridgeCalibrationTests::residualRejectsHuge()
{
  // A slam or a failed read: not a residual, drop it entirely.
  QCOMPARE(bounded_residual(2000, 0), 0);
  QCOMPARE(bounded_residual(0, 2000), 0);
  QCOMPARE(bounded_residual(kResidualRejectPoints + 1, 0), 0);
  QCOMPARE(bounded_residual(kResidualRejectPoints, 0), kMaxResidualPoints);
}

void BridgeCalibrationTests::residualZeroWhenOnTarget()
{
  QCOMPARE(bounded_residual(123, 123), 0);
  QCOMPARE(bounded_residual(0, 0), 0);
}

QTEST_MAIN(BridgeCalibrationTests)
#include "BridgeCalibrationTests.moc"
