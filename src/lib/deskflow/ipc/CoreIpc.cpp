/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "CoreIpc.h"

#include "CoreIpcServer.h"
#include "base/Log.h"

#include <QMetaEnum>

void ipcSendToClient(const QString &command, const QString &args)
{
  // Queued because callers may not be on the main thread,
  // and QLocalSocket can only be written to from its owning thread.
  auto &server = deskflow::core::ipc::CoreIpcServer::instance();
  QMetaObject::invokeMethod(
      &server, [command, args] { deskflow::core::ipc::CoreIpcServer::instance().broadcastCommand(command, args); },
      Qt::QueuedConnection
  );
}

void ipcSendConnectionState(deskflow::core::ConnectionState state)
{
  const auto metaEnum = QMetaEnum::fromType<deskflow::core::ConnectionState>();
  ipcSendToClient(QStringLiteral("connectionState"), metaEnum.valueToKey(static_cast<int>(state)));
}

void ipcRequestLocalCoreRestart()
{
  auto &server = deskflow::core::ipc::CoreIpcServer::instance();
  // Mutually exclusive: never queue restartCore when empty (stale fire later),
  // and never both broadcast and stop.
  if (server.hasClients()) {
    LOG_INFO("keyboard rescue: 5x Esc — requesting core restart via GUI");
    ipcSendToClient(QStringLiteral("restartCore"));
    return;
  }
  LOG_WARN("keyboard rescue: 5x Esc — no GUI IPC client; stopping core for respawn");
  QMetaObject::invokeMethod(
      &server, [] { deskflow::core::ipc::CoreIpcServer::instance().requestStopProcess(); }, Qt::QueuedConnection
  );
}
