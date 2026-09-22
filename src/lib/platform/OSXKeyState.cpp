/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/OSXKeyState.h"
#include "arch/Arch.h"
#include "base/Log.h"
#include "platform/OSXInjectedEvent.h"
#include "platform/OSXMainQueue.h"
#include "platform/OSXMediaKeySupport.h"
#include "platform/OSXUchrKeyResource.h"

#include <Carbon/Carbon.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDParameter.h>
#include <chrono>
#include <pthread.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

// Note that some virtual keys codes appear more than once.  The
// first instance of a virtual key code maps to the KeyID that we
// want to generate for that code.  The others are for mapping
// different KeyIDs to a single key code.
static const uint32_t s_shiftVK = kVK_Shift;
static const uint32_t s_controlVK = kVK_Control;
static const uint32_t s_altVK = kVK_Option;
static const uint32_t s_superVK = kVK_Command;
static const uint32_t s_capsLockVK = kVK_CapsLock;
// Fn/Globe (kVK_Function) is deliberately NOT tracked: no KeyID in the key
// map resolves to it, so this process can never post it and there is
// nothing to release. Revisit only if a Globe KeyID is ever added.
static const uint32_t s_numLockVK = kVK_ANSI_KeypadClear; // 71

static const uint32_t s_brightnessUp = 144;
static const uint32_t s_brightnessDown = 145;
static const uint32_t s_missionControlVK = 160;
static const uint32_t s_launchpadVK = 131;

static const uint32_t s_osxNumLock = 1 << 16;

struct KeyEntry
{
public:
  KeyID m_keyID;
  uint32_t m_virtualKey;
};
static const KeyEntry s_controlKeys[] = {
    // cursor keys.  if we don't do this we'll may still get these from
    // the keyboard resource but they may not correspond to the arrow
    // keys.
    {kKeyLeft, kVK_LeftArrow},
    {kKeyRight, kVK_RightArrow},
    {kKeyUp, kVK_UpArrow},
    {kKeyDown, kVK_DownArrow},
    {kKeyHome, kVK_Home},
    {kKeyEnd, kVK_End},
    {kKeyPageUp, kVK_PageUp},
    {kKeyPageDown, kVK_PageDown},
    {kKeyInsert, kVK_Help}, // Mac Keyboards have 'Help' on 'Insert'

    // function keys
    {kKeyF1, kVK_F1},
    {kKeyF2, kVK_F2},
    {kKeyF3, kVK_F3},
    {kKeyF4, kVK_F4},
    {kKeyF5, kVK_F5},
    {kKeyF6, kVK_F6},
    {kKeyF7, kVK_F7},
    {kKeyF8, kVK_F8},
    {kKeyF9, kVK_F9},
    {kKeyF10, kVK_F10},
    {kKeyF11, kVK_F11},
    {kKeyF12, kVK_F12},
    {kKeyF13, kVK_F13},
    {kKeyF14, kVK_F14},
    {kKeyF15, kVK_F15},
    {kKeyF16, kVK_F16},

    {kKeyKP_0, kVK_ANSI_Keypad0},
    {kKeyKP_1, kVK_ANSI_Keypad1},
    {kKeyKP_2, kVK_ANSI_Keypad2},
    {kKeyKP_3, kVK_ANSI_Keypad3},
    {kKeyKP_4, kVK_ANSI_Keypad4},
    {kKeyKP_5, kVK_ANSI_Keypad5},
    {kKeyKP_6, kVK_ANSI_Keypad6},
    {kKeyKP_7, kVK_ANSI_Keypad7},
    {kKeyKP_8, kVK_ANSI_Keypad8},
    {kKeyKP_9, kVK_ANSI_Keypad9},
    {kKeyKP_Decimal, kVK_ANSI_KeypadDecimal},
    {kKeyKP_Equal, kVK_ANSI_KeypadEquals},
    {kKeyKP_Multiply, kVK_ANSI_KeypadMultiply},
    {kKeyKP_Add, kVK_ANSI_KeypadPlus},
    {kKeyKP_Divide, kVK_ANSI_KeypadDivide},
    {kKeyKP_Subtract, kVK_ANSI_KeypadMinus},
    {kKeyKP_Enter, kVK_ANSI_KeypadEnter},

    // virtual key 110 is fn+enter and i have no idea what that's supposed
    // to map to.  also the enter key with numlock on is a modifier but i
    // don't know which.

    // modifier keys.  OS X doesn't seem to support right handed versions
    // of modifier keys so we map them to the left handed versions.
    {kKeyShift_L, s_shiftVK},
    {kKeyShift_R, s_shiftVK}, // 60
    {kKeyControl_L, s_controlVK},
    {kKeyControl_R, s_controlVK}, // 62
    {kKeyAlt_L, s_altVK},
    {kKeyAlt_R, s_altVK},
    {kKeySuper_L, s_superVK},
    {kKeySuper_R, s_superVK}, // 61
    {kKeyMeta_L, s_superVK},
    {kKeyMeta_R, s_superVK}, // 61

    // toggle modifiers
    {kKeyNumLock, s_numLockVK},
    {kKeyCapsLock, s_capsLockVK},

    // for Apple Pro JIS Keyboard, map Kana (IME activate) to Henkan (show next
    // IME conversion), and
    // Eisu (IME deactivate) to Zenkaku (IME activation toggle) on Windows
    // Japanese keyboard (OADG109A)
    {kKeyHenkan, kVK_JIS_Kana},
    {kKeyZenkaku, kVK_JIS_Eisu},

    {kKeyMissionControl, s_missionControlVK},
    {kKeyLaunchpad, s_launchpadVK},
    {kKeyBrightnessUp, s_brightnessUp},
    {kKeyBrightnessDown, s_brightnessDown}
};

namespace {

io_connect_t getService(io_iterator_t iter)
{
  io_connect_t service = 0;
  auto nextIterator = IOIteratorNext(iter);

  if (nextIterator) {
    IOServiceOpen(nextIterator, mach_task_self(), kIOHIDParamConnectType, &service);
    IOObjectRelease(nextIterator);
  }

  return service;
}

io_connect_t getEventDriver()
{
  static io_connect_t sEventDrvrRef = 0;

  if (!sEventDrvrRef) {
    // Get master device port
    mach_port_t masterPort = 0;
    if (!IOMasterPort(bootstrap_port, &masterPort)) {
      io_iterator_t iter = 0;
      auto dict = IOServiceMatching(kIOHIDSystemClass);

      if (!IOServiceGetMatchingServices(masterPort, dict, &iter)) {
        sEventDrvrRef = getService(iter);
      } else {
        LOG_WARN("io service not found");
      }

      IOObjectRelease(iter);
    } else {
      LOG_WARN("couldn't get io master port");
    }
  }

  return sEventDrvrRef;
}

bool isModifier(uint8_t virtualKey)
{
  static std::set<uint8_t> modifiers{s_shiftVK, s_superVK, s_altVK, s_controlVK, s_capsLockVK};

  return (modifiers.find(virtualKey) != modifiers.end());
}

} // namespace

