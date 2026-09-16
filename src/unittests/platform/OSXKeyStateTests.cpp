/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2011 Nick Bolton
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "OSXKeyStateTests.h"

#include "base/EventQueue.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hidsystem/IOLLEvent.h>
#include <mach/kern_return.h>

#define SHIFT_ID_L kKeyShift_L
#define SHIFT_ID_R kKeyShift_R
#define SHIFT_BUTTON 57
#define A_CHAR_ID 0x00000061
#define A_CHAR_BUTTON 001

void OSXKeyStateTests::initTestCase()
{
  m_arch.init();
  m_log.setFilter(LogLevel::Level::Verbose);
}

void OSXKeyStateTests::mapModifiersFromOSX_OSXMask()
{
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  OSXKeyState keyState(&eventQueue, keyMap, {"en"}, true);

  KeyModifierMask outMask = 0;

  uint32_t shiftMask = 0 | kCGEventFlagMaskShift;
  outMask = keyState.mapModifiersFromOSX(shiftMask);
  QCOMPARE(outMask, KeyModifierShift);

  uint32_t ctrlMask = 0 | kCGEventFlagMaskControl;
  outMask = keyState.mapModifiersFromOSX(ctrlMask);
  QCOMPARE(outMask, KeyModifierControl);

  uint32_t altMask = 0 | kCGEventFlagMaskAlternate;
  outMask = keyState.mapModifiersFromOSX(altMask);
  QCOMPARE(outMask, KeyModifierAlt);

  uint32_t cmdMask = 0 | kCGEventFlagMaskCommand;
  outMask = keyState.mapModifiersFromOSX(cmdMask);
  QCOMPARE(outMask, KeyModifierSuper);

  uint32_t capsMask = 0 | kCGEventFlagMaskAlphaShift;
  outMask = keyState.mapModifiersFromOSX(capsMask);
  QCOMPARE(outMask, KeyModifierCapsLock);

  uint32_t numMask = 0 | kCGEventFlagMaskNumericPad;
  outMask = keyState.mapModifiersFromOSX(numMask);
  QCOMPARE(outMask, KeyModifierNumLock);
}

void OSXKeyStateTests::fakePollShift()
{
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  OSXKeyState keyState(&eventQueue, keyMap, {"en"}, true);
  keyState.updateKeyMap();

  keyState.fakeKeyDown(SHIFT_ID_L, 0, 1, "en");
  QVERIFY(isKeyPressed(keyState, SHIFT_BUTTON));

  keyState.fakeKeyUp(1);
  QVERIFY(!isKeyPressed(keyState, SHIFT_BUTTON));

  keyState.fakeKeyDown(SHIFT_ID_R, 0, 2, "en");
  QVERIFY(isKeyPressed(keyState, SHIFT_BUTTON));

  keyState.fakeKeyUp(2);
  QVERIFY(!isKeyPressed(keyState, SHIFT_BUTTON));
}

void OSXKeyStateTests::fakePollChar()
{
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  OSXKeyState keyState(&eventQueue, keyMap, {"en"}, true);
  keyState.updateKeyMap();

  keyState.fakeKeyDown(A_CHAR_ID, 0, 1, "en");
  QVERIFY(isKeyPressed(keyState, A_CHAR_BUTTON));

  keyState.fakeKeyUp(1);
  QVERIFY(!isKeyPressed(keyState, A_CHAR_BUTTON));

  // HACK: delete the key in case it was typed into a text editor.
  // we should really set focus to an invisible window.
  keyState.fakeKeyDown(kKeyBackSpace, 0, 2, "en");
  keyState.fakeKeyUp(2);
}

void OSXKeyStateTests::fakePollCharWithModifier()
{
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  OSXKeyState keyState(&eventQueue, keyMap, {"en"}, true);
  keyState.updateKeyMap();

  keyState.fakeKeyDown(A_CHAR_ID, KeyModifierShift, 1, "en");
  QVERIFY(isKeyPressed(keyState, A_CHAR_BUTTON));

  keyState.fakeKeyUp(1);
  QVERIFY(!isKeyPressed(keyState, A_CHAR_BUTTON));

  // HACK: delete the key in case it was typed into a text editor.
  // we should really set focus to an invisible window.
  keyState.fakeKeyDown(kKeyBackSpace, 0, 2, "en");
  keyState.fakeKeyUp(2);
}

