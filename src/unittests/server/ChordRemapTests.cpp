/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ChordRemapTests.h"

#include "server/ChordRemapTypes.h"
#include "server/Config.h"

#include <vector>

#include <sstream>

using namespace deskflow::server;

void ChordRemapTests::applyChordRemap_exactMatch()
{
  KeyID id = kKeyTab;
  KeyModifierMask mask = KeyModifierSuper;
  std::vector<ChordRemapEntry> table = {
      {kDefaultChordRemapScreen, KeyModifierSuper, kKeyTab, KeyModifierAlt, kKeyTab},
  };

  QVERIFY(applyChordRemap(id, mask, table, kDefaultChordRemapScreen));
  QCOMPARE(id, kKeyTab);
  QCOMPARE(mask, KeyModifierAlt);
}

void ChordRemapTests::applyChordRemap_wrongScreen()
{
  KeyID id = kKeyTab;
  KeyModifierMask mask = KeyModifierSuper;
  std::vector<ChordRemapEntry> table = {
      {kDefaultChordRemapScreen, KeyModifierSuper, kKeyTab, KeyModifierAlt, kKeyTab},
  };

  QVERIFY(!applyChordRemap(id, mask, table, "hackintosh"));
  QCOMPARE(mask, KeyModifierSuper);
}

void ChordRemapTests::applyChordRemap_preservesCapsLock()
{
  KeyID id = kKeyTab;
  KeyModifierMask mask = KeyModifierSuper | KeyModifierCapsLock;
  std::vector<ChordRemapEntry> table = {
      {kDefaultChordRemapScreen, KeyModifierSuper, kKeyTab, KeyModifierAlt, kKeyTab},
  };

  QVERIFY(applyChordRemap(id, mask, table, kDefaultChordRemapScreen));
  QCOMPARE(mask, KeyModifierAlt | KeyModifierCapsLock);
}

void ChordRemapTests::applyChordRemap_firstMatchPrecedence()
{
  KeyID id = 'q';
  KeyModifierMask mask = KeyModifierSuper;
  std::vector<ChordRemapEntry> table = {
      {kDefaultChordRemapScreen, KeyModifierSuper, 'q', KeyModifierControl, 'q'},
      {kDefaultChordRemapScreen, KeyModifierSuper, 'q', KeyModifierAlt, kKeyF4},
  };

  QVERIFY(applyChordRemap(id, mask, table, kDefaultChordRemapScreen));
  QCOMPARE(id, 'q');
  QCOMPARE(mask, KeyModifierControl);
}

void ChordRemapTests::needsHoldThrough_defaultRows()
{
  for (const auto &entry : kDefaultTiny11ChordRemaps) {
    const bool expected = entry.inKey == entry.outKey && entry.outMods != entry.inMods && entry.outMods != 0;
    QCOMPARE(needsHoldThrough(entry), expected);
  }
}

void ChordRemapTests::seedDefaultChordRemaps_whenTiny11Present()
{
  const std::string conf =
      "section: screens\n"
      "\thackintosh:\n"
      "\ttiny11:\n"
      "end\n\n"
      "section: links\n"
      "end\n\n"
      "section: options\n"
      "end\n\n";
  std::istringstream in(conf);
  ConfigReadContext context(in);
  Config config(nullptr);
  config.read(context);
  QCOMPARE(config.getChordRemaps().size(), kDefaultTiny11ChordRemaps.size());
}

void ChordRemapTests::seedDefaultChordRemaps_skipsWithoutTiny11()
{
  const std::string conf =
      "section: screens\n"
      "\thackintosh:\n"
      "end\n\n"
      "section: links\n"
      "end\n\n"
      "section: options\n"
      "end\n\n";
  std::istringstream in(conf);
  ConfigReadContext context(in);
  Config config(nullptr);
  config.read(context);
  QVERIFY(config.getChordRemaps().empty());
}

QTEST_MAIN(ChordRemapTests)