//
// OSXKeyState
//

OSXKeyState::OSXKeyState(IEventQueue *events, std::vector<std::string> layouts, bool isLangSyncEnabled)
    : KeyState(events, std::move(layouts), isLangSyncEnabled)
{
  init();
}

OSXKeyState::OSXKeyState(
    IEventQueue *events, deskflow::KeyMap &keyMap, std::vector<std::string> layouts, bool isLangSyncEnabled
)
    : KeyState(events, keyMap, std::move(layouts), isLangSyncEnabled)
{
  init();
}

void OSXKeyState::init()
{
  m_deadKeyState = 0;
  m_shiftPressed = false;
  m_controlPressed = false;
  m_altPressed = false;
  m_superPressed = false;
  m_capsPressed = false;

  // build virtual key map
  for (size_t i = 0; i < sizeof(s_controlKeys) / sizeof(s_controlKeys[0]); ++i) {

    m_virtualKeyMap[s_controlKeys[i].m_virtualKey] = s_controlKeys[i].m_keyID;
  }

  // Seed hardware-modifier freshness from OS truth at startup. Without this,
  // m_lastHardwareModifierAt starts at time_point::lowest() ("never seen")
  // for every modifier, on every seat -- not just a secondary screen whose
  // tap never fires. The very first sanitizeInjectedKeys() call (screen
  // enable(), lock/unlock/wake, KeyClearAll) can land before any hardware
  // flagsChanged has been observed; with no seed, "no data yet" reads as
  // "confirmed stale" and releases a modifier the user is already physically
  // holding from before this process started. Reads the raw APIs directly
  // (not the hookable osModifierFlags()/now(), which setHooks() only wires
  // up after construction returns) so this is a one-time real hardware read
  // at process start, and tests that install hooks afterward are unaffected.
  const CGEventFlags startupFlags = CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState);
  const double startupNow = monotonicSeconds();
  for (uint32_t virtualKey : {s_shiftVK, s_controlVK, s_altVK, s_superVK}) {
    const auto vk = static_cast<uint8_t>(virtualKey);
    const int slot = hardwareSlotForVirtualKey(vk);
    const CGEventFlags flag = modifierFlagForVirtualKey(vk);
    if (slot >= 0 && (startupFlags & flag) != 0) {
      m_lastHardwareModifierAt[slot].store(startupNow, std::memory_order_relaxed);
    }
  }
}

KeyModifierMask OSXKeyState::mapModifiersFromOSX(uint32_t mask) const
{
  KeyModifierMask outMask = 0;
  if ((mask & kCGEventFlagMaskShift) != 0) {
    outMask |= KeyModifierShift;
  }
  if ((mask & kCGEventFlagMaskControl) != 0) {
    outMask |= KeyModifierControl;
  }
  if ((mask & kCGEventFlagMaskAlternate) != 0) {
    outMask |= KeyModifierAlt;
  }
  if ((mask & kCGEventFlagMaskCommand) != 0) {
    outMask |= KeyModifierSuper;
  }
  if ((mask & kCGEventFlagMaskAlphaShift) != 0) {
    outMask |= KeyModifierCapsLock;
  }
  if ((mask & kCGEventFlagMaskNumericPad) != 0) {
    outMask |= KeyModifierNumLock;
  }

  LOG_VERBOSE("mask=%04x outMask=%04x", mask, outMask);
  return outMask;
}

KeyModifierMask OSXKeyState::mapModifiersToCarbon(uint32_t mask) const
{
  KeyModifierMask outMask = 0;
  if ((mask & kCGEventFlagMaskShift) != 0) {
    outMask |= shiftKey;
  }
  if ((mask & kCGEventFlagMaskControl) != 0) {
    outMask |= controlKey;
  }
  if ((mask & kCGEventFlagMaskCommand) != 0) {
    outMask |= cmdKey;
  }
  if ((mask & kCGEventFlagMaskAlternate) != 0) {
    outMask |= optionKey;
  }
  if ((mask & kCGEventFlagMaskAlphaShift) != 0) {
    outMask |= alphaLock;
  }
  if ((mask & kCGEventFlagMaskNumericPad) != 0) {
    outMask |= s_osxNumLock;
  }

  return outMask;
}

KeyButton OSXKeyState::mapKeyFromEvent(KeyIDs &ids, KeyModifierMask *maskOut, CGEventRef event) const
{
  ids.clear();

  // map modifier key
  if (maskOut != nullptr) {
    KeyModifierMask activeMask = getActiveModifiers();
    activeMask &= ~KeyModifierAltGr;
    *maskOut = activeMask;
  }

  // get virtual key
  uint32_t vkCode = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);

  // handle up events
  uint32_t eventKind = CGEventGetType(event);
  if (eventKind == kCGEventKeyUp) {
    // the id isn't used.  we just need the same button we used on
    // the key press.  note that we don't use or reset the dead key
    // state;  up events should not affect the dead key state.
    ids.push_back(kKeyNone);
    return mapVirtualKeyToKeyButton(vkCode);
  }

  // check for special keys
  VirtualKeyMap::const_iterator i = m_virtualKeyMap.find(vkCode);
  if (i != m_virtualKeyMap.end()) {
    m_deadKeyState = 0;
    ids.push_back(i->second);
    return mapVirtualKeyToKeyButton(vkCode);
  }

  // TIS APIs assert the main dispatch queue on macOS 14+; mapKeyFromEvent runs
  // on the CGEventTap thread, so fetch layout data via runOnMainQueue (same
  // pattern as getKeyMap/getGroups; see pollActiveGroup for async cache variant).
  CFDataRef layoutRef = deskflow::platform::osx::runOnMainQueue([]() -> CFDataRef {
    std::lock_guard<std::mutex> lock(g_tisMutex);
    AutoTISInputSourceRef currentKeyboardLayout(TISCopyCurrentKeyboardLayoutInputSource(), CFRelease);
    if (!currentKeyboardLayout) {
      return nullptr;
    }
    CFDataRef ref = (CFDataRef)TISGetInputSourceProperty(
        currentKeyboardLayout.get(), kTISPropertyUnicodeKeyLayoutData
    );
    if (ref) {
      CFRetain(ref);
    }
    return ref;
  });
  AutoCFData layoutData(layoutRef, CFRelease);

  if (!layoutData) {
    return kKeyNone;
  }

  // get the event modifiers and remove the command and control
  // keys.  note if we used them though.
  // UCKeyTranslate expects old-style Carbon modifiers, so convert.
  uint32_t modifiers;
  modifiers = mapModifiersToCarbon(CGEventGetFlags(event));
  static const uint32_t s_commandModifiers = cmdKey | controlKey | rightControlKey;
  bool isCommand = ((modifiers & s_commandModifiers) != 0);
  modifiers &= ~s_commandModifiers;

  // if we've used a command key then we want the glyph produced without
  // the option key (i.e. the base glyph).
  // if (isCommand) {
  modifiers &= ~optionKey;
  //}

  // choose action
  uint16_t action;
  if (eventKind == kCGEventKeyDown) {
    action = kUCKeyActionDown;
  } else if (CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat) == 1) {
    action = kUCKeyActionAutoKey;
  } else {
    return 0;
  }

  // translate via uchr resource
  const UCKeyboardLayout *layout = (const UCKeyboardLayout *)CFDataGetBytePtr(layoutData.get());
  const bool layoutValid = (layout != nullptr);

  if (layoutValid) {
    // translate key
    UniCharCount count;
    UniChar chars[2];
    LOG_VERBOSE("modifiers: %08x", modifiers & 0xffu);
    OSStatus status = UCKeyTranslate(
        layout, vkCode & 0xffu, action, (modifiers >> 8) & 0xffu, LMGetKbdType(), 0, &m_deadKeyState,
        sizeof(chars) / sizeof(chars[0]), &count, chars
    );

    // get the characters
    if (status == 0) {
      if (count != 0 || m_deadKeyState == 0) {
        m_deadKeyState = 0;
        for (UniCharCount i = 0; i < count; ++i) {
          ids.push_back(IOSXKeyResource::unicharToKeyID(chars[i]));
        }
        adjustAltGrModifier(ids, maskOut, isCommand);
        return mapVirtualKeyToKeyButton(vkCode);
      }
      return 0;
    }
  }

  return 0;
}

