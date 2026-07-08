/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "KeyboardRelayMapTests.h"

#include "coordination/CoordinationProtocol.h"
#include "coordination/KeyboardRelayMap.h"
#include "deskflow/KeyTypes.h"

#include <atomic>
#include <chrono>
#include <thread>

#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hidsystem/ev_keymap.h>

using deskflow::coordination::mapRelayMediaKeyFromCgEvent;
using deskflow::coordination::mediaKeyIdFromNxType;
using deskflow::coordination::Message;

void KeyboardRelayMapTests::mapRelayKeyFromCgEventOffMainThreadDoesNotCrash()
{
  CGEventRef event = CGEventCreateKeyboardEvent(nullptr, kVK_Space, true);
  QVERIFY(event != nullptr);

  std::atomic<bool> finished{false};
  std::atomic<bool> mapped{false};
  std::thread worker([&]() {
    Message::KeyPhase phase = Message::KeyPhase::Down;
    KeyID id = kKeyNone;
    KeyModifierMask mask = 0;
    KeyButton button = 0;
    mapped.store(
        deskflow::coordination::mapRelayKeyFromCgEvent(event, phase, id, mask, button), std::memory_order_relaxed
    );
    finished.store(true, std::memory_order_relaxed);
  });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!finished.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < deadline) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
  }
  if (!finished.load(std::memory_order_relaxed)) {
    worker.detach();
    QFAIL("mapRelayKeyFromCgEvent did not complete within 5 seconds");
  }
  worker.join();
  CFRelease(event);

  QVERIFY(finished.load(std::memory_order_relaxed));
  QVERIFY(mapped.load(std::memory_order_relaxed));
}

void KeyboardRelayMapTests::mediaKeyIdFromNxType_mapsConsumerKeys()
{
  QCOMPARE(mediaKeyIdFromNxType(NX_KEYTYPE_SOUND_UP), kKeyAudioUp);
  QCOMPARE(mediaKeyIdFromNxType(NX_KEYTYPE_SOUND_DOWN), kKeyAudioDown);
  QCOMPARE(mediaKeyIdFromNxType(NX_KEYTYPE_MUTE), kKeyAudioMute);
  QCOMPARE(mediaKeyIdFromNxType(NX_KEYTYPE_PLAY), kKeyAudioPlay);
  QCOMPARE(mediaKeyIdFromNxType(NX_KEYTYPE_NEXT), kKeyAudioNext);
  QCOMPARE(mediaKeyIdFromNxType(NX_KEYTYPE_PREVIOUS), kKeyAudioPrev);
  QCOMPARE(mediaKeyIdFromNxType(NX_KEYTYPE_BRIGHTNESS_UP), kKeyBrightnessUp);
  QCOMPARE(mediaKeyIdFromNxType(NX_KEYTYPE_BRIGHTNESS_DOWN), kKeyBrightnessDown);
  QCOMPARE(mediaKeyIdFromNxType(NX_KEYTYPE_EJECT), kKeyEject);
  // Unmapped consumer type must not be treated as a media key.
  QCOMPARE(mediaKeyIdFromNxType(NX_KEYTYPE_ILLUMINATION_UP), kKeyNone);
}

void KeyboardRelayMapTests::mapRelayMediaKeyFromCgEvent_ignoresPlainKey()
{
  // A standard keyboard event is not a system-defined media event, so the
  // media decoder must reject it (the standard-key path handles it instead).
  CGEventRef keyEvent = CGEventCreateKeyboardEvent(nullptr, kVK_ANSI_A, true);
  QVERIFY(keyEvent != nullptr);
  KeyID id = kKeyNone;
  bool down = false;
  QVERIFY(!mapRelayMediaKeyFromCgEvent(keyEvent, id, down));
  CFRelease(keyEvent);
}

