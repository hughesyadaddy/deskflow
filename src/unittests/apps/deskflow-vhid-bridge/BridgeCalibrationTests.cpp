/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "BridgeCalibration.h"

#include <QTest>

#include <chrono>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>
#include <thread>

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
  void keyIdHelpers();
  void letterModifiers_data();
  void letterModifiers();

  // -- caps decision table -------------------------------------------------
  void capsDecisionTable_data();
  void capsDecisionTable();
  void capsSyncTable_data();
  void capsSyncTable();
  void capsAssumptionResolution_data();
  void capsAssumptionResolution();
  void derivedShiftIsNotHeld();

  // -- report drain (A-4) --------------------------------------------------
  void drainMarkerLandsBehindNestedJob();
  void drainMarkerRefusedQueue();
  void drainGateBounded();

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

void BridgeCalibrationTests::keyIdHelpers()
{
  QVERIFY(keyid_is_letter('a') && keyid_is_letter('z') && keyid_is_letter('A') && keyid_is_letter('Z'));
  QVERIFY(!keyid_is_letter('1') && !keyid_is_letter('!') && !keyid_is_letter(kKeyIdCapsLock));
  QVERIFY(keyid_is_upper_letter('K') && !keyid_is_upper_letter('k'));
  QVERIFY(keyid_requires_shift('K') && keyid_requires_shift('!') && keyid_requires_shift('?'));
  QVERIFY(!keyid_requires_shift('k') && !keyid_requires_shift('1') && !keyid_requires_shift(' '));
  QVERIFY(!keyid_requires_shift(kKeyIdCapsLock));
}