bool OSXKeyState::fakeCtrlAltDel()
{
  // pass keys through unchanged
  return false;
}

bool OSXKeyState::fakeMediaKey(KeyID id)
{
  return fakeNativeMediaKey(id);
}

CGEventFlags OSXKeyState::getModifierStateAsOSXFlags() const
{
  CGEventFlags modifiers = 0;

  if (m_shiftPressed) {
    modifiers |= kCGEventFlagMaskShift;
  }

  if (m_controlPressed) {
    modifiers |= kCGEventFlagMaskControl;
  }

  if (m_altPressed) {
    modifiers |= kCGEventFlagMaskAlternate;
  }

  if (m_superPressed) {
    modifiers |= kCGEventFlagMaskCommand;
  }

  if (m_capsPressed) {
    modifiers |= kCGEventFlagMaskAlphaShift;
  }

  return modifiers;
}

KeyModifierMask OSXKeyState::pollActiveModifiers() const
{
  // falsely assumed that the mask returned by GetCurrentKeyModifiers()
  // was the same as a CGEventFlags (which is what mapModifiersFromOSX
  // expects). patch by Marc
  uint32_t mask = GetCurrentKeyModifiers();
  KeyModifierMask outMask = 0;

  if ((mask & shiftKey) != 0) {
    outMask |= KeyModifierShift;
  }
  if ((mask & controlKey) != 0) {
    outMask |= KeyModifierControl;
  }
  if ((mask & optionKey) != 0) {
    outMask |= KeyModifierAlt;
  }
  if ((mask & cmdKey) != 0) {
    outMask |= KeyModifierSuper;
  }
  if ((mask & alphaLock) != 0) {
    outMask |= KeyModifierCapsLock;
  }
  if ((mask & s_osxNumLock) != 0) {
    outMask |= KeyModifierNumLock;
  }

  LOG_VERBOSE("mask=%04x outMask=%04x", mask, outMask);
  return outMask;
}

int32_t OSXKeyState::updateActiveGroupCache()
{
  // MUST run on the main dispatch queue: macOS 14+ asserts the main queue
  // inside the Text Input Source APIs.
  AutoTISInputSourceRef keyboardLayout(nullptr, CFRelease);
  CFDataRef id = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_tisMutex);
    keyboardLayout = AutoTISInputSourceRef(TISCopyCurrentKeyboardLayoutInputSource(), CFRelease);
    if (keyboardLayout)
      id = (CFDataRef)TISGetInputSourceProperty(keyboardLayout.get(), kTISPropertyInputSourceID);
  }

  int32_t group = 0;
  if (GroupMap::const_iterator i = m_groupMap.find(id); i != m_groupMap.end()) {
    group = i->second;
  }
  m_activeGroupCache.store(group, std::memory_order_relaxed);
  return group;
}

int32_t OSXKeyState::pollActiveGroup() const
{
  // The TIS calls in updateActiveGroupCache() assert the main dispatch queue
  // (macOS 14+); deskflow injects keys (fakeKeyDown -> pollActiveGroup) on its
  // event thread, where calling them crashes via _dispatch_assert_queue_fail.
  // On the main thread refresh synchronously; off it, kick a non-blocking
  // refresh and return the last cached group (no blocking -> no deadlock).
  if (pthread_main_np() != 0) {
    return const_cast<OSXKeyState *>(this)->updateActiveGroupCache();
  }
  auto *self = const_cast<OSXKeyState *>(this);
  dispatch_async(dispatch_get_main_queue(), ^{ self->updateActiveGroupCache(); });
  return m_activeGroupCache.load(std::memory_order_relaxed);
}

void OSXKeyState::pollPressedKeys(KeyButtonSet &pressedKeys) const
{
  if (m_hooks.pressedKeys) {
    m_hooks.pressedKeys(pressedKeys);
    return;
  }

  ::KeyMap km;
  GetKeys(km);
  const uint8_t *m = reinterpret_cast<const uint8_t *>(km);
  for (uint32_t i = 0; i < 16; ++i) {
    for (uint32_t j = 0; j < 8; ++j) {
      if ((m[i] & (1u << j)) != 0) {
        pressedKeys.insert(mapVirtualKeyToKeyButton(8 * i + j));
      }
    }
  }
}

void OSXKeyState::getKeyMap(deskflow::KeyMap &keyMap)
{
  deskflow::platform::osx::runOnMainQueue([&] { getKeyMapImpl(keyMap); });
}

