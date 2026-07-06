/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QObject>

class PreConnectHostsTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void fleet_serverLanFirstThenStable();
  void fleet_excludesSelfCaseInsensitive();
  void fleet_dedupesWhenLanEqualsIp();
  void fleet_emptyFleetYieldsServerAddressOnly();
  void defaults_serverAddressFirstThenPeers();
  void defaults_emptyServerAddress();
};