void KeyboardRelayMapTests::modifierKeys_mapToFleetKeyIds()
{
  const auto expectDown = [](CGKeyCode vk, CGEventFlags flags, KeyID expected) {
    CGEventRef event = CGEventCreate(nullptr);
    QVERIFY(event != nullptr);
    CGEventSetType(event, kCGEventFlagsChanged);
    CGEventSetIntegerValueField(event, kCGKeyboardEventKeycode, vk);
    CGEventSetFlags(event, flags);

    Message::KeyPhase phase = Message::KeyPhase::Up;
    KeyID id = kKeyNone;
    KeyModifierMask mask = 0;
    KeyButton button = 0;
    QVERIFY(deskflow::coordination::mapRelayModifierFromCgEvent(event, phase, id, mask, button));
    QCOMPARE(phase, Message::KeyPhase::Down);
    QCOMPARE(id, expected);
    CFRelease(event);
  };

  expectDown(kVK_Command, kCGEventFlagMaskCommand, kKeySuper_L);
  expectDown(kVK_Control, kCGEventFlagMaskControl, kKeyControl_L);
  expectDown(kVK_Option, kCGEventFlagMaskAlternate, kKeyAlt_L);
  expectDown(kVK_Shift, kCGEventFlagMaskShift, kKeyShift_L);
  expectDown(kVK_RightCommand, kCGEventFlagMaskCommand, kKeySuper_R);
  expectDown(kVK_RightControl, kCGEventFlagMaskControl, kKeyControl_R);
  expectDown(kVK_RightOption, kCGEventFlagMaskAlternate, kKeyAlt_R);
  expectDown(kVK_RightShift, kCGEventFlagMaskShift, kKeyShift_R);
}

void KeyboardRelayMapTests::mapRelayModifierFromCgEvent_handlesFlagsChanged()
{
  const auto expectUp = [](CGKeyCode vk) {
    CGEventRef event = CGEventCreate(nullptr);
    QVERIFY(event != nullptr);
    CGEventSetType(event, kCGEventFlagsChanged);
    CGEventSetIntegerValueField(event, kCGKeyboardEventKeycode, vk);
    CGEventSetFlags(event, 0);

    Message::KeyPhase phase = Message::KeyPhase::Down;
    KeyID id = kKeySuper_L;
    KeyModifierMask mask = KeyModifierSuper;
    KeyButton button = 0;
    QVERIFY(deskflow::coordination::mapRelayModifierFromCgEvent(event, phase, id, mask, button));
    QCOMPARE(phase, Message::KeyPhase::Up);
    QCOMPARE(id, kKeyNone);
    QCOMPARE(mask, 0u);
    QCOMPARE(button, static_cast<KeyButton>(vk));
    CFRelease(event);
  };

  expectUp(kVK_Command);
  expectUp(kVK_Control);
  expectUp(kVK_Shift);
  expectUp(kVK_Option);
}

void KeyboardRelayMapTests::mapModifiers_useNeutralMaskBits()
{
  CGEventRef event = CGEventCreateKeyboardEvent(nullptr, kVK_ANSI_C, true);
  QVERIFY(event != nullptr);
  CGEventSetFlags(event, kCGEventFlagMaskCommand | kCGEventFlagMaskControl);

  Message::KeyPhase phase = Message::KeyPhase::Down;
  KeyID id = kKeyNone;
  KeyModifierMask mask = 0;
  KeyButton button = 0;
  QVERIFY(deskflow::coordination::mapRelayKeyFromCgEvent(event, phase, id, mask, button));
  QCOMPARE(mask, KeyModifierSuper | KeyModifierControl);
  CFRelease(event);
}

