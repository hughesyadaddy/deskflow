/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ChordRemapConfTests.h"

#include "common/Settings.h"
#include "gui/ChordRemap.h"
#include "gui/config/Screen.h"
#include "gui/config/ServerConfig.h"
#include "server/ChordRemapTypes.h"
#include "server/Config.h"

#include <QDir>
#include <QFile>
#include <QTextStream>

#include <sstream>

using namespace deskflow::server;

void ChordRemapConfTests::initTestCase()
{
  QDir dir;
  QVERIFY(dir.mkpath(m_settingsPath));

  QFile oldSettings(m_settingsFile);
  if (oldSettings.exists()) {
    oldSettings.remove();
  }

  Settings::setSettingsFile(m_settingsFile);
  Settings::setStateFile(m_stateFile);
  Settings::setValue(Settings::Core::ComputerName, QStringLiteral("hackintosh"));
  Settings::setValue(Settings::Server::GridWidth, 3);
  Settings::setValue(Settings::Server::GridHeight, 3);
}

void ChordRemapConfTests::guiConfRoundTrip_defaultTiny11Rows()
{
  ServerConfig guiConfig(3, 3);
  Screen hackintosh(QStringLiteral("hackintosh"));
  hackintosh.markAsServer();
  guiConfig.screens()[4] = hackintosh;
  guiConfig.screens()[5] = Screen(QString::fromUtf8(kDefaultChordRemapScreen));

  for (const auto &entry : kDefaultTiny11ChordRemaps) {
    guiConfig.chordRemaps().append(ChordRemap::fromServerEntry(entry));
  }

  QString confText;
  QTextStream out(&confText);
  out << guiConfig;

  std::istringstream in(confText.toStdString());
  ConfigReadContext context(in);
  Config coreConfig(nullptr);
  coreConfig.read(context);

  QCOMPARE(coreConfig.getChordRemaps().size(), kDefaultTiny11ChordRemaps.size());
  for (std::size_t i = 0; i < kDefaultTiny11ChordRemaps.size(); ++i) {
    const auto &expected = kDefaultTiny11ChordRemaps[i];
    const auto &actual = coreConfig.getChordRemaps()[i];
    QCOMPARE(actual.screen, expected.screen);
    QCOMPARE(actual.inMods, expected.inMods);
    QCOMPARE(actual.inKey, expected.inKey);
    QCOMPARE(actual.outMods, expected.outMods);
    QCOMPARE(actual.outKey, expected.outKey);
  }
}

QTEST_MAIN(ChordRemapConfTests)
