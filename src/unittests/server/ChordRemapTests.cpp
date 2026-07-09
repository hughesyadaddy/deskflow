/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ChordRemapTests.h"

#include "server/ChordRemapTypes.h"
#include "server/Config.h"

#include <sstream>
#include <vector>

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

void ChordRemapTests::applyChordRemap_noMatch_wrongKey()
{
  KeyID id = 'q';
  KeyModifierMask mask = KeyModifierSuper;
  const KeyID originalId = id;
  const KeyModifierMask originalMask = mask;
  std::vector<ChordRemapEntry> table = {
      {kDefaultChordRemapScreen, KeyModifierSuper, kKeyTab, KeyModifierAlt, kKeyTab},
  };

  QVERIFY(!applyChordRemap(id, mask, table, kDefaultChordRemapScreen));
  QCOMPARE(id, originalId);
  QCOMPARE(mask, originalMask);
}

void ChordRemapTests::applyChordRemap_noMatch_extraModifier()
{
  KeyID id = kKeyTab;
  KeyModifierMask mask = KeyModifierSuper | KeyModifierShift;
  const KeyID originalId = id;
  const KeyModifierMask originalMask = mask;
  std::vector<ChordRemapEntry> table = {
      {kDefaultChordRemapScreen, KeyModifierSuper, kKeyTab, KeyModifierAlt, kKeyTab},
  };

  QVERIFY(!applyChordRemap(id, mask, table, kDefaultChordRemapScreen));
  QCOMPARE(id, originalId);
  QCOMPARE(mask, originalMask);
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
  static const bool kExpected[] = {
      false, // Ctrl+Alt+F12 → Super+Tab
      true,  // Super+Tab → Alt+Tab
      false, // Super+H → Super+Down
      true,  // Super+N → Ctrl+N
      false, // Super+Q → Alt+F4
      true,  // Super+R → Ctrl+R
      true,  // Super+T → Ctrl+T
      true,  // Super+V → Ctrl+V
      true,  // Super+W → Ctrl+W
      true,  // Super+X → Ctrl+X
      false, // Super+` → Ctrl+Tab
  };

  QCOMPARE(kDefaultTiny11ChordRemaps.size(), std::size(kExpected));
  for (std::size_t i = 0; i < kDefaultTiny11ChordRemaps.size(); ++i) {
    QCOMPARE(needsHoldThrough(kDefaultTiny11ChordRemaps[i]), kExpected[i]);
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
  for (std::size_t i = 0; i < kDefaultTiny11ChordRemaps.size(); ++i) {
    QCOMPARE(config.getChordRemaps()[i], kDefaultTiny11ChordRemaps[i]);
  }
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

void ChordRemapTests::seedDefaultChordRemaps_skipsWhenSectionPresent()
{
  const std::string conf =
      "section: screens\n"
      "\thackintosh:\n"
      "\ttiny11:\n"
      "end\n\n"
      "section: links\n"
      "end\n\n"
      "section: options\n"
      "end\n\n"
      "section: chordRemaps\n"
      "\ttiny11:\n"
      "\t\tchordRemap(Super+Tab) = Alt+Tab\n"
      "end\n\n";
  std::istringstream in(conf);
  ConfigReadContext context(in);
  Config config(nullptr);
  config.read(context);
  QCOMPARE(config.getChordRemaps().size(), 1u);
  QCOMPARE(config.getChordRemaps()[0].inMods, KeyModifierSuper);
  QCOMPARE(config.getChordRemaps()[0].inKey, kKeyTab);
}

void ChordRemapTests::seedDefaultChordRemaps_skipsWhenSectionEmpty()
{
  const std::string conf =
      "section: screens\n"
      "\thackintosh:\n"
      "\ttiny11:\n"
      "end\n\n"
      "section: links\n"
      "end\n\n"
      "section: options\n"
      "end\n\n"
      "section: chordRemaps\n"
      "end\n\n";
  std::istringstream in(conf);
  ConfigReadContext context(in);
  Config config(nullptr);
  config.read(context);
  QVERIFY(config.getChordRemaps().empty());
}

void ChordRemapTests::applyChordRemap_caseInsensitiveScreen()
{
  KeyID id = kKeyTab;
  KeyModifierMask mask = KeyModifierSuper;
  std::vector<ChordRemapEntry> table = {
      {"Tiny11", KeyModifierSuper, kKeyTab, KeyModifierAlt, kKeyTab},
  };

  QVERIFY(applyChordRemap(id, mask, table, "tiny11"));
  QCOMPARE(mask, KeyModifierAlt);
}

QTEST_MAIN(ChordRemapTests)
