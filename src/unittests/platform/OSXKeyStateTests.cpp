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
#include <set>
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
  double now = 100.0; // fake monotonic clock, seconds

  OSXKeyState::Hooks hooks()
  {
    OSXKeyState::Hooks h;
    h.monotonicNow = [this] { return now; };
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

//! Exposes the protected fakeKey() so a test can post a modifier virtual
//! key directly (ledgered but outside the synthetic key set), as a relay
//! or a crashed incarnation would have.
struct InjectingKeyState : OSXKeyState
{
  using OSXKeyState::fakeKey;
  using OSXKeyState::OSXKeyState;
};

deskflow::KeyMap::Keystroke stroke(uint32_t virtualKey, bool press)
{
  return deskflow::KeyMap::Keystroke(buttonFor(virtualKey), press, false, 0);
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

void OSXKeyStateTests::keyboardEventFlagsCarryShiftForUpperLetterWithCapsOn()
{
  // Cross-machine vector for the login-screen capitalization fix: the OS
  // reports Caps Lock ON (the user left it on at this seat) while a server
  // whose caps is off sends 'K' with Shift. The layout maps 'K' to the k
  // key + Shift, so the client injects a Shift press and the letter must
  // be posted with event flags carrying Shift on top of the OS caps state
  // (never stripped because caps is on) -- that is what getKeyboardEventFlags()
  // returns at the moment the letter goes out. All OS touch points are
  // hooked: nothing is injected.
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  OSXKeyState keyState(&eventQueue, keyMap, {"en"}, true);
  HookedState os;
  OSXKeyState::Hooks hooks = os.hooks();
  // Record, for the letter itself, the flags the real (unhooked) path would
  // stamp on the CGEvent: the non-modifier hook is handed 0 by design.
  hooks.postHIDKey = [&](uint8_t vk, bool down, CGEventFlags flags) {
    os.posted.push_back({vk, down, vk == kVK_ANSI_K ? keyState.getKeyboardEventFlags() : flags});
    return KERN_SUCCESS;
  };
  keyState.setHooks(hooks);
  keyState.updateKeyMap();

  os.osFlags = kCGEventFlagMaskAlphaShift;
  os.capsOn = true;
  keyState.updateKeyState();
  QCOMPARE(keyState.getKeyboardEventFlags() & kCGEventFlagMaskAlphaShift, CGEventFlags(kCGEventFlagMaskAlphaShift));
  QCOMPARE(keyState.getKeyboardEventFlags() & kCGEventFlagMaskShift, CGEventFlags(0));

  keyState.fakeKeyDown(static_cast<KeyID>('K'), KeyModifierShift, 1, "en");

  bool sawShiftDown = false;
  bool sawLetter = false;
  for (const PostedKey &p : os.posted) {
    if (p.virtualKey == kVK_Shift && p.down) {
      sawShiftDown = true;
      QVERIFY((p.flags & kCGEventFlagMaskShift) != 0);
      QVERIFY((p.flags & kCGEventFlagMaskAlphaShift) != 0);
      QVERIFY((p.flags & NX_DEVICELSHIFTKEYMASK) != 0);
    }
    if (p.virtualKey == kVK_ANSI_K && p.down) {
      sawLetter = true;
      QVERIFY(sawShiftDown); // Shift lands before the letter
      // The letter's flags: Shift held, OS caps still on, device bit set.
      QVERIFY((p.flags & kCGEventFlagMaskShift) != 0);
      QVERIFY((p.flags & kCGEventFlagMaskAlphaShift) != 0);
      QVERIFY((p.flags & NX_DEVICELSHIFTKEYMASK) != 0);
    }
  }
  QVERIFY(sawLetter);
  QVERIFY(sawShiftDown);
  QCOMPARE(os.capsSetCalls, 0); // no caps toggle to compose a capital

  keyState.fakeKeyUp(1);
  QCOMPARE(keyState.getKeyboardEventFlags() & kCGEventFlagMaskShift, CGEventFlags(0));
  QVERIFY(keyState.injectedModifiers().empty());
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
  // (the event tap saw its hardware flagsChanged a moment ago)
  os.osFlags = kCGEventFlagMaskShift | kCGEventFlagMaskControl;
  os.physical = {buttonFor(kVK_Shift), buttonFor(kVK_Control)};
  keyState.noteHardwareModifierFlags(kCGEventFlagMaskControl | NX_DEVICELCTLKEYMASK, os.now - 0.5);

  keyState.sanitizeInjectedKeys();

  // exactly one release, for shift, with control still set in the flags
  QCOMPARE(os.posted.size(), size_t(1));
  QCOMPARE(int(os.posted[0].virtualKey), int(kVK_Shift));
  QVERIFY(!os.posted[0].down);
  QVERIFY((os.posted[0].flags & kCGEventFlagMaskShift) == 0);
  QVERIFY((os.posted[0].flags & kCGEventFlagMaskControl) != 0);

  QVERIFY(keyState.injectedModifiers().empty());
  QCOMPARE(keyState.getModifierStateAsOSXFlags(), CGEventFlags(kCGEventFlagMaskControl));

  // a second pass (the OS honoured the release) has nothing left to do
  os.osFlags = kCGEventFlagMaskControl;
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

void OSXKeyStateTests::sanitizeReleasesStaleModifiersWithoutRecentHardwarePress()
{
  // K2 residual: the process restarted (injected set empty) while the OS
  // still holds a Command that the old incarnation posted. No hardware
  // flagsChanged has carried a Command device bit -- ever, or within the
  // freshness window -- so the sweep must release it.
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  OSXKeyState keyState(&eventQueue, keyMap, {"en"}, true);
  HookedState os;
  keyState.setHooks(os.hooks());
  keyState.updateKeyMap();
  QVERIFY(keyState.injectedModifiers().empty());

  os.osFlags = kCGEventFlagMaskCommand | kCGEventFlagMaskShift;
  // shift WAS pressed on hardware, but long ago
  keyState.noteHardwareModifierFlags(kCGEventFlagMaskShift | NX_DEVICELSHIFTKEYMASK, os.now - 10.0);

  keyState.sanitizeInjectedKeys();

  QCOMPARE(os.posted.size(), size_t(2));
  std::set<int> released;
  for (const auto &p : os.posted) {
    QVERIFY(!p.down);
    released.insert(p.virtualKey);
  }
  QVERIFY(released.contains(kVK_Command));
  QVERIFY(released.contains(kVK_Shift));
  QCOMPARE(keyState.getModifierStateAsOSXFlags(), CGEventFlags(0));
}

void OSXKeyStateTests::sanitizeKeepsModifiersBackedByRecentHardwarePress()
{
  // The user is holding Shift for real: the tap saw the device bit within
  // the freshness window (left OR right side), so the sweep leaves it.
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  OSXKeyState keyState(&eventQueue, keyMap, {"en"}, true);
  HookedState os;
  keyState.setHooks(os.hooks());
  keyState.updateKeyMap();

  os.osFlags = kCGEventFlagMaskShift | kCGEventFlagMaskAlternate;
  keyState.noteHardwareModifierFlags(kCGEventFlagMaskShift | NX_DEVICERSHIFTKEYMASK, os.now - 1.0);
  keyState.noteHardwareModifierFlags(kCGEventFlagMaskAlternate | NX_DEVICELALTKEYMASK, os.now - 1.9);
  keyState.sanitizeInjectedKeys();
  QVERIFY(os.posted.empty());

  // the generic flag bits alone are not evidence of hardware
  keyState.noteHardwareModifierFlags(kCGEventFlagMaskShift | kCGEventFlagMaskAlternate, os.now + 5.0);
  os.now += 5.0;
  keyState.sanitizeInjectedKeys();
  QCOMPARE(os.posted.size(), size_t(2));
}

void OSXKeyStateTests::sanitizeReleasesInjectedCaps()
{
  // K2 gap b2: a Caps Lock KEY we posted Down and never Up used to have no
  // release path (the sweep skipped it as "a lock"). The sweep must post
  // the Up; the lock state stays whatever the OS says.
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  InjectingKeyState keyState(&eventQueue, keyMap, {"en"}, true);
  HookedState os;
  keyState.setHooks(os.hooks());

  keyState.fakeKey(stroke(kVK_CapsLock, true));
  QVERIFY(keyState.injectedModifiers().contains(kVK_CapsLock));
  QCOMPARE(os.posted.size(), size_t(1));
  os.posted.clear();

  // our Caps down toggled the lock ON in the OS
  os.osFlags = kCGEventFlagMaskAlphaShift;
  keyState.sanitizeInjectedKeys();

  QCOMPARE(os.posted.size(), size_t(1));
  QCOMPARE(int(os.posted[0].virtualKey), int(kVK_CapsLock));
  QVERIFY(!os.posted[0].down);
  // the caps key-up carries the lock state, it does not clear it
  QVERIFY((os.posted[0].flags & kCGEventFlagMaskAlphaShift) != 0);
  QVERIFY(keyState.injectedModifiers().empty());
  QCOMPARE(keyState.getModifierStateAsOSXFlags(), CGEventFlags(kCGEventFlagMaskAlphaShift));

  // nothing left to do on a second pass
  keyState.sanitizeInjectedKeys();
  QCOMPARE(os.posted.size(), size_t(1));
}

void OSXKeyStateTests::sanitizeLeavesOsCapsLockAlone()
{
  // Caps lock ON in the OS with nothing injected is the user's lock state:
  // never a candidate for a synthetic key-up, whatever the freshness clock
  // says (there is no hardware press to back a lock).
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  InjectingKeyState keyState(&eventQueue, keyMap, {"en"}, true);
  HookedState os;
  keyState.setHooks(os.hooks());
  QVERIFY(keyState.injectedModifiers().empty());

  os.osFlags = kCGEventFlagMaskAlphaShift;
  keyState.sanitizeInjectedKeys();
  QVERIFY(os.posted.empty());
  QVERIFY(keyState.injectedModifiers().empty());
  QCOMPARE(keyState.getModifierStateAsOSXFlags(), CGEventFlags(kCGEventFlagMaskAlphaShift));

  // a caps we pressed AND released (a toggle tap) leaves nothing to sweep
  keyState.fakeKey(stroke(kVK_CapsLock, true));
  keyState.fakeKey(stroke(kVK_CapsLock, false));
  QVERIFY(keyState.injectedModifiers().empty());
  os.posted.clear();
  keyState.sanitizeInjectedKeys();
  QVERIFY(os.posted.empty());
  QCOMPARE(keyState.getModifierStateAsOSXFlags(), CGEventFlags(kCGEventFlagMaskAlphaShift));
}

void OSXKeyStateTests::releaseInjectedKeysLeavesPhysicallyHeldModifierAlone()
{
  // Review finding: a held key emits ONE flagsChanged, so a user shift-
  // dragging for longer than kHardwareModifierFreshS reads as "stale" to
  // the full sweep. The ledger-only release used on enterPrimary and by
  // the post-switch verifier must never reason about freshness at all.
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  InjectingKeyState keyState(&eventQueue, keyMap, {"en"}, true);
  HookedState os;
  keyState.setHooks(os.hooks());
  QVERIFY(keyState.injectedModifiers().empty());

  // Shift and Cmd held on this keyboard; the last flagsChanged is long past
  os.osFlags = kCGEventFlagMaskShift | kCGEventFlagMaskCommand;
  keyState.noteHardwareModifierFlags(kCGEventFlagMaskShift | NX_DEVICELSHIFTKEYMASK, os.now - 30.0);
  keyState.noteHardwareModifierFlags(kCGEventFlagMaskCommand | NX_DEVICELCMDKEYMASK, os.now - 30.0);

  keyState.releaseInjectedKeys();
  QVERIFY(os.posted.empty());
  QCOMPARE(keyState.getModifierStateAsOSXFlags(), CGEventFlags(kCGEventFlagMaskShift | kCGEventFlagMaskCommand));

  // ... whereas the full sweep WOULD release them (documented, boundary-only)
  keyState.sanitizeInjectedKeys();
  QCOMPARE(os.posted.size(), size_t(2));
}

void OSXKeyStateTests::releaseInjectedKeysReleasesLedgeredCmd()
{
  // The stuck case the verifier exists for: WE posted Cmd down (ledger),
  // the Up never came, nothing was typed. The ledger release closes it and
  // nothing else, with the user's physically held Shift left alone.
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  InjectingKeyState keyState(&eventQueue, keyMap, {"en"}, true);
  HookedState os;
  keyState.setHooks(os.hooks());

  keyState.fakeKey(stroke(kVK_Command, true));
  QVERIFY(keyState.injectedModifiers().contains(kVK_Command));
  os.posted.clear();

  os.osFlags = kCGEventFlagMaskCommand | kCGEventFlagMaskShift;
  keyState.releaseInjectedKeys();

  QCOMPARE(os.posted.size(), size_t(1));
  QCOMPARE(int(os.posted[0].virtualKey), int(kVK_Command));
  QVERIFY(!os.posted[0].down);
  QVERIFY((os.posted[0].flags & kCGEventFlagMaskCommand) == 0);
  QVERIFY((os.posted[0].flags & kCGEventFlagMaskShift) != 0);
  QVERIFY(keyState.injectedModifiers().empty());
  QCOMPARE(keyState.getModifierStateAsOSXFlags(), CGEventFlags(kCGEventFlagMaskShift));
}

void OSXKeyStateTests::fakeAllKeysUpReleasesLedgeredModifierOutsideSyntheticSet()
{
  // K2 gap b1: leave() runs fakeAllKeysUp(), which used to clear() the
  // injected ledger without releasing a modifier posted outside the
  // synthetic key set. It must post the Up (and still leave the user's
  // own Shift alone, freshness or not).
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  InjectingKeyState keyState(&eventQueue, keyMap, {"en"}, true);
  HookedState os;
  keyState.setHooks(os.hooks());

  keyState.fakeKey(stroke(kVK_Option, true)); // ledgered, not synthetic
  os.posted.clear();
  os.osFlags = kCGEventFlagMaskAlternate | kCGEventFlagMaskShift;

  keyState.fakeAllKeysUp();

  QCOMPARE(os.posted.size(), size_t(1));
  QCOMPARE(int(os.posted[0].virtualKey), int(kVK_Option));
  QVERIFY(!os.posted[0].down);
  QVERIFY(keyState.injectedModifiers().empty());
  QCOMPARE(keyState.getModifierStateAsOSXFlags(), CGEventFlags(kCGEventFlagMaskAlternate | kCGEventFlagMaskShift));
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

void OSXKeyStateTests::setToggleStateKeepsTrackedMaskInStep()
{
  // The Caps double-toggle: after setToggleState() the OS has caps on but
  // the tracked mask (what mapKey() consults) said off, so the next key
  // with Caps in its mask clicked Caps again. Every exit path must sync it.
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  OSXKeyState keyState(&eventQueue, keyMap, {"en"}, true);
  HookedState os;
  os.capsOn = false;
  keyState.setHooks(os.hooks());
  QCOMPARE(keyState.getActiveModifiers() & KeyModifierCapsLock, KeyModifierMask(0));

  // lock-state API path
  keyState.setToggleState(KeyModifierCapsLock, true);
  QVERIFY((keyState.getActiveModifiers() & KeyModifierCapsLock) != 0);
  keyState.setToggleState(KeyModifierCapsLock, false);
  QCOMPARE(keyState.getActiveModifiers() & KeyModifierCapsLock, KeyModifierMask(0));

  // already-in-phase path still syncs (the mask may have been stale)
  os.capsOn = true;
  os.osFlags = kCGEventFlagMaskAlphaShift;
  keyState.setToggleState(KeyModifierCapsLock, true);
  QVERIFY((keyState.getActiveModifiers() & KeyModifierCapsLock) != 0);

  // failed set: the mask follows what the OS actually reports
  os.capsSetSucceeds = false;
  keyState.setToggleState(KeyModifierCapsLock, false);
  QVERIFY((keyState.getActiveModifiers() & KeyModifierCapsLock) != 0);
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
