/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/KeyboardRelayMap.h"

#include "deskflow/KeyTypes.h"
#include "platform/OSXAutoTypes.h"
#include "platform/OSXMainQueue.h"

#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#import <Cocoa/Cocoa.h>
#import <IOKit/hidsystem/ev_keymap.h>

namespace deskflow::coordination {

namespace {

KeyModifierMask mapModifiers(CGEventFlags flags)
{
  KeyModifierMask mask = 0;
  if ((flags & kCGEventFlagMaskShift) != 0) {
    mask |= KeyModifierShift;
  }
  if ((flags & kCGEventFlagMaskControl) != 0) {
    mask |= KeyModifierControl;
  }
  if ((flags & kCGEventFlagMaskAlternate) != 0) {
    mask |= KeyModifierAlt;
  }
  if ((flags & kCGEventFlagMaskCommand) != 0) {
    mask |= KeyModifierSuper;
  }
  if ((flags & kCGEventFlagMaskAlphaShift) != 0) {
    mask |= KeyModifierCapsLock;
  }
  return mask;
}

KeyID modifierKeyIdFromVirtualKey(CGKeyCode vk)
{
  switch (vk) {
  case kVK_Shift:
    return kKeyShift_L;
  case kVK_RightShift:
    return kKeyShift_R;
  case kVK_Control:
    return kKeyControl_L;
  case kVK_RightControl:
    return kKeyControl_R;
  case kVK_Option:
    return kKeyAlt_L;
  case kVK_RightOption:
    return kKeyAlt_R;
  case kVK_Command:
    return kKeySuper_L;
  case kVK_RightCommand:
    return kKeySuper_R;
  case kVK_CapsLock:
    return kKeyCapsLock;
  default:
    return kKeyNone;
  }
}

bool modifierIsDown(CGKeyCode vk, CGEventFlags flags)
{
  switch (vk) {
  case kVK_Shift:
  case kVK_RightShift:
    return (flags & kCGEventFlagMaskShift) != 0;
  case kVK_Control:
  case kVK_RightControl:
    return (flags & kCGEventFlagMaskControl) != 0;
  case kVK_Option:
  case kVK_RightOption:
    return (flags & kCGEventFlagMaskAlternate) != 0;
  case kVK_Command:
  case kVK_RightCommand:
    return (flags & kCGEventFlagMaskCommand) != 0;
  case kVK_CapsLock:
    return (flags & kCGEventFlagMaskAlphaShift) != 0;
  default:
    return false;
  }
}

KeyID translateVirtualKey(CGKeyCode vk, CGEventFlags flags, const UCKeyboardLayout *layout, UInt32 keyboardType)
{
  if (const KeyID modifier = modifierKeyIdFromVirtualKey(vk); modifier != kKeyNone) {
    return modifier;
  }

  switch (vk) {
  case kVK_Return:
    return kKeyReturn;
  case kVK_Tab:
    return kKeyTab;
  case kVK_Space:
    return static_cast<KeyID>(' ');
  case kVK_Delete:
    return kKeyBackSpace;
  case kVK_ForwardDelete:
    return kKeyDelete;
  case kVK_Escape:
    return kKeyEscape;
  case kVK_LeftArrow:
    return kKeyLeft;
  case kVK_RightArrow:
    return kKeyRight;
  case kVK_UpArrow:
    return kKeyUp;
  case kVK_DownArrow:
    return kKeyDown;
  case kVK_Home:
    return kKeyHome;
  case kVK_End:
    return kKeyEnd;
  case kVK_PageUp:
    return kKeyPageUp;
  case kVK_PageDown:
    return kKeyPageDown;
  default:
    break;
  }

  if (layout == nullptr) {
    return kKeyNone;
  }

  // Translate with the live Shift/CapsLock state so Shift+A relays as 'A',
  // matching the normal server capture path (OSXKeyState::mapKeyFromEvent).
  // With a modifier-less KeyID ('a'), the target's KeyMap would actively
  // RELEASE Shift to reproduce the lowercase glyph, dropping the modifier.
  // Command/Control/Option are excluded, mirroring the normal path, which
  // strips them so the base glyph is sent and the mask applies them remotely.
  UInt32 modifierKeyState = 0;
  if ((flags & kCGEventFlagMaskShift) != 0) {
    modifierKeyState |= (shiftKey >> 8);
  }
  if ((flags & kCGEventFlagMaskAlphaShift) != 0) {
    modifierKeyState |= (alphaLock >> 8);
  }

  UInt32 deadKeyState = 0;
  UniChar chars[4];
  UniCharCount count = 0;
  if (UCKeyTranslate(
          layout, vk, kUCKeyActionDisplay, modifierKeyState, keyboardType, kUCKeyTranslateNoDeadKeysMask, &deadKeyState,
          4, &count, chars
      ) == noErr &&
      count > 0) {
    const UniChar c = chars[0];
    if (c >= 32) {
      return static_cast<KeyID>(c);
    }
  }
  return kKeyNone;
}

} // namespace

bool mapRelayKeyFromCgEvent(
    void *cgEvent, Message::KeyPhase &phase, KeyID &id, KeyModifierMask &mask, KeyButton &button
)
{
  auto *event = static_cast<CGEventRef>(cgEvent);
  const CGEventType type = CGEventGetType(event);
  if (type == kCGEventKeyDown) {
    phase = CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat) != 0 ? Message::KeyPhase::Repeat
                                                                                : Message::KeyPhase::Down;
  } else if (type == kCGEventKeyUp) {
    phase = Message::KeyPhase::Up;
  } else {
    return false;
  }

