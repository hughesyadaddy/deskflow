/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/KeyTypes.h"
#include "base/String.h"

#include <array>
#include <string>
#include <vector>

namespace deskflow::server {

constexpr KeyModifierMask kChordRemapMods =
    KeyModifierShift | KeyModifierControl | KeyModifierAlt | KeyModifierSuper;

struct ChordRemapEntry
{
  std::string screen;
  KeyModifierMask inMods = 0;
  KeyID inKey = kKeyNone;
  KeyModifierMask outMods = 0;
  KeyID outKey = kKeyNone;

  bool operator==(const ChordRemapEntry &other) const
  {
    return screen == other.screen && inMods == other.inMods && inKey == other.inKey &&
           outMods == other.outMods && outKey == other.outKey;
  }
};

inline constexpr const char *kDefaultChordRemapScreen = "tiny11";

inline const std::array<ChordRemapEntry, 11> kDefaultTiny11ChordRemaps = {{
    {kDefaultChordRemapScreen, KeyModifierControl | KeyModifierAlt, kKeyF12, KeyModifierSuper, kKeyTab},
    {kDefaultChordRemapScreen, KeyModifierSuper, kKeyTab, KeyModifierAlt, kKeyTab},
    {kDefaultChordRemapScreen, KeyModifierSuper, 'h', KeyModifierSuper, kKeyDown},
    {kDefaultChordRemapScreen, KeyModifierSuper, 'n', KeyModifierControl, 'n'},
    {kDefaultChordRemapScreen, KeyModifierSuper, 'q', KeyModifierAlt, kKeyF4},
    {kDefaultChordRemapScreen, KeyModifierSuper, 'r', KeyModifierControl, 'r'},
    {kDefaultChordRemapScreen, KeyModifierSuper, 't', KeyModifierControl, 't'},
    {kDefaultChordRemapScreen, KeyModifierSuper, 'v', KeyModifierControl, 'v'},
    {kDefaultChordRemapScreen, KeyModifierSuper, 'w', KeyModifierControl, 'w'},
    {kDefaultChordRemapScreen, KeyModifierSuper, 'x', KeyModifierControl, 'x'},
    {kDefaultChordRemapScreen, KeyModifierSuper, '`', KeyModifierControl, kKeyTab},
}};

inline bool needsHoldThrough(const ChordRemapEntry &entry)
{
  const auto chord = kChordRemapMods;
  const auto in = entry.inMods & chord;
  const auto out = entry.outMods & chord;
  return entry.inKey == entry.outKey && out != in && out != 0;
}

inline bool applyChordRemap(
    KeyID &id, KeyModifierMask &mask, const std::vector<ChordRemapEntry> &table, const std::string &screen
)
{
  for (const auto &entry : table) {
    if (!deskflow::string::CaselessCmp::equal(entry.screen, screen)) {
      continue;
    }
    if (entry.inKey == id && (mask & kChordRemapMods) == entry.inMods) {
      id = entry.outKey;
      mask = (mask & ~kChordRemapMods) | entry.outMods;
      return true;
    }
  }
  return false;
}

} // namespace deskflow::server
