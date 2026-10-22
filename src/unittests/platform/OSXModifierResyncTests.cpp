/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "OSXModifierResyncTests.h"

#include "base/EventQueue.h"
#include "deskflow/KeyMap.h"
#include "platform/OSXInjectedEvent.h"
#include "platform/OSXKeyState.h"
#include "platform/OSXModifierResync.h"
#include "platform/OSXScreen.h"

#include <ApplicationServices/ApplicationServices.h>
#include <QTest>

using namespace deskflow::platform;

namespace {
constexpr uint64_t kLCmd = kFlagCommand | 0x0008;
constexpr uint64_t kRCmd = kFlagCommand | 0x0010;
constexpr uint64_t kLShift = kFlagShift | 0x0002;
constexpr uint64_t kRShift = kFlagShift | 0x0004;
constexpr uint64_t kLAlt = kFlagAlternate | 0x0020;
constexpr uint64_t kRAlt = kFlagAlternate | 0x0040;
constexpr uint64_t kLCtl = kFlagControl | 0x0001;
constexpr uint64_t kRCtl = kFlagControl | 0x2000;
constexpr uint64_t kNC = 0x100; // always-on non-coalesced bit

struct FakeOs
{
  uint64_t hid = kNC;
  uint64_t session = kNC;
  bool adopt = true; // session adopts each post (as the OS does)
  std::vector<ModifierResyncStep> posts;
  std::function<void()> beforeEachRead; // simulate user acting mid-resync

  PrimaryModifierResync driver(ResyncEchoFilter *echo)
  {
    PrimaryModifierResync r;
    r.readHid = [this] {
      if (beforeEachRead) {
        beforeEachRead();
      }
      return hid;
    };
    r.readSession = [this] { return session; };
    r.postFlags = [this](uint16_t kc, bool down, uint64_t flags) {
      posts.push_back({kc, down, flags});
      if (adopt) {
        session = flags;
      }
      return true;
    };
    r.echo = echo;
    return r;
  }
};
} // namespace

void OSXModifierResyncTests::initTestCase()
{
  m_arch.init();
}

void OSXModifierResyncTests::agreementPostsNothing()
{
  QVERIFY(!primaryModifierResyncFlags(kNC, kNC));
  QVERIFY(!primaryModifierResyncFlags(kNC | kLCmd, kNC | kLCmd));
  QVERIFY(primaryModifierResyncSteps(kNC | kLShift, kNC | kLShift).empty());
}

// 2026-10-14 14:19 / 14:40 repro: Cmd held on the Hackintosh, cursor crosses
// to the MacBook, Cmd released there; the primary's tap swallowed that
// flagsChanged, session still says Cmd, HID says up. Return -> release.
void OSXModifierResyncTests::cmdHeldAcrossToClientReleasedThere_releasesOnReturn()
{
  const auto steps = primaryModifierResyncSteps(kNC, kNC | kLCmd);
  QCOMPARE(steps.size(), size_t{1});
  QCOMPARE(steps[0].keycode, uint16_t{55});
  QCOMPARE(steps[0].keyDown, false);
  QCOMPARE(steps[0].flags, kNC);
}

void OSXModifierResyncTests::cmdStillHeldWhenReturning_assertsCmd()
{
  const auto steps = primaryModifierResyncSteps(kNC | kLCmd, kNC);
  QCOMPARE(steps.size(), size_t{1});
  QCOMPARE(steps[0].keyDown, true);
  QCOMPARE(steps[0].flags, kNC | kLCmd);
}

void OSXModifierResyncTests::everyModifierReleasedOnReturn()
{
  const auto steps = primaryModifierResyncSteps(kNC, kNC | kLCmd | kLShift | kLAlt | kLCtl);
  QCOMPARE(steps.size(), size_t{4});
  QCOMPARE(steps.back().flags, kNC);
  for (const auto &s : steps) {
    QVERIFY(!s.keyDown);
  }
}

void OSXModifierResyncTests::capsLockAndFnUntouched()
{
  // Caps on + Fn in session, stale Cmd; HID lacks Caps/Fn: only Cmd moves
  const auto f = primaryModifierResyncFlags(kNC, kNC | kFlagAlphaShift | kFlagSecondaryFn | kLCmd);
  QVERIFY(f.has_value());
  QCOMPARE(*f, kNC | kFlagAlphaShift | kFlagSecondaryFn);
  QVERIFY(!primaryModifierResyncFlags(kNC | kFlagAlphaShift, kNC));
  QVERIFY(!primaryModifierResyncFlags(kNC, kNC | kFlagSecondaryFn));
  QVERIFY(!primaryModifierResyncFlags(kNC | kFlagSecondaryFn, kNC));
}

