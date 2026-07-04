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

#include <CoreFoundation/CoreFoundation.h>
#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include <IOKit/hidsystem/ev_keymap.h>

using deskflow::coordination::Message;
using deskflow::coordination::mapRelayMediaKeyFromCgEvent;
using deskflow::coordination::mediaKeyIdFromNxType;

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

QTEST_MAIN(KeyboardRelayMapTests)
