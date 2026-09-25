/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/KeyTypes.h"

#include <cstdint>
#include <string>

namespace deskflow {

//! Keystroke content in the log (K9).
/*!
Every key that crosses the server/client path used to be written to the core
log in cleartext at DEBUG (`onKeyDown id=65 ('a') ...`), which made typed
passwords recoverable from ~/Library/Deskflow/deskflow-core.log. These helpers
are the ONE way a KeyID, KeyButton, scan code or virtual key reaches a log
line. Content-bearing keys are replaced by `<redacted>` unless keystroke
logging is deliberately enabled (`[log] keystrokes=true` AND `[log]
level=VERBOSE`); a small allowlist of modifier, lock, navigation, editing and
function keys keeps its name because those carry no secret and the stuck-key
diagnostics need them.
*/

//! `[log] keystrokes=true` was read from settings (default false).
void setKeystrokeLoggingEnabled(bool enabled);
bool keystrokeLoggingEnabled();

//! True when typed content may actually land in the log right now: the flag
//! is set AND the log filter is VERBOSE (the level above DEBUG).
bool keystrokeLoggingActive();

//! Keys whose name carries no typed content: modifiers, locks, Return, Tab,
//! Escape, BackSpace, Delete, Insert, arrows, Home/End/Page*, F1-F35, keypad
//! navigation, Menu/Print/Pause/Break.
bool isKeyLogAllowlisted(KeyID id);

//! `id=<n> (<name>) button=0x<b>` for allowlisted keys or when keystroke
//! logging is active; `id=<redacted> button=<redacted>` otherwise.
std::string keyLogToken(KeyID id, KeyButton button);

//! `id=<n> (<name>)` / `id=<redacted>` -- for lines that carry no button.
std::string keyLogToken(KeyID id);

//! A bare button / scan code / virtual key / keycode with no KeyID in hand:
//! `0x<code>` when keystroke logging is active, `<redacted>` otherwise.
std::string keyLogCode(uint32_t code);

} // namespace deskflow