// The letter rule (BridgeCalibration.h, decide_letter_modifiers):
//   wantUpper = isUpper(id) || (Shift && !Caps); shift = wantUpper XOR Caps;
//   capsEdge  = local caps known && local != server's caps bit.
// local: -1 unknown, 0 off, 1 on.
void BridgeCalibrationTests::letterModifiers_data()
{
  QTest::addColumn<uint16_t>("id");
  QTest::addColumn<uint32_t>("mask");
  QTest::addColumn<int>("local");
  QTest::addColumn<uint8_t>("bits");
  QTest::addColumn<bool>("edge");
  const uint8_t none = 0;
  // The rows from the fleet-hardening plan.
  QTest::newRow("0x0044 'D' / shift -> shift") << uint16_t{0x0044} << kMaskShift << -1 << kHidLeftShift << false;
  QTest::newRow("0x0021 '!' / shift -> shift (non-letter)")
      << uint16_t{0x0021} << kMaskShift << -1 << kHidLeftShift << false;
  QTest::newRow("'k' / shift -> shift (base id, shift held)")
      << uint16_t{'k'} << kMaskShift << -1 << kHidLeftShift << false;
  QTest::newRow("'K' / none -> shift (caps-composed upper)")
      << uint16_t{'K'} << uint32_t{0} << -1 << kHidLeftShift << false;
  QTest::newRow("'K' / caps -> no shift") << uint16_t{'K'} << kMaskCapsLock << -1 << none << false;
  QTest::newRow("'K' / caps, local off -> no shift + edge") << uint16_t{'K'} << kMaskCapsLock << 0 << none << true;
  QTest::newRow("'K' / caps, local on -> no shift, no edge") << uint16_t{'K'} << kMaskCapsLock << 1 << none << false;
  QTest::newRow("'K' / none, local on -> shift + edge") << uint16_t{'K'} << uint32_t{0} << 1 << kHidLeftShift << true;
  QTest::newRow("'K' / none, local off -> shift, no edge")
      << uint16_t{'K'} << uint32_t{0} << 0 << kHidLeftShift << false;
  QTest::newRow("'k' / caps -> shift (caps+shift composes lowercase)")
      << uint16_t{'k'} << kMaskCapsLock << -1 << kHidLeftShift << false;
  // Shift+Caps on the server composed 'k': with the target's caps on (M=1)
  // lowercase needs Shift held, so the byte is 0x02 -- the plan's row said
  // 0x00, which with caps on would type 'K'; the rule's algebra
  // (shift = wantUpper XOR M = 0 XOR 1) is what keeps it lowercase.
  QTest::newRow("'k' / shift+caps (0x1001) -> shift")
      << uint16_t{'k'} << (kMaskShift | kMaskCapsLock) << -1 << kHidLeftShift << false;
  QTest::newRow("'k' / none -> none") << uint16_t{'k'} << uint32_t{0} << -1 << none << false;
  QTest::newRow("'k' / none, local on -> none + edge") << uint16_t{'k'} << uint32_t{0} << 1 << none << true;
  QTest::newRow("CapsLock id / caps -> none, no edge") << kKeyIdCapsLock << kMaskCapsLock << 0 << none << false;
  QTest::newRow("CapsLock id / shift+caps -> none, no edge")
      << kKeyIdCapsLock << (kMaskShift | kMaskCapsLock) << 1 << none << false;
  // Other modifiers pass through for letters; the mask's Shift does not.
  QTest::newRow("ctrl+'k' keeps ctrl") << uint16_t{'k'} << kMaskControl << -1 << kHidLeftControl << false;
  QTest::newRow("ctrl+shift+'K' keeps ctrl, shift from rule")
      << uint16_t{'K'} << (kMaskControl | kMaskShift) << -1 << uint8_t(kHidLeftControl | kHidLeftShift) << false;
  QTest::newRow("ctrl+'K' / caps -> ctrl only")
      << uint16_t{'K'} << (kMaskControl | kMaskCapsLock) << 1 << kHidLeftControl << false;
  QTest::newRow("alt+super+'k' -> option+command")
      << uint16_t{'k'} << (kMaskAlt | kMaskSuper) << -1 << uint8_t(kHidLeftOption | kHidLeftCommand) << false;
  // Non-letters ignore caps entirely: mask bits, plus shift for shifted symbols.
  QTest::newRow("'1' / caps, local off -> none, no edge") << uint16_t{'1'} << kMaskCapsLock << 0 << none << false;
  QTest::newRow("'!' / none -> shift implied") << uint16_t{'!'} << uint32_t{0} << -1 << kHidLeftShift << false;
  QTest::newRow("ctrl+'1' keeps ctrl") << uint16_t{'1'} << kMaskControl << 1 << kHidLeftControl << false;
  QTest::newRow("Left arrow / shift keeps shift (selection)")
      << uint16_t{0xEF51} << kMaskShift << -1 << kHidLeftShift << false;
}