void OSXModifierResyncTests::rightSideBitsUseRightKeycodes()
{
  QCOMPARE(primaryModifierResyncSteps(kNC, kNC | kRCmd).at(0).keycode, uint16_t{54});
  QCOMPARE(primaryModifierResyncSteps(kNC, kNC | kRShift).at(0).keycode, uint16_t{60});
  QCOMPARE(primaryModifierResyncSteps(kNC, kNC | kRAlt).at(0).keycode, uint16_t{61});
  QCOMPARE(primaryModifierResyncSteps(kNC, kNC | kRCtl).at(0).keycode, uint16_t{62});
  QCOMPARE(primaryModifierResyncSteps(kNC, kNC | kLCtl).at(0).keycode, uint16_t{59});
}

void OSXModifierResyncTests::bothSidesChanged_twoSteps()
{
  // session: both Cmds stale. Two steps; Cmd generic stays until the last.
  const auto steps = primaryModifierResyncSteps(kNC, kNC | kLCmd | kRCmd);
  QCOMPARE(steps.size(), size_t{2});
  QCOMPARE(steps[0].keycode, uint16_t{55});
  QCOMPARE(steps[0].flags, kNC | kRCmd); // right still down -> generic kept
  QCOMPARE(steps[1].keycode, uint16_t{54});
  QCOMPARE(steps[1].flags, kNC);
  // swap: session left Cmd, HID right Cmd
  const auto swap = primaryModifierResyncSteps(kNC | kRCmd, kNC | kLCmd);
  QCOMPARE(swap.size(), size_t{2});
  QCOMPARE(swap[0].keyDown, false); // left released
  QCOMPARE(swap[1].keyDown, true);  // right pressed
  QCOMPARE(swap[1].flags, kNC | kRCmd);
}

void OSXModifierResyncTests::keyDownMatchesHidBit()
{
  const uint64_t hid = kNC | kLShift | kRAlt;
  const auto steps = primaryModifierResyncSteps(hid, kNC | kLCmd);
  QCOMPARE(steps.size(), size_t{3});
  for (const auto &s : steps) {
    const uint64_t dev = s.keycode == 55 ? 0x8 : s.keycode == 56 ? 0x2 : s.keycode == 61 ? 0x40 : 0;
    QVERIFY(dev != 0);
    QCOMPARE(s.keyDown, (hid & dev) != 0);
  }
}

void OSXModifierResyncTests::stepsEndAtTruth()
{
  const uint64_t hid = kNC | kLShift | kRCtl;
  const uint64_t session = kNC | kLCmd | kLAlt | kRShift | kFlagAlphaShift;
  const auto steps = primaryModifierResyncSteps(hid, session);
  QVERIFY(!steps.empty());
  QCOMPARE(steps.back().flags, *primaryModifierResyncFlags(hid, session));
  QCOMPARE(steps.back().flags & kHeldModifierFlags, hid & kHeldModifierFlags);
}

void OSXModifierResyncTests::run_postsAbsoluteFlagsThroughSink()
{
  ResyncEchoFilter echo;
  FakeOs os;
  os.session = kNC | kLCmd | kLAlt;
  const auto posted = os.driver(&echo).run();
  QCOMPARE(posted.size(), size_t{2});
  QCOMPARE(os.posts.back().flags, kNC);
  QCOMPARE(os.session, kNC);
  // each post was announced to the echo filter
  QVERIFY(echo.matches(kNC | kLAlt, ResyncEchoFilter::Clock::now()));
  QVERIFY(echo.matches(kNC, ResyncEchoFilter::Clock::now()));
}

void OSXModifierResyncTests::run_resamplesAndSkipsResolvedStep()
{
  // Session stale Cmd+Opt. After the first post (Cmd), the user physically
  // presses Opt: HID now agrees with session for Opt -> it must NOT be
  // posted (the old snapshot-then-post code would have released it).
  ResyncEchoFilter echo;
  FakeOs os;
  os.session = kNC | kLCmd | kLAlt;
  int reads = 0;
  os.beforeEachRead = [&] {
    if (++reads == 2) {
      os.hid = kNC | kLAlt;
    }
  };
  os.driver(&echo).run();
  QCOMPARE(os.posts.size(), size_t{1});
  QCOMPARE(os.posts[0].keycode, uint16_t{55});
  QCOMPARE(os.session & kHeldModifierFlags, kLAlt);
}