  const CGKeyCode vk = static_cast<CGKeyCode>(CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
  button = static_cast<KeyButton>(vk);
  mask = mapModifiers(CGEventGetFlags(event));

  if (type == kCGEventKeyUp) {
    id = kKeyNone;
    return true;
  }

  // TIS APIs assert the main dispatch queue on macOS 14+; relay tap runs on a
  // dedicated CFRunLoop thread (see OSXKeyboardRelayMonitor).
  const UInt32 keyboardType = LMGetKbdType();
  CFDataRef layoutRef = deskflow::platform::osx::runOnMainQueue([]() -> CFDataRef {
    std::lock_guard<std::mutex> lock(g_tisMutex);
    AutoTISInputSourceRef source(TISCopyCurrentKeyboardInputSource(), CFRelease);
    if (!source) {
      return nullptr;
    }
    CFDataRef ref = static_cast<CFDataRef>(TISGetInputSourceProperty(source.get(), kTISPropertyUnicodeKeyLayoutData));
    if (ref) {
      CFRetain(ref);
    }
    return ref;
  });
  AutoCFData layoutData(layoutRef, CFRelease);
  const auto *layout =
      layoutData ? reinterpret_cast<const UCKeyboardLayout *>(CFDataGetBytePtr(layoutData.get())) : nullptr;
  id = translateVirtualKey(vk, CGEventGetFlags(event), layout, keyboardType);

  return id != kKeyNone || phase == Message::KeyPhase::Repeat;
}

bool mapRelayModifierFromCgEvent(
    void *cgEvent, Message::KeyPhase &phase, KeyID &id, KeyModifierMask &mask, KeyButton &button
)
{
  auto *event = static_cast<CGEventRef>(cgEvent);
  if (CGEventGetType(event) != kCGEventFlagsChanged) {
    return false;
  }

  const CGKeyCode vk = static_cast<CGKeyCode>(CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
  const CGEventFlags flags = CGEventGetFlags(event);
  button = static_cast<KeyButton>(vk);
  mask = mapModifiers(flags);
  id = modifierKeyIdFromVirtualKey(vk);
  if (id == kKeyNone) {
    return false;
  }

  if (modifierIsDown(vk, flags)) {
    phase = Message::KeyPhase::Down;
  } else {
    phase = Message::KeyPhase::Up;
    id = kKeyNone;
  }
  return true;
}

// NX key type -> neutral media KeyID. Kept in sync with the inverse table in
// platform/OSXMediaKeySupport.m (convertNXKeyTypeToKeyID); duplicated here so
// coordination stays independent of the platform layer and remains testable.
KeyID mediaKeyIdFromNxType(uint32_t nxKeyType)
{
  switch (nxKeyType) {
  case NX_KEYTYPE_SOUND_UP:
    return kKeyAudioUp;
  case NX_KEYTYPE_SOUND_DOWN:
    return kKeyAudioDown;
  case NX_KEYTYPE_MUTE:
    return kKeyAudioMute;
  case NX_KEYTYPE_PLAY:
    return kKeyAudioPlay;
  case NX_KEYTYPE_FAST:
  case NX_KEYTYPE_NEXT:
    return kKeyAudioNext;
  case NX_KEYTYPE_REWIND:
  case NX_KEYTYPE_PREVIOUS:
    return kKeyAudioPrev;
  case NX_KEYTYPE_BRIGHTNESS_UP:
    return kKeyBrightnessUp;
  case NX_KEYTYPE_BRIGHTNESS_DOWN:
    return kKeyBrightnessDown;
  case NX_KEYTYPE_EJECT:
    return kKeyEject;
  default:
    return kKeyNone;
  }
}

bool mapRelayMediaKeyFromCgEvent(void *cgEvent, KeyID &id, bool &down)
{
  auto *event = static_cast<CGEventRef>(cgEvent);
  // System-defined events carry consumer/media keys (NX_SYSDEFINED == 14).
  if (CGEventGetType(event) != static_cast<CGEventType>(NX_SYSDEFINED)) {
    return false;
  }
  // The tap thread has no ambient autorelease pool; +eventWithCGEvent: returns
  // an autoreleased NSEvent that would otherwise leak on every media event.
  @autoreleasepool {
    NSEvent *nsEvent = nil;
    @try {
      nsEvent = [NSEvent eventWithCGEvent:event];
    } @catch (NSException *e) {
      return false;
    }
    // subtype 8 == NX_SUBTYPE_AUX_CONTROL_BUTTONS (media/consumer keys).
    if (nsEvent == nil || [nsEvent subtype] != NX_SUBTYPE_AUX_CONTROL_BUTTONS) {
      return false;
    }
    const NSInteger data1 = [nsEvent data1];
    const uint32_t nxType = static_cast<uint32_t>((data1 & 0xFFFF0000) >> 16);
    id = mediaKeyIdFromNxType(nxType);
    down = (data1 & 0x100) == 0; // bit 8 set == key up
  }
  return id != kKeyNone;
}

} // namespace deskflow::coordination
