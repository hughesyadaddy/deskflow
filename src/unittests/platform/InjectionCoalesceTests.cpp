/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "InjectionCoalesceTests.h"

#include "platform/InjectionCoalesce.h"

#include <QTest>

#include <limits>

using deskflow::platform::clampMoveDelta;
using deskflow::platform::kMaxCoalescedMoves;

void InjectionCoalesceTests::clampMoveDelta_passesNormalValues()
{
  QCOMPARE(clampMoveDelta(0), 0);
  QCOMPARE(clampMoveDelta(-37), -37);
  QCOMPARE(clampMoveDelta(1920), 1920);
  QCOMPARE(clampMoveDelta(std::numeric_limits<int32_t>::max()), std::numeric_limits<int32_t>::max());
  QCOMPARE(clampMoveDelta(std::numeric_limits<int32_t>::min()), std::numeric_limits<int32_t>::min());
}

void InjectionCoalesceTests::clampMoveDelta_clampsOverflow()
{
  // Summed relative deltas (the in-game aim path) must saturate, not wrap:
  // a wrapped delta would fling the view in the opposite direction.
  const int64_t hugePositive = static_cast<int64_t>(std::numeric_limits<int32_t>::max()) * 3;
  const int64_t hugeNegative = static_cast<int64_t>(std::numeric_limits<int32_t>::min()) * 3;
  QCOMPARE(clampMoveDelta(hugePositive), std::numeric_limits<int32_t>::max());
  QCOMPARE(clampMoveDelta(hugeNegative), std::numeric_limits<int32_t>::min());
}

void InjectionCoalesceTests::coalesceCap_isBounded()
{
  // The desk-thread drain loop must be bounded so a sustained flood
  // cannot delay the injection itself.
  QVERIFY(kMaxCoalescedMoves > 0);
  QVERIFY(kMaxCoalescedMoves <= 256);
}

QTEST_MAIN(InjectionCoalesceTests)