void OSXKeyStateTests::mapKeyFromEventOffMainThreadDoesNotCrash()
{
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  OSXKeyState keyState(&eventQueue, keyMap, {"en"}, true);
  keyState.updateKeyMap();

  CGEventRef event = CGEventCreateKeyboardEvent(nullptr, kVK_ANSI_A, true);
  QVERIFY(event != nullptr);

  std::atomic<bool> finished{false};
  std::atomic<KeyButton> button{0};
  std::thread worker([&]() {
    OSXKeyState::KeyIDs ids;
    KeyModifierMask mask = 0;
    button.store(keyState.mapKeyFromEvent(ids, &mask, event), std::memory_order_relaxed);
    finished.store(true, std::memory_order_relaxed);
  });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!finished.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < deadline) {
    // Pump the main CFRunLoop so dispatch_sync(main_queue) from the worker can run.
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
  }
  if (!finished.load(std::memory_order_relaxed)) {
    worker.detach();
    QFAIL("mapKeyFromEvent did not complete within 5 seconds");
  }
  worker.join();
  CFRelease(event);

  QVERIFY(finished.load(std::memory_order_relaxed));
  QVERIFY(button.load(std::memory_order_relaxed) != 0);
}

//
// Headless modifier-reconciliation tests. All OS touch points are hooked so
// these never read the real keyboard nor inject anything.
//

namespace {

struct PostedKey
{
  uint8_t virtualKey;
  bool down;
  CGEventFlags flags;
};

struct HookedState
{
  CGEventFlags osFlags = 0;
  IKeyState::KeyButtonSet physical;
  std::vector<PostedKey> posted;
  bool capsKnown = true;
  bool capsOn = false;
  bool capsSetSucceeds = true;
  int capsSetCalls = 0;

  OSXKeyState::Hooks hooks()
  {
    OSXKeyState::Hooks h;
    h.osModifierFlags = [this] { return osFlags; };
    h.pressedKeys = [this](IKeyState::KeyButtonSet &out) { out = physical; };
    h.postHIDKey = [this](uint8_t vk, bool down, CGEventFlags flags) {
      posted.push_back({vk, down, flags});
      return KERN_SUCCESS;
    };
    h.getCapsLockState = [this](bool &on) {
      on = capsOn;
      return capsKnown;
    };
    h.setCapsLockState = [this](bool on) {
      ++capsSetCalls;
      if (capsSetSucceeds) {
        capsOn = on;
      }
      return capsSetSucceeds;
    };
    return h;
  }
};

// KeyButton for a modifier virtual key (mirrors OSXKeyState's +1 offset)
constexpr KeyButton buttonFor(uint32_t virtualKey)
{
  return static_cast<KeyButton>(virtualKey + 1);
}

} // namespace

void OSXKeyStateTests::shadowFlagsReseedFromOsOnUpdateKeyState()
{
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  OSXKeyState keyState(&eventQueue, keyMap, {"en"}, true);
  HookedState os;
  keyState.setHooks(os.hooks());

  QCOMPARE(keyState.getModifierStateAsOSXFlags(), CGEventFlags(0));

  os.osFlags = kCGEventFlagMaskShift | kCGEventFlagMaskCommand;
  keyState.updateKeyState();
  QCOMPARE(keyState.getModifierStateAsOSXFlags(), CGEventFlags(kCGEventFlagMaskShift | kCGEventFlagMaskCommand));

  os.osFlags = kCGEventFlagMaskAlphaShift;
  keyState.updateKeyState();
  QCOMPARE(keyState.getModifierStateAsOSXFlags(), CGEventFlags(kCGEventFlagMaskAlphaShift));

  os.osFlags = 0;
  keyState.fakeAllKeysUp();
  QCOMPARE(keyState.getModifierStateAsOSXFlags(), CGEventFlags(0));
  QVERIFY(os.posted.empty());
}

