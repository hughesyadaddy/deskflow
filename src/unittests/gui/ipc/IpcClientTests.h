/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QTest>

class IpcClientTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void forever_client_backs_off_and_never_gives_up();
  void limited_client_fails_after_max_attempts();
  void disconnect_cancels_pending_retry();
  void forever_client_attaches_once_server_appears();
};
