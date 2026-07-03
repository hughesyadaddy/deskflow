/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/KeyTypes.h"

namespace deskflow::coordination {

//! Ctrl+Alt+Shift+Escape: force the keyboard back to the machine it is
//! physically attached to, regardless of relay or fleet cursor state.
/*!
Detected at the lowest level of both grab paths (server active-screen
relay and client keyboard-relay hook) so a wedged or stale fleet state
can never lock the user out of the keyboard in front of them.
*/
inline bool isKeyboardRescueChord(KeyID id, KeyModifierMask mask)
{
  constexpr KeyModifierMask required = KeyModifierShift | KeyModifierControl | KeyModifierAlt;
  return id == kKeyEscape && (mask & required) == required;
}

} // namespace deskflow::coordination