void OSXKeyStateTests::keyboardEventFlagsKeepDeviceBitsWithCapsOn()
{
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  OSXKeyState keyState(&eventQueue, keyMap, {"en"}, true);
  HookedState os;
  keyState.setHooks(os.hooks());
  keyState.updateKeyMap();

  // caps lock is on in the OS; a faked shift press must still carry the
  // device-dependent left-shift bit, otherwise Shift+Caps combos post as
  // plain caps.
  os.osFlags = kCGEventFlagMaskAlphaShift;
  keyState.updateKeyState();
  keyState.fakeKeyDown(SHIFT_ID_L, 0, 1, "en");

  QVERIFY(!os.posted.empty());
  const PostedKey &shiftDown = os.posted.back();
  QCOMPARE(int(shiftDown.virtualKey), int(kVK_Shift));
  QVERIFY(shiftDown.down);
  QVERIFY((shiftDown.flags & kCGEventFlagMaskShift) != 0);
  QVERIFY((shiftDown.flags & kCGEventFlagMaskAlphaShift) != 0);
  QVERIFY((shiftDown.flags & NX_DEVICELSHIFTKEYMASK) != 0);

  keyState.fakeKeyUp(1);
}

void OSXKeyStateTests::sanitizeReleasesOnlyInjectedModifiers()
{
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  OSXKeyState keyState(&eventQueue, keyMap, {"en"}, true);
  HookedState os;
  keyState.setHooks(os.hooks());
  keyState.updateKeyMap();

  // we inject shift (captured by the hook, never reaches the OS) ...
  keyState.fakeKeyDown(SHIFT_ID_L, 0, 1, "en");
  QVERIFY(keyState.injectedModifiers().count(kVK_Shift) == 1);
  os.posted.clear();

  // ... then the OS reports shift AND control down, control physically held
  os.osFlags = kCGEventFlagMaskShift | kCGEventFlagMaskControl;
  os.physical = {buttonFor(kVK_Shift), buttonFor(kVK_Control)};

  keyState.sanitizeInjectedKeys();

  // exactly one release, for shift, with control still set in the flags
  QCOMPARE(os.posted.size(), size_t(1));
  QCOMPARE(int(os.posted[0].virtualKey), int(kVK_Shift));
  QVERIFY(!os.posted[0].down);
  QVERIFY((os.posted[0].flags & kCGEventFlagMaskShift) == 0);
  QVERIFY((os.posted[0].flags & kCGEventFlagMaskControl) != 0);

  QVERIFY(keyState.injectedModifiers().empty());
  QCOMPARE(keyState.getModifierStateAsOSXFlags(), CGEventFlags(kCGEventFlagMaskControl));

  // a second pass has nothing left to do
  keyState.sanitizeInjectedKeys();
  QCOMPARE(os.posted.size(), size_t(1));
}

void OSXKeyStateTests::sanitizeSkipsModifiersOsReportsUp()
{
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  OSXKeyState keyState(&eventQueue, keyMap, {"en"}, true);
  HookedState os;
  keyState.setHooks(os.hooks());
  keyState.updateKeyMap();

  keyState.fakeKeyDown(SHIFT_ID_L, 0, 1, "en");
  os.posted.clear();

  // OS lost the shift already (e.g. HID re-enumeration): don't post a
  // spurious up, just forget it.
  os.osFlags = 0;
  keyState.sanitizeInjectedKeys();

  QVERIFY(os.posted.empty());
  QVERIFY(keyState.injectedModifiers().empty());
  QCOMPARE(keyState.getModifierStateAsOSXFlags(), CGEventFlags(0));
}