void OSXKeyState::getKeyMapImpl(deskflow::KeyMap &keyMap)
{
  // update keyboard groups
  int32_t numGroups{0};
  if (getGroups(m_groups)) {
    m_groupMap.clear();
    numGroups = CFArrayGetCount(m_groups.get());
    for (int32_t g = 0; g < numGroups; ++g) {
      TISInputSourceRef keyboardLayout = (TISInputSourceRef)CFArrayGetValueAtIndex(m_groups.get(), g);
      CFDataRef id = nullptr;
      {
        std::lock_guard<std::mutex> lock(g_tisMutex);
        id = (CFDataRef)TISGetInputSourceProperty(keyboardLayout, kTISPropertyInputSourceID);
      }
      m_groupMap[id] = g;
    }
  }

  uint32_t keyboardType = LMGetKbdType();
  for (int32_t g = 0; g < numGroups; ++g) {
    // add special keys
    getKeyMapForSpecialKeys(keyMap, g);

    const void *resource;
    bool layoutValid = false;

    // add regular keys
    // try uchr resource first
    TISInputSourceRef keyboardLayout = (TISInputSourceRef)CFArrayGetValueAtIndex(m_groups.get(), g);
    CFDataRef resourceRef = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_tisMutex);
      resourceRef = (CFDataRef)TISGetInputSourceProperty(keyboardLayout, kTISPropertyUnicodeKeyLayoutData);
    }

    layoutValid = resourceRef != nullptr;
    if (layoutValid)
      resource = CFDataGetBytePtr(resourceRef);

    if (layoutValid) {
      OSXUchrKeyResource uchr(resource, keyboardType);
      if (uchr.isValid()) {
        LOG_VERBOSE("using uchr resource for group %d", g);
        getKeyMap(keyMap, g, uchr);
        continue;
      }
    }

    LOG_VERBOSE("no keyboard resource for group %d", g);
  }
}

CGEventFlags OSXKeyState::getDeviceDependedFlags() const
{
  CGEventFlags modifiers = 0;

  if (m_shiftPressed) {
    modifiers |= NX_DEVICELSHIFTKEYMASK;
  }

  if (m_controlPressed) {
    modifiers |= NX_DEVICELCTLKEYMASK;
  }

  if (m_altPressed) {
    modifiers |= NX_DEVICELALTKEYMASK;
  }

  if (m_superPressed) {
    modifiers |= NX_DEVICELCMDKEYMASK;
  }

  return modifiers;
}

CGEventFlags OSXKeyState::getKeyboardEventFlags() const
{
  // set the event flags for special keys
  // http://tinyurl.com/pxl742y
  // The device-dependent (NX_DEVICEL*KEYMASK) bits used to be omitted
  // whenever caps lock was on, which made Shift+Caps combos post as plain
  // caps: apps that key off the device bits saw no shift at all. The device
  // bits only describe shift/ctrl/alt/cmd and are independent of the caps
  // lock state, so always carry them.
  return getModifierStateAsOSXFlags() | getDeviceDependedFlags();
}

CGEventFlags OSXKeyState::modifierFlagForVirtualKey(uint8_t virtualKey)
{
  switch (virtualKey) {
  case s_shiftVK:
    return kCGEventFlagMaskShift;
  case s_controlVK:
    return kCGEventFlagMaskControl;
  case s_altVK:
    return kCGEventFlagMaskAlternate;
  case s_superVK:
    return kCGEventFlagMaskCommand;
  case s_capsLockVK:
    return kCGEventFlagMaskAlphaShift;
  default:
    return 0;
  }
}

void OSXKeyState::setHooks(Hooks hooks)
{
  m_hooks = std::move(hooks);
}

std::set<uint8_t> OSXKeyState::injectedModifiers() const
{
  return m_injectedModifiers;
}

CGEventFlags OSXKeyState::osModifierFlags() const
{
  if (m_hooks.osModifierFlags) {
    return m_hooks.osModifierFlags();
  }
  return CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState);
}

void OSXKeyState::reseedShadowFlagsFromOS()
{
  const CGEventFlags os = osModifierFlags();
  m_shiftPressed = (os & kCGEventFlagMaskShift) != 0;
  m_controlPressed = (os & kCGEventFlagMaskControl) != 0;
  m_altPressed = (os & kCGEventFlagMaskAlternate) != 0;
  m_superPressed = (os & kCGEventFlagMaskCommand) != 0;
  m_capsPressed = (os & kCGEventFlagMaskAlphaShift) != 0;
  LOG_DEBUG("reseeded shadow modifier flags from os: 0x%llx", static_cast<unsigned long long>(os));
}

void OSXKeyState::setShadowFlags(CGEventFlags flags)
{
  m_shiftPressed = (flags & kCGEventFlagMaskShift) != 0;
  m_controlPressed = (flags & kCGEventFlagMaskControl) != 0;
  m_altPressed = (flags & kCGEventFlagMaskAlternate) != 0;
  m_superPressed = (flags & kCGEventFlagMaskCommand) != 0;
  m_capsPressed = (flags & kCGEventFlagMaskAlphaShift) != 0;
}

CGEventFlags OSXKeyState::leftDeviceBitForVirtualKey(uint8_t virtualKey)
{
  switch (virtualKey) {
  case s_shiftVK:
    return NX_DEVICELSHIFTKEYMASK;
  case s_controlVK:
    return NX_DEVICELCTLKEYMASK;
  case s_altVK:
    return NX_DEVICELALTKEYMASK;
  case s_superVK:
    return NX_DEVICELCMDKEYMASK;
  default:
    return 0;
  }
}

CGEventFlags OSXKeyState::rightDeviceBitForVirtualKey(uint8_t virtualKey)
{
  switch (virtualKey) {
  case s_shiftVK:
    return NX_DEVICERSHIFTKEYMASK;
  case s_controlVK:
    return NX_DEVICERCTLKEYMASK;
  case s_altVK:
    return NX_DEVICERALTKEYMASK;
  case s_superVK:
    return NX_DEVICERCMDKEYMASK;
  default:
    return 0;
  }
}

CGEventFlags OSXKeyState::modifierEventFlags(uint8_t virtualKey, bool down) const
{
  const CGEventFlags generic = modifierFlagForVirtualKey(virtualKey);
  CGEventFlags flags = osModifierFlags();
  if (generic == kCGEventFlagMaskAlphaShift) {
    // Caps is a lock: the Down asserts the flag (the OS toggles the lock on
    // the press); the Up carries whatever lock state the OS now reports.
    return down ? (flags | generic) : flags;
  }
  const CGEventFlags left = leftDeviceBitForVirtualKey(virtualKey);
  const CGEventFlags right = rightDeviceBitForVirtualKey(virtualKey);
  if (down) {
    return flags | generic | left;
  }
  flags &= ~left;
  if ((flags & right) == 0) {
    flags &= ~generic;
  }
  return flags;
}

bool OSXKeyState::secureInputEnabled() const
{
  if (m_hooks.secureInputEnabled) {
    return m_hooks.secureInputEnabled();
  }
  return IsSecureEventInputEnabled();
}

void OSXKeyState::noteHardwareObservation(bool observing, double now)
{
  if (observing && !m_hardwareObservable.load(std::memory_order_relaxed)) {
    m_hardwareObservableSince.store(now, std::memory_order_relaxed);
  }
  m_hardwareObservable.store(observing, std::memory_order_relaxed);
}

bool OSXKeyState::hardwareObservableFor(double seconds, double at) const
{
  if (!m_hardwareObservable.load(std::memory_order_relaxed)) {
    return false;
  }
  return (at - m_hardwareObservableSince.load(std::memory_order_relaxed)) >= seconds;
}

