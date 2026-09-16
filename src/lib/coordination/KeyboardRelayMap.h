/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "coordination/CoordinationProtocol.h"
#include "deskflow/KeyTypes.h"

namespace deskflow::coordination {

#if defined(__APPLE__)
//! \p allowLayoutLookup false skips the keyboard-layout glyph translation
//! (a main-queue hop from the tap thread): only fixed-table keys map.
bool mapRelayKeyFromCgEvent(
    void *cgEvent, Message::KeyPhase &phase, KeyID &id, KeyModifierMask &mask, KeyButton &button,
    bool allowLayoutLookup = true
);

//! macOS modifier keys arrive as kCGEventFlagsChanged, not key down/up.
/*!
\p capsLockOn is the caller-owned last-known Caps Lock toggle state. Caps
generates flagsChanged on BOTH the toggling press and the (state-preserving)
release, and the flags carry the toggle state rather than the key's travel --
so deciding press/release from the flags alone double-relays one toggle
direction and drops the other. A caps event relays exactly one Down per
observed state CHANGE (half-duplex targets toggle once per Down); the
release edge and repeats relay nothing. The function updates \p capsLockOn.
*/
bool mapRelayModifierFromCgEvent(
    void *cgEvent, Message::KeyPhase &phase, KeyID &id, KeyModifierMask &mask, KeyButton &button, bool &capsLockOn
);

//! Neutral media KeyID for an IOKit NX key type (kKeyNone if unmapped). Pure.
KeyID mediaKeyIdFromNxType(uint32_t nxKeyType);

//! Decode a system-defined (consumer/media) CGEvent. Returns true only for a
//! recognized media key; sets \p id and \p down (press vs release).
bool mapRelayMediaKeyFromCgEvent(void *cgEvent, KeyID &id, bool &down);
#elif defined(_WIN32)
//! Samples the modifier state from the OS (GetAsyncKeyState/GetKeyState).
//! The OS view misses modifiers whose Down the LL hook swallowed; the
//! monitor uses the overload below with its own shadow instead.
bool mapRelayKeyFromHook(
    int vkCode, int scanCode, bool isExtended, bool keyUp, bool isRepeat, KeyID &id, KeyModifierMask &mask,
    KeyButton &button, Message::KeyPhase &phase
);

//! Same, with the caller-resolved modifier state \p modifiers (relayed as
//! the mask and used for the Shift/CapsLock glyph translation).
bool mapRelayKeyFromHook(
    int vkCode, int scanCode, bool isExtended, bool keyUp, bool isRepeat, KeyModifierMask modifiers, KeyID &id,
    KeyModifierMask &mask, KeyButton &button, Message::KeyPhase &phase
);

//! Shift/Ctrl/Alt/Win the OS currently reports held (GetAsyncKeyState);
//! never CapsLock. ORed with the hook's shadow (KeyboardRelayModifierShadow).
KeyModifierMask relayOsHeldModifiers();

//! Translate a virtual key to a KeyID using the given Shift/CapsLock state so
//! shifted glyphs relay as the character the user actually typed.
KeyID mapRelayVirtualKey(int vkCode, bool shift, bool capsLock);
#endif

} // namespace deskflow::coordination