void OSXKeyStateTests::setToggleStateNoOpsWhenCapsMatches()
{
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  OSXKeyState keyState(&eventQueue, keyMap, {"en"}, true);
  HookedState os;
  os.capsOn = true;
  os.osFlags = kCGEventFlagMaskAlphaShift;
  keyState.setHooks(os.hooks());

  keyState.setToggleState(KeyModifierCapsLock, true);

  QCOMPARE(os.capsSetCalls, 0);
  QVERIFY(os.posted.empty());
  QCOMPARE(keyState.getModifierStateAsOSXFlags(), CGEventFlags(kCGEventFlagMaskAlphaShift));

  os.capsOn = false;
  os.osFlags = 0;
  keyState.setToggleState(KeyModifierCapsLock, false);
  QCOMPARE(os.capsSetCalls, 0);
  QVERIFY(os.posted.empty());
  QCOMPARE(keyState.getModifierStateAsOSXFlags(), CGEventFlags(0));
}

void OSXKeyStateTests::setToggleStateDrivesCapsViaLockStateApi()
{
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  OSXKeyState keyState(&eventQueue, keyMap, {"en"}, true);
  HookedState os;
  os.capsOn = false;
  keyState.setHooks(os.hooks());

  keyState.setToggleState(KeyModifierCapsLock, true);

  QCOMPARE(os.capsSetCalls, 1);
  QVERIFY(os.capsOn);
  QVERIFY(os.posted.empty()); // lock-state API path never fakes a key
  QCOMPARE(keyState.getModifierStateAsOSXFlags(), CGEventFlags(kCGEventFlagMaskAlphaShift));

  keyState.setToggleState(KeyModifierCapsLock, false);
  QCOMPARE(os.capsSetCalls, 2);
  QVERIFY(!os.capsOn);
  QVERIFY(os.posted.empty());
  QCOMPARE(keyState.getModifierStateAsOSXFlags(), CGEventFlags(0));
}

void OSXKeyStateTests::setToggleStateFallsBackToFakeCapsPress()
{
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  OSXKeyState keyState(&eventQueue, keyMap, {"en"}, true);
  HookedState os;
  os.capsKnown = false; // lock-state API unavailable
  os.capsSetSucceeds = false;
  os.osFlags = 0; // CG flags say caps is off
  keyState.setHooks(os.hooks());

  keyState.setToggleState(KeyModifierCapsLock, true);

  // set attempted once, then a synthetic caps press+release
  QCOMPARE(os.capsSetCalls, 1);
  QCOMPARE(os.posted.size(), size_t(2));
  QCOMPARE(int(os.posted[0].virtualKey), int(kVK_CapsLock));
  QVERIFY(os.posted[0].down);
  QVERIFY((os.posted[0].flags & kCGEventFlagMaskAlphaShift) != 0);
  QCOMPARE(int(os.posted[1].virtualKey), int(kVK_CapsLock));
  QVERIFY(!os.posted[1].down);

  // when the flags already agree the fallback must not fire
  os.posted.clear();
  os.capsSetCalls = 0;
  os.osFlags = kCGEventFlagMaskAlphaShift;
  keyState.setToggleState(KeyModifierCapsLock, true);
  QCOMPARE(os.capsSetCalls, 0);
  QVERIFY(os.posted.empty());
}

void OSXKeyStateTests::setToggleStateIgnoresNumAndScrollLock()
{
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  OSXKeyState keyState(&eventQueue, keyMap, {"en"}, true);
  HookedState os;
  keyState.setHooks(os.hooks());

  keyState.setToggleState(KeyModifierNumLock, true);
  keyState.setToggleState(KeyModifierScrollLock, true);

  QCOMPARE(os.capsSetCalls, 0);
  QVERIFY(os.posted.empty());
}

bool OSXKeyStateTests::isKeyPressed(const OSXKeyState &keyState, KeyButton button)
{
  // HACK: allow os to realize key state changes.
  Arch::sleep(.2);

  IKeyState::KeyButtonSet pressed;
  keyState.pollPressedKeys(pressed);

  IKeyState::KeyButtonSet::const_iterator it;
  for (it = pressed.begin(); it != pressed.end(); ++it) {
    LOG_DEBUG("checking key %d", *it);
    if (*it == button) {
      return true;
    }
  }
  return false;
}

QTEST_MAIN(OSXKeyStateTests)
