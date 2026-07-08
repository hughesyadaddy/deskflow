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
bool mapRelayKeyFromCgEvent(
    void *cgEvent, Message::KeyPhase &phase, KeyID &id, KeyModifierMask &mask, KeyButton &button
);

//! macOS modifier keys arrive as kCGEventFlagsChanged, not key down/up.
bool mapRelayModifierFromCgEvent(
    void *cgEvent, Message::KeyPhase &phase, KeyID &id, KeyModifierMask &mask, KeyButton &button
);

//! Neutral media KeyID for an IOKit NX key type (kKeyNone if unmapped). Pure.
KeyID mediaKeyIdFromNxType(uint32_t nxKeyType);

//! Decode a system-defined (consumer/media) CGEvent. Returns true only for a
//! recognized media key; sets \p id and \p down (press vs release).
bool mapRelayMediaKeyFromCgEvent(void *cgEvent, KeyID &id, bool &down);
#elif defined(_WIN32)
bool mapRelayKeyFromHook(
    int vkCode, int scanCode, bool isExtended, bool keyUp, bool isRepeat, KeyID &id, KeyModifierMask &mask,
    KeyButton &button, Message::KeyPhase &phase
);

//! Translate a virtual key to a KeyID using the given Shift/CapsLock state so
//! shifted glyphs relay as the character the user actually typed.
KeyID mapRelayVirtualKey(int vkCode, bool shift, bool capsLock);
#endif

} // namespace deskflow::coordination