bool OSXKeyState::getCapsLockState(bool &on) const
{
  if (m_hooks.getCapsLockState) {
    return m_hooks.getCapsLockState(on);
  }
  auto driver = getEventDriver();
  if (!driver) {
    return false;
  }
  return IOHIDGetModifierLockState(driver, kIOHIDCapsLockState, &on) == KERN_SUCCESS;
}

bool OSXKeyState::setCapsLockState(bool on)
{
  if (m_hooks.setCapsLockState) {
    return m_hooks.setCapsLockState(on);
  }
  auto driver = getEventDriver();
  if (!driver) {
    return false;
  }
  return IOHIDSetModifierLockState(driver, kIOHIDCapsLockState, on) == KERN_SUCCESS;
}

void OSXKeyState::setToggleState(KeyModifierMask bit, bool on)
{
  // Num lock and scroll lock have no OS-level lock state on macOS.
  if (bit != KeyModifierCapsLock) {
    return;
  }

  bool current = false;
  bool known = getCapsLockState(current);
  if (!known) {
    current = (osModifierFlags() & kCGEventFlagMaskAlphaShift) != 0;
    known = true;
  }
  if (current == on) {
    m_capsPressed = on;
    syncTrackedModifier(bit, on);
    LOG_DEBUG("caps lock already %s", on ? "on" : "off");
    return;
  }

  if (setCapsLockState(on)) {
    LOG_DEBUG("set caps lock %s via IOHIDSystem", on ? "on" : "off");
  } else {
    // user-session path unavailable: fall back to a synthetic caps press,
    // but only because we verified above that the OS disagrees with `on`.
    LOG_DEBUG("IOHIDSetModifierLockState unavailable, faking caps lock press");
    setKeyboardModifiers(s_capsLockVK, true);
    if (postHIDVirtualKey(s_capsLockVK, true) != KERN_SUCCESS) {
      postKeyboardKey(s_capsLockVK, true);
    }
    setKeyboardModifiers(s_capsLockVK, false);
    if (postHIDVirtualKey(s_capsLockVK, false) != KERN_SUCCESS) {
      postKeyboardKey(s_capsLockVK, false);
    }
  }

  // keep the shadow in step with what the OS now says
  bool actual = on;
  if (!getCapsLockState(actual)) {
    actual = (osModifierFlags() & kCGEventFlagMaskAlphaShift) != 0;
  }
  m_capsPressed = actual;
  if (actual != on) {
    // macOS applies a hold requirement to Caps on some keyboards; an
    // instantaneous synthetic press can be dropped. Say so rather than
    // letting the shadow and the OS disagree silently.
    LOG_WARN("[keys] caps lock still %s after fallback press (wanted %s)", actual ? "on" : "off", on ? "on" : "off");
  }
  // ... and the tracked mask too. mapKey() decides from m_mask whether a key
  // needs Caps flipped; leaving it stale after applying the lock made the
  // first letter with Caps in its mask click Caps a second time (inverted
  // for the rest of the epoch).
  syncTrackedModifier(bit, actual);
}

void OSXKeyState::syncTrackedModifier(KeyModifierMask bit, bool on)
{
  auto &tracked = getActiveModifiersRValue();
  tracked = on ? (tracked | bit) : (tracked & ~bit);
}

