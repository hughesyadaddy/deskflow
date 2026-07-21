/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/KeyboardRelayMap.h"

#include "deskflow/KeyTypes.h"

#include <windows.h>

namespace deskflow::coordination {

namespace {

KeyModifierMask activeModifiers()
{
  KeyModifierMask mask = 0;
  if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
    mask |= KeyModifierShift;
  }
  if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
    mask |= KeyModifierControl;
  }
  if (GetAsyncKeyState(VK_MENU) & 0x8000) {
    mask |= KeyModifierAlt;
  }
  if (GetAsyncKeyState(VK_LWIN) & 0x8000 || GetAsyncKeyState(VK_RWIN) & 0x8000) {
    mask |= KeyModifierSuper;
  }
  if (GetKeyState(VK_CAPITAL) & 1) {
    mask |= KeyModifierCapsLock;
  }
  return mask;
}

} // namespace

KeyID mapRelayVirtualKey(int vkCode, bool shift, bool capsLock)
{
  // Contiguous ranges (MSVC has no case ranges). Both sides are contiguous:
  // VK_F1..VK_F24 = 0x70..0x87, kKeyF1..kKeyF24 = 0xEFBE..0xEFD5;
  // VK_NUMPAD0..9 = 0x60..0x69, kKeyKP_0..kKeyKP_9 = 0xEFB0..0xEFB9.
  if (vkCode >= VK_F1 && vkCode <= VK_F24) {
    return kKeyF1 + static_cast<KeyID>(vkCode - VK_F1);
  }
  if (vkCode >= VK_NUMPAD0 && vkCode <= VK_NUMPAD9) {
    return kKeyKP_0 + static_cast<KeyID>(vkCode - VK_NUMPAD0);
  }

  switch (vkCode) {
  case VK_SNAPSHOT:
    return kKeyPrint;
  case VK_INSERT:
    return kKeyInsert;
  case VK_APPS:
    return kKeyMenu;
  case VK_NUMLOCK:
    return kKeyNumLock;
  case VK_SCROLL:
    return kKeyScrollLock;
  case VK_PAUSE:
    return kKeyPause;
  case VK_CLEAR:
    return kKeyClear;
  case VK_MULTIPLY:
    return kKeyKP_Multiply;
  case VK_ADD:
    return kKeyKP_Add;
  case VK_SUBTRACT:
    return kKeyKP_Subtract;
  case VK_DECIMAL:
    return kKeyKP_Decimal;
  case VK_DIVIDE:
    return kKeyKP_Divide;
  case VK_RETURN:
    return kKeyReturn;
  case VK_TAB:
    return kKeyTab;
  case VK_SPACE:
    return static_cast<KeyID>(' ');
  case VK_BACK:
    return kKeyBackSpace;
  case VK_DELETE:
    return kKeyDelete;
  case VK_ESCAPE:
    return kKeyEscape;
  case VK_LEFT:
    return kKeyLeft;
  case VK_RIGHT:
    return kKeyRight;
  case VK_UP:
    return kKeyUp;
  case VK_DOWN:
    return kKeyDown;
  case VK_HOME:
    return kKeyHome;
  case VK_END:
    return kKeyEnd;
  case VK_PRIOR:
    return kKeyPageUp;
  case VK_NEXT:
    return kKeyPageDown;
  // Consumer-control keys: the LL keyboard hook delivers these VKs, but they
  // are non-printable so ToUnicodeEx below yields nothing. Map them to neutral
  // media KeyIDs so the fleet relay forwards them to the cursor host, where the
  // media-VK inject table actuates them. (Brightness has no VK -- it is
  // firmware/consumer-HID and never reaches this hook.)
  case VK_VOLUME_MUTE:
    return kKeyAudioMute;
  case VK_VOLUME_DOWN:
    return kKeyAudioDown;
  case VK_VOLUME_UP:
    return kKeyAudioUp;
  case VK_MEDIA_NEXT_TRACK:
    return kKeyAudioNext;
  case VK_MEDIA_PREV_TRACK:
    return kKeyAudioPrev;
  case VK_MEDIA_PLAY_PAUSE:
    return kKeyAudioPlay;
  // VK_MEDIA_STOP intentionally omitted: the macOS injector has no NX key
  // type for stop, so it cannot actuate on a Mac target.
  case VK_LSHIFT:
  case VK_SHIFT:
    return kKeyShift_L;
  case VK_RSHIFT:
    return kKeyShift_R;
  case VK_LCONTROL:
  case VK_CONTROL:
    return kKeyControl_L;
  case VK_RCONTROL:
    return kKeyControl_R;
  case VK_LMENU:
  case VK_MENU:
    return kKeyAlt_L;
  case VK_RMENU:
    return kKeyAlt_R;
  case VK_LWIN:
    return kKeySuper_L;
  case VK_RWIN:
    return kKeySuper_R;
  case VK_CAPITAL:
    return kKeyCapsLock;
  default:
    break;
  }

  // Translate with the live Shift/CapsLock state so Shift+A relays as 'A',
  // matching the normal server capture path. With a modifier-less KeyID
  // ('a'), the target's KeyMap would actively RELEASE Shift to reproduce the
  // lowercase glyph, dropping the modifier. Ctrl/Alt are excluded so the base
  // glyph is sent and the mask applies them remotely (and to avoid ToUnicodeEx
  // emitting control characters).
  BYTE keyboardState[256] = {};
  if (shift) {
    keyboardState[VK_SHIFT] = 0x80;
  }
  if (capsLock) {
    keyboardState[VK_CAPITAL] = 0x01;
  }
  // Use the FOREGROUND window's layout, not the hook thread's:
  // GetKeyboardLayout(0) returns this thread's layout, seeded at thread
  // creation and never tracking the user's per-application input locale --
  // wrong glyphs on layout-switching systems.
  const HWND foreground = GetForegroundWindow();
  const HKL layout = GetKeyboardLayout(foreground != nullptr ? GetWindowThreadProcessId(foreground, nullptr) : 0);
  WCHAR buffer[8] = {};
  const int rc = ToUnicodeEx(static_cast<UINT>(vkCode), 0, keyboardState, buffer, 8, 0, layout);
  if (rc == 1 && buffer[0] >= 32) {
    return static_cast<KeyID>(buffer[0]);
  }
  return kKeyNone;
}