void OSXModifierResyncTests::run_boundedWhenSessionNeverAdopts()
{
  ResyncEchoFilter echo;
  FakeOs os;
  os.adopt = false;
  os.session = kNC | kLCmd | kRCmd | kLShift | kRShift | kLAlt | kRAlt | kLCtl | kRCtl;
  os.driver(&echo).run();
  QVERIFY(os.posts.size() <= 8); // each side at most once, never spins
  QCOMPARE(os.posts.size(), size_t{8}); // one post per side, then stops
}

void OSXModifierResyncTests::keyState_postModifierResync_usesIOHIDHookWithAbsoluteFlags()
{
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  OSXKeyState keyState(&eventQueue, keyMap, {"en"}, true);
  struct Rec
  {
    uint8_t vk = 0;
    bool down = true;
    CGEventFlags flags = 0;
    int n = 0;
  } rec;
  OSXKeyState::Hooks hooks;
  hooks.osModifierFlags = [] { return CGEventFlags(0); };
  hooks.postHIDKey = [&rec](uint8_t vk, bool down, CGEventFlags flags) {
    rec = {vk, down, flags, rec.n + 1};
    return KERN_SUCCESS;
  };
  keyState.setHooks(hooks);
  QVERIFY(keyState.postModifierResync(54, false, kNC));
  QCOMPARE(rec.n, 1);
  QCOMPARE(rec.vk, uint8_t{54});
  QCOMPARE(rec.down, false);
  QCOMPARE(uint64_t(rec.flags), kNC); // absolute word, not a delta
}

void OSXModifierResyncTests::tap_echoOfOwnPostIsRecognisedOnce()
{
  auto &echo = ResyncEchoFilter::instance();
  echo.expect(kNC | kRShift, ResyncEchoFilter::Clock::now());
  CGEventRef e = CGEventCreateKeyboardEvent(nullptr, 60, false);
  QVERIFY(e != nullptr);
  CGEventSetType(e, kCGEventFlagsChanged);
  CGEventSetFlags(e, CGEventFlags(kNC | kRShift));
  // the primary tap's gate: own echo -> pass to apps, no onKey / no relay
  QVERIFY(OSXScreen::isOwnModifierResyncEvent(kCGEventFlagsChanged, e));
  // consumed: a later identical HARDWARE event is not swallowed
  QVERIFY(!OSXScreen::isOwnModifierResyncEvent(kCGEventFlagsChanged, e));
  // non-flagsChanged never matches
  echo.expect(kNC | kRShift, ResyncEchoFilter::Clock::now());
  QVERIFY(!OSXScreen::isOwnModifierResyncEvent(kCGEventKeyDown, e));
  CFRelease(e);
}

void OSXModifierResyncTests::tap_dskrMarkerIsOwnAndInjected()
{
  CGEventRef e = CGEventCreateKeyboardEvent(nullptr, 55, false);
  QVERIFY(e != nullptr);
  CGEventSetType(e, kCGEventFlagsChanged);
  CGEventSetIntegerValueField(e, kCGEventSourceUserData, kModifierResyncMarker);
  QVERIFY(OSXScreen::isOwnModifierResyncEvent(kCGEventFlagsChanged, e));
  QVERIFY(isInjectedEvent(e)); // relay + local monitors skip it
  CFRelease(e);
}

void OSXModifierResyncTests::tap_hardwareFlagsChangedIsNotOwn()
{
  CGEventRef e = CGEventCreateKeyboardEvent(nullptr, 58, true);
  QVERIFY(e != nullptr);
  CGEventSetType(e, kCGEventFlagsChanged);
  CGEventSetFlags(e, CGEventFlags(kNC | kLAlt | 0x0400 /* unlikely word */));
  QVERIFY(!OSXScreen::isOwnModifierResyncEvent(kCGEventFlagsChanged, e));
  QVERIFY(!isInjectedEvent(e));
  CFRelease(e);
}

// -- stuck-Fn (2026-10-21): pure stray-modifier decision helpers -------------

void OSXModifierResyncTests::stray_eachWatchedBitUnexplainedIsStray()
{
  // nothing explained -> each watched bit present in the OS word is stray
  QCOMPARE(strayModifierBits(kFlagSecondaryFn, 0), kFlagSecondaryFn);
  QCOMPARE(strayModifierBits(kFlagControl, 0), kFlagControl);
  QCOMPARE(strayModifierBits(kFlagAlternate, 0), kFlagAlternate);
  QCOMPARE(strayModifierBits(kFlagShift, 0), kFlagShift);
  QCOMPARE(strayModifierBits(kFlagCommand, 0), kFlagCommand);
  // explained -> nothing stray
  QCOMPARE(strayModifierBits(kFlagCommand, kFlagCommand), uint64_t{0});
  // NumericPad on its own is not watched (it only rides with a cleared bit)
  QCOMPARE(strayModifierBits(kFlagNumericPad, 0), uint64_t{0});
}