double OSXKeyState::monotonicSeconds()
{
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double OSXKeyState::now() const
{
  if (m_hooks.monotonicNow) {
    return m_hooks.monotonicNow();
  }
  return monotonicSeconds();
}

int OSXKeyState::hardwareSlotForVirtualKey(uint8_t virtualKey)
{
  switch (virtualKey) {
  case s_shiftVK:
    return 0;
  case s_controlVK:
    return 1;
  case s_altVK:
    return 2;
  case s_superVK:
    return 3;
  default:
    return -1;
  }
}

void OSXKeyState::noteHardwareModifierFlags(CGEventFlags flags, double now)
{
  // Side-specific device bits only: the generic kCGEventFlagMask* bits are
  // also set by our own injection and by lock state, so they say nothing
  // about a physical key. Right-side bits are always hardware (we only ever
  // post the left virtual keys). Runs on the event-tap thread: atomics only.
  struct Side
  {
    int slot;
    CGEventFlags left;
    CGEventFlags right;
  };
  static constexpr Side kSides[] = {
      {0, NX_DEVICELSHIFTKEYMASK, NX_DEVICERSHIFTKEYMASK},
      {1, NX_DEVICELCTLKEYMASK, NX_DEVICERCTLKEYMASK},
      {2, NX_DEVICELALTKEYMASK, NX_DEVICERALTKEYMASK},
      {3, NX_DEVICELCMDKEYMASK, NX_DEVICERCMDKEYMASK},
  };
  for (const auto &side : kSides) {
    if ((flags & (side.left | side.right)) != 0) {
      m_lastHardwareModifierAt[side.slot].store(now, std::memory_order_relaxed);
    }
  }
}

void OSXKeyState::releaseLedgeredModifiers(CGEventFlags os, CGEventFlags keep)
{
  const std::set<uint8_t> injected = m_injectedModifiers;
  for (uint8_t virtualKey : injected) {
    const CGEventFlags flag = modifierFlagForVirtualKey(virtualKey);
    if (flag == 0) {
      m_injectedModifiers.erase(virtualKey);
      continue;
    }
    if ((keep & flag) != 0) {
      // re-asserted for the user's ongoing chord: stays down, stays ledgered
      LOG_DEBUG("keeping re-asserted modifier 0x%02x", virtualKey);
      continue;
    }
    if (flag == kCGEventFlagMaskAlphaShift) {
      // Caps is a lock: the OS flag is the lock STATE (setToggleState()
      // owns that) and says nothing about whether the KEY is still down.
      // We posted this Down ourselves and never the Up, so close the key.
      // A Caps key-up toggles nothing -- and m_capsPressed keeps the lock
      // state reseedShadowFlagsFromOS() just read, so the release event
      // carries it. Never done for an OS-reported lock with no injection.
      if (postHIDVirtualKey(virtualKey, false) != KERN_SUCCESS) {
        postKeyboardKey(virtualKey, false);
      }
      m_injectedModifiers.erase(virtualKey);
      LOG_INFO("released injected caps lock key 0x%02x (lock state untouched)", virtualKey);
      continue;
    }
    if ((os & flag) == 0) {
      // the OS already considers it up; nothing to release
      m_injectedModifiers.erase(virtualKey);
      continue;
    }
    setKeyboardModifiers(virtualKey, false);
    if (postHIDVirtualKey(virtualKey, false) != KERN_SUCCESS) {
      postKeyboardKey(virtualKey, false);
    }
    m_injectedModifiers.erase(virtualKey);
    LOG_INFO("released injected modifier 0x%02x", virtualKey);
  }
}

void OSXKeyState::releaseInjectedKeys(KeyModifierMask keep)
{
  // Ledger only -- the strict subset of sanitizeInjectedKeys() that can
  // never touch a modifier the user is physically holding, whatever the
  // freshness clock says. Reseed first so the release carries real flags.
  CGEventFlags keepFlags = 0;
  if ((keep & KeyModifierShift) != 0) {
    keepFlags |= kCGEventFlagMaskShift;
  }
  if ((keep & KeyModifierControl) != 0) {
    keepFlags |= kCGEventFlagMaskControl;
  }
  if ((keep & KeyModifierAlt) != 0) {
    keepFlags |= kCGEventFlagMaskAlternate;
  }
  if ((keep & KeyModifierSuper) != 0) {
    keepFlags |= kCGEventFlagMaskCommand;
  }
  reseedShadowFlagsFromOS();
  releaseLedgeredModifiers(osModifierFlags(), keepFlags);
}

void OSXKeyState::sanitizeInjectedKeys()
{
  // Start from OS truth so the release we post below carries the real
  // global flags for every other modifier.
  reseedShadowFlagsFromOS();
  const CGEventFlags os = osModifierFlags();
  const std::set<uint8_t> injected = m_injectedModifiers;
  releaseLedgeredModifiers(os);

  // Modifiers the OS reports down that we never injected: the user's, if a
  // physical key recently drove them (a hardware flagsChanged carried the
  // side-specific device bit within kHardwareModifierFreshS). Otherwise they
  // are stale -- typically an injection by a previous incarnation of this
  // process that crashed with the key held (K2): nothing on the keyboard
  // backs them, and nobody else will ever release them. The HID key map
  // (pollPressedKeys) is deliberately NOT consulted here: it reports a
  // posted modifier as down just like a physical one. Caps is deliberately
  // NOT here: its OS flag is lock state, never evidence of a held key.
  //
  // A held key emits ONE flagsChanged, so a >kHardwareModifierFreshS hold
  // reads as stale here. This sweep therefore belongs only at boundaries
  // where the user cannot be mid-gesture at this keyboard (screen enable);
  // lock/unlock/wake, disable, clear-all, enter/leave and the verifier use
  // releaseInjectedKeys().
  //
  // K5: the freshness verdict is only meaningful if a hardware press COULD
  // have stamped the clock. Two cases where it cannot, and where the user
  // is typically typing at this very keyboard: a password field owns
  // input (secure event input blinds every tap; that is the lock screen,
  // sudo, 1Password...), and the tap has not been up for the whole window
  // (epoch restart, disabled-by-timeout). Releasing a physically held
  // Shift there replaced the global flags with Shift OFF under the user's
  // fingers -- lowercase at the prompt until the next physical re-press.
  const double at = now();
  if (secureInputEnabled()) {
    LOG_INFO("[keys] secure-input=on; stale-modifier sweep skipped (hardware presses invisible)");
    return;
  }
  if (!hardwareObservableFor(kHardwareModifierFreshS, at)) {
    LOG_INFO("[keys] stale-modifier sweep skipped: hardware not observable for %.0f s", kHardwareModifierFreshS);
    return;
  }
  for (uint32_t virtualKey : {s_shiftVK, s_controlVK, s_altVK, s_superVK}) {
    const auto vk = static_cast<uint8_t>(virtualKey);
    const CGEventFlags flag = modifierFlagForVirtualKey(vk);
    if ((os & flag) == 0 || injected.contains(vk)) {
      continue;
    }
    const int slot = hardwareSlotForVirtualKey(vk);
    const double seenAt = slot < 0 ? at : m_lastHardwareModifierAt[slot].load(std::memory_order_relaxed);
    if ((at - seenAt) <= kHardwareModifierFreshS) {
      LOG_DEBUG("leaving physically held modifier 0x%02x", virtualKey);
      continue;
    }
    setKeyboardModifiers(vk, false);
    if (postHIDVirtualKey(vk, false) != KERN_SUCCESS) {
      postKeyboardKey(vk, false);
    }
    LOG_INFO("released stale modifier 0x%02x (no hardware press in %.0f s)", virtualKey, kHardwareModifierFreshS);
  }
}

void OSXKeyState::updateKeyState()
{
  reseedShadowFlagsFromOS();
  KeyState::updateKeyState();
}

void OSXKeyState::fakeAllKeysUp()
{
  // Synthetic keys first (their fakeKey() Ups drop them from the ledger),
  // then whatever the ledger still holds beyond the synthetic set -- a
  // modifier posted outside KeyState's press path. K2 gap b1: this used to
  // clear() the ledger here, forgetting such a modifier instead of
  // releasing it, and nothing downstream ever released it either.
  KeyState::fakeAllKeysUp();
  reseedShadowFlagsFromOS();
  releaseLedgeredModifiers(osModifierFlags());
  reseedShadowFlagsFromOS();
}

void OSXKeyState::setKeyboardModifiers(CGKeyCode virtualKey, bool keyDown)
{
  switch (virtualKey) {
  case s_shiftVK:
    m_shiftPressed = keyDown;
    break;
  case s_controlVK:
    m_controlPressed = keyDown;
    break;
  case s_altVK:
    m_altPressed = keyDown;
    break;
  case s_superVK:
    m_superPressed = keyDown;
    break;
  case s_capsLockVK:
    m_capsPressed = keyDown;
    break;
  default:
    LOG_VERBOSE("the key is not a modifier");
    break;
  }
}

kern_return_t OSXKeyState::postHIDVirtualKey(uint8_t virtualKey, bool postDown)
{
  // A modifier post carries the global flag word the OS will adopt
  // (kIOHIDSetGlobalEventFlags): derive it from the live flags so a
  // physical modifier the shadow never saw survives, then make the shadow
  // agree so every following non-modifier post composes the same way.
  CGEventFlags modifierFlags = 0;
  if (isModifier(virtualKey)) {
    modifierFlags = modifierEventFlags(virtualKey, postDown);
    setShadowFlags(modifierFlags);
  }

  if (m_hooks.postHIDKey) {
    return m_hooks.postHIDKey(virtualKey, postDown, modifierFlags);
  }

  NXEventData event;
  bzero(&event, sizeof(NXEventData));
  auto driver = getEventDriver();
  kern_return_t result = KERN_FAILURE;

  if (driver) {
    // Gotcha: `keyCode` must be set for all event types (including modifier key press);
    // the default zero value is interpreted as the 'a' key by some input methods (e.g. Chinese).
    event.key.keyCode = virtualKey;
    if (isModifier(virtualKey)) {
      result = IOHIDPostEvent(
          driver, NX_FLAGSCHANGED, {0, 0}, &event, kNXEventDataVersion, modifierFlags, kIOHIDSetGlobalEventFlags
      );
    } else {
      const auto eventType = postDown ? NX_KEYDOWN : NX_KEYUP;
      result = IOHIDPostEvent(driver, eventType, {0, 0}, &event, kNXEventDataVersion, 0, false);
    }
  }

  return result;
}

void OSXKeyState::postKeyboardKey(CGKeyCode virtualKey, bool keyDown)
{
  CGEventRef event = CGEventCreateKeyboardEvent(nullptr, virtualKey, keyDown);
  if (event) {
    CGEventSetFlags(event, getKeyboardEventFlags());
    deskflow::platform::markInjectedEvent(event);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
  } else {
    LOG_CRIT("unable to create keyboard event for keystroke");
  }
}

void OSXKeyState::fakeKey(const Keystroke &keystroke)
{
  switch (keystroke.m_type) {
  case Keystroke::KeyType::Button: {
    bool keyDown = keystroke.m_data.m_button.m_press;
    uint32_t client = keystroke.m_data.m_button.m_client;
    KeyButton button = keystroke.m_data.m_button.m_button;
    CGKeyCode virtualKey = mapKeyButtonToVirtualKey(button);

    LOG_VERBOSE(
        "  button=0x%04x virtualKey=0x%04x keyDown=%s client=0x%04x", button, virtualKey, keyDown ? "down" : "up",
        client
    );

    setKeyboardModifiers(virtualKey, keyDown);
    if (postHIDVirtualKey(virtualKey, keyDown) != KERN_SUCCESS) {
      LOG_WARN("fail to post hid event");
      postKeyboardKey(virtualKey, keyDown);
    }

    // remember which modifiers we hold down so sanitizeInjectedKeys() can
    // release exactly those and nothing the user is physically holding.
    if (virtualKey <= 0xff && modifierFlagForVirtualKey(static_cast<uint8_t>(virtualKey)) != 0) {
      if (keyDown) {
        m_injectedModifiers.insert(static_cast<uint8_t>(virtualKey));
      } else {
        m_injectedModifiers.erase(static_cast<uint8_t>(virtualKey));
      }
    }

    break;
  }

  case Keystroke::KeyType::Group: {
    int32_t group = keystroke.m_data.m_group.m_group;
    if (!keystroke.m_data.m_group.m_restore) {
      if (keystroke.m_data.m_group.m_absolute) {
        LOG_VERBOSE("  group %d", group);
        setGroup(group);
      } else {
        LOG_VERBOSE("  group %+d", group);
        setGroup(getEffectiveGroup(pollActiveGroup(), group));
      }

      if (pollActiveGroup() != group) {
        LOG_WARN("failed to set new keyboard layout");
      }
    }
    break;
  }
  }
}

void OSXKeyState::getKeyMapForSpecialKeys(deskflow::KeyMap &keyMap, int32_t group) const
{
  // special keys are insensitive to modifers and none are dead keys
  deskflow::KeyMap::KeyItem item;
  for (size_t i = 0; i < sizeof(s_controlKeys) / sizeof(s_controlKeys[0]); ++i) {
    const KeyEntry &entry = s_controlKeys[i];
    item.m_id = entry.m_keyID;
    item.m_group = group;
    item.m_button = mapVirtualKeyToKeyButton(entry.m_virtualKey);
    item.m_required = 0;
    item.m_sensitive = 0;
    item.m_dead = false;
    item.m_client = 0;
    deskflow::KeyMap::initModifierKey(item);
    keyMap.addKeyEntry(item);

    if (item.m_lock) {
      // all locking keys are half duplex on OS X
      keyMap.addHalfDuplexButton(item.m_button);
    }
  }

  // note:  we don't special case the number pad keys.  querying the
  // mac keyboard returns the non-keypad version of those keys but
  // a KeyState always provides a mapping from keypad keys to
  // non-keypad keys so we'll be able to generate the characters
  // anyway.
}

bool OSXKeyState::getKeyMap(deskflow::KeyMap &keyMap, int32_t group, const IOSXKeyResource &r) const
{
  if (!r.isValid()) {
    return false;
  }

  // space for all possible modifier combinations
  std::vector<bool> modifiers(r.getNumModifierCombinations());

  // make space for the keys that any single button can synthesize
  std::vector<std::pair<KeyID, bool>> buttonKeys(r.getNumTables());

  // iterate over each button
  deskflow::KeyMap::KeyItem item;
  for (uint32_t i = 0; i < r.getNumButtons(); ++i) {
    item.m_button = mapVirtualKeyToKeyButton(i);

    // the KeyIDs we've already handled
    std::set<KeyID> keys;

    // convert the entry in each table for this button to a KeyID
    for (uint32_t j = 0; j < r.getNumTables(); ++j) {
      buttonKeys[j].first = r.getKey(j, i);
      buttonKeys[j].second = deskflow::KeyMap::isDeadKey(buttonKeys[j].first);
    }

    // iterate over each character table
    for (uint32_t j = 0; j < r.getNumTables(); ++j) {
      // get the KeyID for the button/table
      KeyID id = buttonKeys[j].first;
      if (id == kKeyNone) {
        continue;
      }

      // if we've already handled the KeyID in the table then
      // move on to the next table
      if (keys.count(id) > 0) {
        continue;
      }
      keys.insert(id);

      // prepare item.  the client state is 1 for dead keys.
      item.m_id = id;
      item.m_group = group;
      item.m_dead = buttonKeys[j].second;
      item.m_client = buttonKeys[j].second ? 1 : 0;
      deskflow::KeyMap::initModifierKey(item);
      if (item.m_lock) {
        // all locking keys are half duplex on OS X
        keyMap.addHalfDuplexButton(i);
      }

      // collect the tables that map to the same KeyID.  we know it
      // can't be any earlier tables because of the check above.
      std::set<uint8_t> tables;
      tables.insert(static_cast<uint8_t>(j));
      for (uint32_t k = j + 1; k < r.getNumTables(); ++k) {
        if (buttonKeys[k].first == id) {
          tables.insert(static_cast<uint8_t>(k));
        }
      }

      // collect the modifier combinations that map to any of the
      // tables we just collected
      for (uint32_t k = 0; k < r.getNumModifierCombinations(); ++k) {
        modifiers[k] = (tables.count(r.getTableForModifier(k)) > 0);
      }

      // figure out which modifiers the key is sensitive to.  the
      // key is insensitive to a modifier if for every modifier mask
      // with the modifier bit unset in the modifiers we also find
      // the same mask with the bit set.
      //
      // we ignore a few modifiers that we know aren't important
      // for generating characters.  in fact, we want to ignore any
      // characters generated by the control key.  we don't map
      // those and instead expect the control modifier plus a key.
      uint32_t sensitive = 0;
      for (uint32_t k = 0; (1u << k) < r.getNumModifierCombinations(); ++k) {
        uint32_t bit = (1u << k);
        if ((bit << 8) == cmdKey || (bit << 8) == controlKey || (bit << 8) == rightControlKey) {
          continue;
        }
        for (uint32_t m = 0; m < r.getNumModifierCombinations(); ++m) {
          if (modifiers[m] != modifiers[m ^ bit]) {
            sensitive |= bit;
            break;
          }
        }
      }

      // find each required modifier mask.  the key can be synthesized
      // using any of the masks.
      std::set<uint32_t> required;
      for (uint32_t k = 0; k < r.getNumModifierCombinations(); ++k) {
        if ((k & sensitive) == k && modifiers[k & sensitive]) {
          required.insert(k);
        }
      }

      // now add a key entry for each key/required modifier pair.
      item.m_sensitive = mapModifiersFromOSX(sensitive << 16);
      for (std::set<uint32_t>::iterator k = required.begin(); k != required.end(); ++k) {
        item.m_required = mapModifiersFromOSX(*k << 16);
        keyMap.addKeyEntry(item);
      }
    }
  }

  return true;
}

bool OSXKeyState::mapDeskflowHotKeyToMac(
    KeyID key, KeyModifierMask mask, uint32_t &macVirtualKey, uint32_t &macModifierMask
) const
{
  // look up button for key
  KeyButton button = getButton(key, pollActiveGroup());
  if (button == 0 && key != kKeyNone) {
    return false;
  }
  macVirtualKey = mapKeyButtonToVirtualKey(button);

  // calculate modifier mask
  macModifierMask = 0;
  if ((mask & KeyModifierShift) != 0) {
    macModifierMask |= shiftKey;
  }
  if ((mask & KeyModifierControl) != 0) {
    macModifierMask |= controlKey;
  }
  if ((mask & KeyModifierAlt) != 0) {
    macModifierMask |= cmdKey;
  }
  if ((mask & KeyModifierSuper) != 0) {
    macModifierMask |= optionKey;
  }
  if ((mask & KeyModifierCapsLock) != 0) {
    macModifierMask |= alphaLock;
  }
  if ((mask & KeyModifierNumLock) != 0) {
    macModifierMask |= s_osxNumLock;
  }

  return true;
}

void OSXKeyState::handleModifierKeys(void *target, KeyModifierMask oldMask, KeyModifierMask newMask)
{
  // compute changed modifiers
  KeyModifierMask changed = (oldMask ^ newMask);

  // synthesize changed modifier keys
  if ((changed & KeyModifierShift) != 0) {
    handleModifierKey(target, s_shiftVK, kKeyShift_L, (newMask & KeyModifierShift) != 0, newMask);
  }
  if ((changed & KeyModifierControl) != 0) {
    handleModifierKey(target, s_controlVK, kKeyControl_L, (newMask & KeyModifierControl) != 0, newMask);
  }
  if ((changed & KeyModifierAlt) != 0) {
    handleModifierKey(target, s_altVK, kKeyAlt_L, (newMask & KeyModifierAlt) != 0, newMask);
  }
  if ((changed & KeyModifierSuper) != 0) {
    handleModifierKey(target, s_superVK, kKeySuper_L, (newMask & KeyModifierSuper) != 0, newMask);
  }
  if ((changed & KeyModifierCapsLock) != 0) {
    handleModifierKey(target, s_capsLockVK, kKeyCapsLock, (newMask & KeyModifierCapsLock) != 0, newMask);
  }
  if ((changed & KeyModifierNumLock) != 0) {
    handleModifierKey(target, s_numLockVK, kKeyNumLock, (newMask & KeyModifierNumLock) != 0, newMask);
  }
}

void OSXKeyState::handleModifierKey(void *target, uint32_t virtualKey, KeyID id, bool down, KeyModifierMask newMask)
{
  KeyButton button = mapVirtualKeyToKeyButton(virtualKey);
  onKey(button, down, newMask);
  sendKeyEvent(target, down, false, id, newMask, 0, button);
}

bool OSXKeyState::getGroups(AutoCFArray &groups) const
{
  return deskflow::platform::osx::runOnMainQueue([&]() -> bool {
    // get number of layouts
    CFStringRef keys[] = {kTISPropertyInputSourceCategory};
    CFStringRef values[] = {kTISCategoryKeyboardInputSource};
    AutoCFDictionary dict(
        CFDictionaryCreate(nullptr, (const void **)keys, (const void **)values, 1, nullptr, nullptr), CFRelease
    );
    AutoCFArray kbds(nullptr, CFRelease);
    {
      std::lock_guard<std::mutex> lock(g_tisMutex);
      kbds = AutoCFArray(TISCreateInputSourceList(dict.get(), false), CFRelease);
    }

    if (CFArrayGetCount(kbds.get()) > 0) {
      groups = std::move(kbds);
    } else {
      LOG_VERBOSE("can't get keyboard layouts");
      return false;
    }

    return true;
  });
}

void OSXKeyState::setGroup(int32_t group)
{
  TISInputSourceRef keyboardLayout = (TISInputSourceRef)CFArrayGetValueAtIndex(m_groups.get(), group);
  if (!keyboardLayout) {
    LOG_WARN("needed keyboard layout is null");
    return;
  }
  CFBooleanRef canBeSetted = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_tisMutex);
    AutoTISInputSourceRef source(TISCopyCurrentKeyboardInputSource(), CFRelease);
    if (source)
      canBeSetted = (CFBooleanRef)TISGetInputSourceProperty(source.get(), kTISPropertyInputSourceIsEnableCapable);
  }
  if (!canBeSetted) {
    LOG_WARN("needed keyboard layout is disabled for programmatically selection");
    return;
  }

  {
    std::lock_guard<std::mutex> lock(g_tisMutex);
    if (TISSelectInputSource(keyboardLayout) != noErr) {
      LOG_WARN("failed to set needed keyboard layout");
    }
  }

  LOG_VERBOSE("keyboard layout change to %d", group);

  // A minimal delay is needed after a group change because the
  // keyboard key event often happens immediately after.
  // Language (TIS) and event (CG) systems are not in the mutual
  // event queue and without a delay the subsequent key press
  // event could be applied before the keyboard layout would
  // actually be changed.
  Arch::sleep(.01);
}

void OSXKeyState::adjustAltGrModifier(const KeyIDs &ids, KeyModifierMask *mask, bool isCommand) const
{
  if (!isCommand) {
    for (KeyIDs::const_iterator i = ids.begin(); i != ids.end(); ++i) {
      KeyID id = *i;
      if (id != kKeyNone && ((id < 0xe000u || id > 0xefffu) || (id >= kKeyKP_Equal && id <= kKeyKP_9))) {
        *mask |= KeyModifierAltGr;
        return;
      }
    }
  }
}

KeyButton OSXKeyState::mapVirtualKeyToKeyButton(uint32_t keyCode)
{
  // 'A' maps to 0 so shift every id
  return static_cast<KeyButton>(keyCode + KeyButtonOffset);
}

uint32_t OSXKeyState::mapKeyButtonToVirtualKey(KeyButton keyButton)
{
  return static_cast<uint32_t>(keyButton - KeyButtonOffset);
}
