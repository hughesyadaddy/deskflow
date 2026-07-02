/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <cctype>
#include <string>

namespace deskflow::common {

//! Case-insensitive machine/screen name comparison.
/*!
Fleet configs mix casings in practice (computerName=Hackintosh vs peer
entry hackintosh vs layout screen Hackintosh); every name lookup in the
coordination chain must tolerate that or keyboard routing silently drops.
*/
inline bool namesEqual(const std::string &a, const std::string &b)
{
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

//! True when \p cursorHost names this machine's screen/host.
inline bool cursorHostIsLocal(const std::string &selfName, const std::string &cursorHost)
{
  return !cursorHost.empty() && namesEqual(selfName, cursorHost);
}

} // namespace deskflow::common