void OSXModifierResyncTests::stray_deviceBackedModifierIsNotStray()
{
  // heldModifierFlags() returns the generic bit for a device-backed modifier
  // (a real key, this machine's own); the caller passes that as "explained".
  const uint64_t os = kFlagCommand | 0x0008 /* LCmd device */;
  QCOMPARE(strayModifierBits(os, kFlagCommand), uint64_t{0});
  // but the same generic bit with nothing explaining it is a ghost -> stray
  QCOMPARE(strayModifierBits(kFlagCommand, 0), kFlagCommand);
}

void OSXModifierResyncTests::stray_fnExplainedByHeldFunctionKeyOrPhysicalFn()
{
  // classifiers: function keys, the navigation cluster and the arrows carry Fn
  QVERIFY(isFnCarryingKey(0x7A));  // F1
  QVERIFY(isFnCarryingKey(0x5A));  // F20
  QVERIFY(isFnCarryingKey(0x72));  // Help / Insert
  QVERIFY(isFnCarryingKey(0x7B));  // Left arrow
  QVERIFY(!isFnCarryingKey(0x00)); // 'a'
  QVERIFY(!isFnCarryingKey(0x52)); // keypad 0: NumericPad, not Fn
  // a held Fn key (or the physical fn) is folded into "explained" by the
  // caller; then Fn is not stray even though the word carries it.
  const uint64_t os = kFlagSecondaryFn | kFlagNumericPad;
  QCOMPARE(strayModifierBits(os, kFlagSecondaryFn), uint64_t{0});
  QCOMPARE(strayModifierBits(os, 0), kFlagSecondaryFn);
}

void OSXModifierResyncTests::stray_capsLockIsNeverStray()
{
  QCOMPARE(strayModifierBits(kFlagAlphaShift, 0), uint64_t{0});
  QCOMPARE(strayModifierBits(kFlagAlphaShift | kFlagCommand, 0), kFlagCommand);
}

void OSXModifierResyncTests::stray_numericPadCleaning()
{
  const uint64_t os = kFlagSecondaryFn | kFlagNumericPad | 0x100 /* NonCoalesced */;
  // no keypad key held -> Fn and NumericPad both cleared, the rest kept
  QCOMPARE(cleanedModifierWord(os, kFlagSecondaryFn, false), uint64_t{0x100});
  // a keypad key held -> NumericPad rides along, only Fn is cleared
  QCOMPARE(cleanedModifierWord(os, kFlagSecondaryFn, true), uint64_t{kFlagNumericPad | 0x100});
  // keypad digits and the arrows are NumericPad keys; the F-keys are not
  QVERIFY(isNumericPadKey(0x52)); // keypad 0
  QVERIFY(isNumericPadKey(0x51)); // keypad equals
  QVERIFY(isNumericPadKey(0x7E)); // Up arrow
  QVERIFY(!isNumericPadKey(0x7A)); // F1
}

void OSXModifierResyncTests::stray_resyncKeycodePrefersFn()
{
  QCOMPARE(strayResyncKeycode(kFlagSecondaryFn), uint16_t{0x3F});
  QCOMPARE(strayResyncKeycode(kFlagSecondaryFn | kFlagCommand), uint16_t{0x3F});
  QCOMPARE(strayResyncKeycode(kFlagCommand), uint16_t{55});
  QCOMPARE(strayResyncKeycode(kFlagShift), uint16_t{56});
  QCOMPARE(strayResyncKeycode(kFlagAlternate), uint16_t{58});
  QCOMPARE(strayResyncKeycode(kFlagControl), uint16_t{59});
}

void OSXModifierResyncTests::stray_dwellRequiresSameSetTwice()
{
  StrayDwell d;
  QVERIFY(!d.sample(kFlagSecondaryFn)); // once is not enough
  QVERIFY(d.sample(kFlagSecondaryFn));  // same set twice -> trigger
  // a different set restarts the dwell
  QVERIFY(!d.sample(kFlagCommand));
  QVERIFY(d.sample(kFlagCommand));
  // a clean sample (nothing stray) resets it
  QVERIFY(!d.sample(0));
  QVERIFY(!d.sample(kFlagCommand)); // back to needing two in a row
}

QTEST_MAIN(OSXModifierResyncTests)
