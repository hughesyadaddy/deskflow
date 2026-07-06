/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QObject>

class WakeOnLanTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();
  void parseMac_colonSeparated();
  void parseMac_dashSeparated();
  void parseMac_rejectsMalformed();
  void magicPacket_layout();
  void sendWakeOnLan_rejectsInvalidMac();
};
