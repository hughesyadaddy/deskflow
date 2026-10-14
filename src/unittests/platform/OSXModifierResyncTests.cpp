/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "OSXModifierResyncTests.h"

#include "platform/OSXModifierResync.h"

#include <QTest>

using namespace deskflow::platform;

namespace {
constexpr uint64_t kLCmd = kFlagCommand | 0x0008;
constexpr uint64_t kLShift = kFlagShift | 0x0002;
constexpr uint64_t kLAlt = kFlagAlternate | 0x0020;
constexpr uint64_t kLCtl = kFlagControl | 0x0001;
constexpr uint64_t kNonCoalesced = 0x100; // always-on bit in real flags words
} // namespace

void OSXModifierResyncTests::agreementPostsNothing()
{
  QVERIFY(!primaryModifierResyncFlags(kNonCoalesced, kNonCoalesced));
  QVERIFY(!primaryModifierResyncFlags(kNonCoalesced | kLCmd, kNonCoalesced | kLCmd));
  QVERIFY(primaryModifierResyncSteps(kNonCoalesced | kLShift, kNonCoalesced | kLShift).empty());
}

// 2026-10-14 14:19 repro: Cmd held on the Hackintosh, cursor crosses to the
// MacBook, Cmd released there. The primary's tap swallowed that flagsChanged
// so its session still says Cmd; HID says up. Return -> Cmd must be released.
void OSXModifierResyncTests::cmdHeldAcrossToClientReleasedThere_releasesOnReturn()
{
  const auto flags = primaryModifierResyncFlags(kNonCoalesced, kNonCoalesced | kLCmd);
  QVERIFY(flags.has_value());
  QCOMPARE(*flags & kHeldModifierFlags, uint64_t{0});
  const auto steps = primaryModifierResyncSteps(kNonCoalesced, kNonCoalesced | kLCmd);
  QCOMPARE(steps.size(), size_t{1});
  QCOMPARE(steps[0].keycode, uint16_t{55});
  QCOMPARE(steps[0].flags, uint64_t{kNonCoalesced});
}

// Other direction: Cmd pressed while on the MacBook (swallowed here), still
// physically held when the cursor comes home -> session gains Cmd.
void OSXModifierResyncTests::cmdStillHeldWhenReturning_assertsCmd()
{
  const auto flags = primaryModifierResyncFlags(kNonCoalesced | kLCmd, kNonCoalesced);
  QVERIFY(flags.has_value());
  QCOMPARE(*flags, kNonCoalesced | kLCmd);
}

void OSXModifierResyncTests::everyModifierReleasedOnReturn()
{
  const uint64_t stale = kNonCoalesced | kLCmd | kLShift | kLAlt | kLCtl | kFlagSecondaryFn;
  const auto steps = primaryModifierResyncSteps(kNonCoalesced, stale);
  QCOMPARE(steps.size(), size_t{5});
  QCOMPARE(steps.back().flags, uint64_t{kNonCoalesced});
  QCOMPARE(steps[4].keycode, uint16_t{63}); // Fn last
}

void OSXModifierResyncTests::capsLockStateUntouched()
{
  // session has Caps on + stale Cmd; HID word lacks Caps (lock state is not
  // a held key): Caps must survive, Cmd must go.
  const auto flags = primaryModifierResyncFlags(kNonCoalesced, kNonCoalesced | kFlagAlphaShift | kLCmd);
  QVERIFY(flags.has_value());
  QCOMPARE(*flags, kNonCoalesced | kFlagAlphaShift);
  // Caps difference alone is never a resync
  QVERIFY(!primaryModifierResyncFlags(kNonCoalesced | kFlagAlphaShift, kNonCoalesced));
}

void OSXModifierResyncTests::stepsCarryModifierKeycodesAndEndAtTruth()
{
  const uint64_t hid = kNonCoalesced | kLShift;           // still holding Shift
  const uint64_t session = kNonCoalesced | kLCmd | kLAlt; // Cmd+Opt stale
  const auto steps = primaryModifierResyncSteps(hid, session);
  QCOMPARE(steps.size(), size_t{3});
  QCOMPARE(steps[0].keycode, uint16_t{55});
  QCOMPARE(steps[1].keycode, uint16_t{56});
  QCOMPARE(steps[2].keycode, uint16_t{58});
  QCOMPARE(steps.back().flags, *primaryModifierResyncFlags(hid, session));
  QCOMPARE(steps.back().flags & kHeldModifierFlags, kLShift);
}

void OSXModifierResyncTests::leftRightDeviceBitsFollowHid()
{
  // right Cmd held physically, session thinks left Cmd
  const uint64_t hid = kNonCoalesced | kFlagCommand | 0x0010;
  const auto flags = primaryModifierResyncFlags(hid, kNonCoalesced | kLCmd);
  QVERIFY(flags.has_value());
  QCOMPARE(*flags, hid);
}

QTEST_MAIN(OSXModifierResyncTests)
