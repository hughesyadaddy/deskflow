/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QTest>

class IpcServerLocalRestartTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();
  void emptyClients_stopsWithoutQueuingRestart();
  void withClient_broadcastsRestartWithoutStop();
  void broadcastCommandIfClients_emptyDropsWithoutQueue();
};