void BridgeCalibrationTests::letterModifiers()
{
  QFETCH(uint16_t, id);
  QFETCH(uint32_t, mask);
  QFETCH(int, local);
  QFETCH(uint8_t, bits);
  QFETCH(bool, edge);
  std::optional<bool> localCaps;
  if (local >= 0)
    localCaps = (local == 1);
  const LetterDecision d = decide_letter_modifiers(id, mask, localCaps);
  QCOMPARE(d.modifierBits, bits);
  QCOMPARE(d.capsEdge, edge);
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

// Enter / mask-only sync vs a real caps press with the truth unknown: the
// press edges (best effort), the sync must NOT (it would toggle the real
// lock and invert every following letter).
void BridgeCalibrationTests::capsSyncTable_data()
{
  QTest::addColumn<int>("truth"); // -1 unknown, 0 off, 1 on
  QTest::addColumn<bool>("desired");
  QTest::addColumn<bool>("syncEmit");
  QTest::addColumn<bool>("pressEmit");
  QTest::newRow("unknown/off: sync skip, press edge") << -1 << false << false << true;
  QTest::newRow("unknown/on: sync skip, press edge") << -1 << true << false << true;
  QTest::newRow("off/on: both edge") << 0 << true << true << true;
  QTest::newRow("on/off: both edge") << 1 << false << true << true;
  QTest::newRow("off/off: both skip") << 0 << false << false << false;
  QTest::newRow("on/on: both skip") << 1 << true << false << false;
}

void BridgeCalibrationTests::capsSyncTable()
{
  QFETCH(int, truth);
  QFETCH(bool, desired);
  QFETCH(bool, syncEmit);
  QFETCH(bool, pressEmit);
  std::optional<bool> t;
  if (truth >= 0)
    t = (truth == 1);
  QCOMPARE(caps_sync_edge_needed(t, desired), syncEmit);
  QCOMPARE(caps_edge_needed(t, desired), pressEmit);
}

// A-5: after an emitted edge the bridge assumes the state it set UNTIL the
// OS reader agrees, bounded by kCapsAssumeMaxMs. reader: -1 unreadable,
// 0 off, 1 on; the assumed state is always `on` here.
void BridgeCalibrationTests::capsAssumptionResolution_data()
{
  QTest::addColumn<int>("reader");
  QTest::addColumn<int64_t>("ageMs");
  QTest::addColumn<int>("result"); // 0 Hold, 1 Confirmed, 2 Expired
  QTest::newRow("agrees at once") << 1 << int64_t{0} << 1;
  QTest::newRow("agrees late") << 1 << int64_t{250} << 1;
  QTest::newRow("agrees after the bound") << 1 << int64_t{5000} << 1;
  QTest::newRow("stale at 0") << 0 << int64_t{0} << 0;
  QTest::newRow("stale at 10") << 0 << int64_t{10} << 0;
  QTest::newRow("stale at 40") << 0 << int64_t{40} << 0;
  QTest::newRow("stale at 80 (old 50 ms timer would re-edge)") << 0 << int64_t{80} << 0;
  QTest::newRow("stale at 299") << 0 << int64_t{299} << 0;
  QTest::newRow("stale at 300 -> expired") << 0 << int64_t{300} << 2;
  QTest::newRow("stale much later -> expired") << 0 << int64_t{4000} << 2;
  QTest::newRow("unreadable within") << -1 << int64_t{100} << 0;
  QTest::newRow("unreadable at bound -> expired") << -1 << int64_t{300} << 2;
  QTest::newRow("clock went backwards -> expired") << 0 << int64_t{-1} << 2;
}

void BridgeCalibrationTests::capsAssumptionResolution()
{
  QFETCH(int, reader);
  QFETCH(int64_t, ageMs);
  QFETCH(int, result);
  const std::optional<bool> r = reader < 0 ? std::optional<bool>{} : std::optional<bool>{reader == 1};
  const auto got = resolve_caps_assumption(true, r, 1000 + ageMs, 1000);
  QCOMPARE(int(got), result);
  QCOMPARE(int64_t{kCapsAssumeMaxMs}, int64_t{300});
}

// A stale reader for 80 ms yields exactly one edge: simulate the bridge's
// per-key decision over a burst after one edge at t=0.
void BridgeCalibrationTests::derivedShiftIsNotHeld()
{
  // A-1: the derived Shift rides on the key-down report (modifierBits),
  // never on the ledger entry (heldBits); real modifiers stay on both.
  auto d = decide_letter_modifiers('K', 0, std::nullopt);
  QCOMPARE(d.modifierBits, kHidLeftShift);
  QCOMPARE(d.heldBits, uint8_t{0});
  d = decide_letter_modifiers('k', kMaskShift, std::nullopt); // the mask's Shift is not stored on a letter either
  QCOMPARE(d.modifierBits, kHidLeftShift);
  QCOMPARE(d.heldBits, uint8_t{0});
  d = decide_letter_modifiers('k', kMaskCapsLock, std::nullopt); // caps on: lowercase needs Shift, one report only
  QCOMPARE(d.modifierBits, kHidLeftShift);
  QCOMPARE(d.heldBits, uint8_t{0});
  d = decide_letter_modifiers('k', kMaskControl, std::nullopt); // ctrl is real: held
  QCOMPARE(d.modifierBits, kHidLeftControl);
  QCOMPARE(d.heldBits, kHidLeftControl);
  d = decide_letter_modifiers('!', 0, std::nullopt); // shifted symbol: derived
  QCOMPARE(d.modifierBits, kHidLeftShift);
  QCOMPARE(d.heldBits, uint8_t{0});
  d = decide_letter_modifiers('!', kMaskShift, std::nullopt); // the mask's Shift on a non-letter is real
  QCOMPARE(d.modifierBits, kHidLeftShift);
  QCOMPARE(d.heldBits, kHidLeftShift);
  d = decide_letter_modifiers('1', kMaskShift | kMaskAlt, std::nullopt);
  QCOMPARE(d.modifierBits, uint8_t(kHidLeftShift | kHidLeftOption));
  QCOMPARE(d.heldBits, uint8_t(kHidLeftShift | kHidLeftOption));

  // one edge for a stale burst: edge at t=0 (assumed on), reader says off
  // at 10/40/80 ms -> Hold each time (no re-edge), agrees at 100 -> done.
  int edges = 1;
  bool assumed = true;
  for (int64_t t : {10, 40, 80}) {
    if (resolve_caps_assumption(assumed, false, t, 0) != CapsAssumption::Hold) {
      ++edges;
    }
  }
  QCOMPARE(resolve_caps_assumption(assumed, true, 100, 0), CapsAssumption::Confirmed);
  QCOMPARE(edges, 1);
}

// Simulated pqrs dispatcher: a FIFO whose report job appends a second job
// (the local_datagram hop) that finally "sends". One hop is not enough;
// kReportDrainHops (2) puts the marker behind the send.
void BridgeCalibrationTests::drainMarkerLandsBehindNestedJob()
{
  for (int hops : {1, kReportDrainHops}) {
    std::deque<std::function<void()>> fifo;
    auto enqueue = [&fifo](std::function<void()> fn) {
      fifo.push_back(std::move(fn));
      return true;
    };
    bool sent = false;
    bool sentWhenMarkerFired = false;
    bool fired = false;
    enqueue([&] { enqueue([&] { sent = true; }); }); // report: job 1 enqueues job 2
    QVERIFY(enqueue_drain_marker(enqueue, hops, [&] {
      fired = true;
      sentWhenMarkerFired = sent;
    }));
    while (!fifo.empty()) {
      auto job = std::move(fifo.front());
      fifo.pop_front();
      job();
    }
    QVERIFY(fired);
    QVERIFY(sent);
    QCOMPARE(sentWhenMarkerFired, hops >= kReportDrainHops);
  }
}

void BridgeCalibrationTests::drainMarkerRefusedQueue()
{
  bool fired = false;
  auto refuse = [](std::function<void()>) { return false; };
  QVERIFY(!enqueue_drain_marker(refuse, kReportDrainHops, [&] { fired = true; }));
  QVERIFY(!fired);
  // zero hops completes synchronously without touching the queue
  QVERIFY(enqueue_drain_marker(refuse, 0, [&] { fired = true; }));
  QVERIFY(fired);
}

void BridgeCalibrationTests::drainGateBounded()
{
  {
    DrainGate gate;
    gate.complete();
    QCOMPARE(gate.wait(kReportDrainBoundMs), DrainOutcome::Drained);
  }
  {
    DrainGate gate;
    const auto t0 = std::chrono::steady_clock::now();
    QCOMPARE(gate.wait(30), DrainOutcome::TimedOut);
    QVERIFY(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(30));
  }
  {
    auto gate = std::make_shared<DrainGate>();
    std::thread worker([gate] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      gate->complete();
    });
    QCOMPARE(gate->wait(kReportDrainBoundMs), DrainOutcome::Drained);
    worker.join();
    gate->complete(); // idempotent
    QCOMPARE(gate->wait(1), DrainOutcome::Drained);
  }
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
