/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/KeyTypes.h"

namespace deskflow {

//! True for a key press that pastes: Cmd/Ctrl(+Meta)+V or Shift+Insert
/*!
Only these (and clicks) flush a pending clipboard read/send before they are
forwarded; plain typing and modifier presses never pay for a clipboard read.
*/
inline bool isPasteChord(KeyID id, KeyModifierMask mask)
{
  constexpr KeyModifierMask command = KeyModifierControl | KeyModifierSuper | KeyModifierMeta;
  if ((id == 'v' || id == 'V') && (mask & command) != 0) {
    return true;
  }
  return id == kKeyInsert && (mask & KeyModifierShift) != 0;
}

} // namespace deskflow