bool mapRelayKeyFromHook(
    int vkCode, int scanCode, bool isExtended, bool keyUp, bool isRepeat, KeyID &id, KeyModifierMask &mask,
    KeyButton &button, Message::KeyPhase &phase
)
{
  (void)isExtended;
  (void)scanCode;

  phase = keyUp ? Message::KeyPhase::Up : (isRepeat ? Message::KeyPhase::Repeat : Message::KeyPhase::Down);
  button = static_cast<KeyButton>(vkCode);
  mask = activeModifiers();
  if (vkCode == VK_CAPITAL && !keyUp && !isRepeat) {
    // The LL hook fires before the OS commits the toggle, so the sampled caps
    // state is the PRE-toggle value; flip it so the relayed mask carries the
    // post-toggle intent deterministically instead of racing the OS.
    mask ^= KeyModifierCapsLock;
  }
  id = mapRelayVirtualKey(vkCode, (mask & KeyModifierShift) != 0, (mask & KeyModifierCapsLock) != 0);
  if (keyUp) {
    // Release is matched on the target by BUTTON, so the id is cleared. But
    // the consumed/mapped decision must MIRROR the Down's: a key whose Down
    // is not relayable leaks to the local OS, and its Up must leak too or
    // the key sticks down locally.
    const bool relayable = id != kKeyNone;
    id = kKeyNone;
    return relayable;
  }
  // No isRepeat escape hatch: an unmapped key's repeats follow its leaked
  // local Down, same as the Up (the monitor's ledger enforces destination).
  return id != kKeyNone;
}

} // namespace deskflow::coordination