void KeyboardRelayMapTests::shiftedKey_translatesToShiftedKeyId()
{
  // Regression: the relay used to translate with zero modifier state, so
  // Shift+A relayed as 'a' and the target's KeyMap released Shift to
  // reproduce the lowercase glyph -- modifiers appeared dead on relayed
  // keyboards. The KeyID must reflect the live Shift state.
  const auto mapKey = [](CGKeyCode vk, CGEventFlags flags, KeyID &id, KeyModifierMask &mask) {
    CGEventRef event = CGEventCreateKeyboardEvent(nullptr, vk, true);
    QVERIFY(event != nullptr);
    CGEventSetFlags(event, flags);

    // mapRelayKeyFromCgEvent fetches layout data via the main queue, so run
    // it off-main and pump the run loop (same pattern as the crash test).
    std::atomic<bool> finished{false};
    std::atomic<bool> mapped{false};
    std::thread worker([&]() {
      Message::KeyPhase phase = Message::KeyPhase::Down;
      KeyButton button = 0;
      mapped.store(
          deskflow::coordination::mapRelayKeyFromCgEvent(event, phase, id, mask, button), std::memory_order_relaxed
      );
      finished.store(true, std::memory_order_relaxed);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!finished.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < deadline) {
      CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
    }
    if (!finished.load(std::memory_order_relaxed)) {
      worker.detach();
      QFAIL("mapRelayKeyFromCgEvent did not complete within 5 seconds");
    }
    worker.join();
    CFRelease(event);
    QVERIFY(mapped.load(std::memory_order_relaxed));
  };

  KeyID unshiftedId = kKeyNone;
  KeyModifierMask unshiftedMask = 0;
  mapKey(kVK_ANSI_A, 0, unshiftedId, unshiftedMask);

  KeyID shiftedId = kKeyNone;
  KeyModifierMask shiftedMask = 0;
  mapKey(kVK_ANSI_A, kCGEventFlagMaskShift, shiftedId, shiftedMask);

  KeyID capsLockedId = kKeyNone;
  KeyModifierMask capsLockedMask = 0;
  mapKey(kVK_ANSI_A, kCGEventFlagMaskAlphaShift, capsLockedId, capsLockedMask);

  KeyID shiftCapsId = kKeyNone;
  KeyModifierMask shiftCapsMask = 0;
  mapKey(kVK_ANSI_A, kCGEventFlagMaskShift | kCGEventFlagMaskAlphaShift, shiftCapsId, shiftCapsMask);

  if (QTest::currentTestFailed()) {
    return;
  }

  QVERIFY((shiftedMask & KeyModifierShift) != 0);
  QVERIFY(shiftedId != kKeyNone);
  // On any Latin layout the A key produces a lowercase letter unshifted and
  // its uppercase counterpart shifted or caps-locked. Unlike Windows, macOS
  // does NOT cancel CapsLock with Shift: Shift+CapsLock+A stays uppercase.
  if (unshiftedId >= 'a' && unshiftedId <= 'z') {
    QCOMPARE(shiftedId, static_cast<KeyID>(unshiftedId - 'a' + 'A'));
    QCOMPARE(capsLockedId, shiftedId);
    QCOMPARE(shiftCapsId, shiftedId);
  } else {
    QVERIFY(shiftedId != unshiftedId);
  }

  // Digit row: Shift changes the glyph but CapsLock must not. Only assert on
  // US-like layouts where the key produces '1' unshifted (e.g. French AZERTY
  // differs on both counts).
  KeyID digitId = kKeyNone;
  KeyModifierMask digitMask = 0;
  mapKey(kVK_ANSI_1, 0, digitId, digitMask);

  if (QTest::currentTestFailed()) {
    return;
  }

  if (digitId == static_cast<KeyID>('1')) {
    KeyID shiftedDigitId = kKeyNone;
    KeyModifierMask shiftedDigitMask = 0;
    mapKey(kVK_ANSI_1, kCGEventFlagMaskShift, shiftedDigitId, shiftedDigitMask);

    KeyID capsDigitId = kKeyNone;
    KeyModifierMask capsDigitMask = 0;
    mapKey(kVK_ANSI_1, kCGEventFlagMaskAlphaShift, capsDigitId, capsDigitMask);

    if (QTest::currentTestFailed()) {
      return;
    }
    QVERIFY(shiftedDigitId != digitId);
    QCOMPARE(capsDigitId, digitId);
  }
}

QTEST_MAIN(KeyboardRelayMapTests)
