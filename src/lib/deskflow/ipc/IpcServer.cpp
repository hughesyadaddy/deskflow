/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025-2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "IpcServer.h"

#include "base/Log.h"
#include "common/Constants.h"
#include "common/VersionInfo.h"

#include <QLocalServer>
#include <QLocalSocket>

#if defined(Q_OS_WIN)
#include <QCoreApplication>
#include <QDir>
#include <QTimer>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <tlhelp32.h>

#include <iterator>
#endif

namespace deskflow::core::ipc {

#if defined(Q_OS_WIN)
namespace {

//! Terminate every process named \p exeName whose image lives under
//! \p rootDir, in every session (never self). Returns how many were hit.
int terminateProcessesUnderRoot(const wchar_t *exeName, const QString &rootDir)
{
  int terminated = 0;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    LOG_WARN("[rescue] could not snapshot processes: %lu", GetLastError());
    return 0;
  }
  const QString root = QDir::cleanPath(rootDir).toLower() + QLatin1Char('/');
  const DWORD self = GetCurrentProcessId();
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
    if (entry.th32ProcessID == self || _wcsicmp(entry.szExeFile, exeName) != 0) {
      continue;
    }
    HANDLE process =
        OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID);
    if (process == nullptr) {
      continue;
    }
    wchar_t image[MAX_PATH * 2] = {};
    DWORD size = static_cast<DWORD>(std::size(image));
    if (QueryFullProcessImageNameW(process, 0, image, &size)) {
      const QString path = QDir::fromNativeSeparators(QString::fromWCharArray(image, static_cast<int>(size))).toLower();
      if (path.startsWith(root)) {
        LOG_INFO("[rescue] terminating %ls pid %lu", entry.szExeFile, entry.th32ProcessID);
        if (TerminateProcess(process, 0)) {
          WaitForSingleObject(process, 2000);
          ++terminated;
        }
      }
    }
    CloseHandle(process);
  }
  CloseHandle(snapshot);
  return terminated;
}

} // namespace
#endif

IpcServer::IpcServer(QObject *parent, const QString &serverName, const QString &typeName)
    : QObject(parent),
      m_server{new QLocalServer(this)}, // NOSONAR - Qt memory
      m_serverName(serverName),
      m_typeName(typeName.toUtf8())
{
  // do nothing
}

IpcServer::~IpcServer()
{
  m_server->close();
}

void IpcServer::listen()
{
  // IPC server normally runs as system, but GUI runs as regular user, so we need to allow world access.
  m_server->setSocketOptions(QLocalServer::WorldAccessOption);

  connect(m_server, &QLocalServer::newConnection, this, &IpcServer::handleNewConnection);
  QLocalServer::removeServer(m_serverName);
  if (m_server->listen(m_serverName)) {
    LOG_DEBUG("%s ipc server listening on: %s", m_typeName.constData(), m_serverName.toUtf8().constData());
  } else {
    LOG_ERR("%s ipc server failed to listen on: %s", m_typeName.constData(), m_serverName.toUtf8().constData());
  }
}

void IpcServer::handleNewConnection()
{
  QLocalSocket *clientSocket = m_server->nextPendingConnection();
  if (!clientSocket) {
    LOG_ERR("%s ipc server failed to get new connection", m_typeName.constData());
    return;
  }

  LOG_DEBUG("%s ipc server got new connection", m_typeName.constData());
  m_clients.insert(clientSocket);

  connect(clientSocket, &QLocalSocket::readyRead, this, &IpcServer::handleReadyRead);
  connect(clientSocket, &QLocalSocket::disconnected, this, &IpcServer::handleDisconnected);
  connect(clientSocket, &QLocalSocket::errorOccurred, this, &IpcServer::handleErrorOccurred);
}

void IpcServer::handleReadyRead()
{
  const auto clientSocket = qobject_cast<QLocalSocket *>(sender());
  LOG_VERBOSE("%s ipc server ready to read data", m_typeName.constData());

  QByteArray data = clientSocket->readAll();
  if (data.isEmpty()) {
    LOG_WARN("%s ipc server got empty message", m_typeName.constData());
    return;
  }

  // we don't handle incomplete messages yet; each socket read must have delimiters.
  if (!data.contains('\n')) {
    LOG_WARN("%s ipc server got incomplete message: %s", m_typeName.constData(), data.constData());
    return;
  }

  // each message is delimited by a newline to keep the protocol super simple.
  while (data.contains('\n')) {
    const auto index = data.indexOf('\n');
    QByteArray messageData = data.left(index);
    data.remove(0, index + 1);
    QString message = QString::fromUtf8(messageData);
    processMessage(clientSocket, message);
  }
}

void IpcServer::handleDisconnected()
{
  const auto clientSocket = qobject_cast<QLocalSocket *>(sender());
  LOG_DEBUG("%s ipc server client disconnected", m_typeName.constData());
  m_clients.remove(clientSocket);
  clientSocket->deleteLater();
}

void IpcServer::handleErrorOccurred()
{
  const auto clientSocket = qobject_cast<QLocalSocket *>(sender());
  LOG_ERR("%s ipc server client error: %s", m_typeName.constData(), clientSocket->errorString().toUtf8().constData());
  m_clients.remove(clientSocket);
  clientSocket->deleteLater();
}

void IpcServer::processMessage(QLocalSocket *clientSocket, const QString &message)
{
  LOG_VERBOSE("%s ipc server got message: %s", m_typeName.constData(), message.toUtf8().constData());
  const auto parts = message.split('=');
  if (parts.isEmpty()) {
    LOG_ERR("%s ipc server got invalid message: %s", m_typeName.constData(), message.toUtf8().constData());
    writeToClientSocket(clientSocket, QStringLiteral("error"));
    return;
  }

  if (const auto &command = parts.at(0); command == QStringLiteral("hello")) {
    if (parts.size() < 2) {
      LOG_ERR("%s ipc client hello missing version", m_typeName.constData());
      writeToClientSocket(clientSocket, "error=missing version");
      clientSocket->flush();
      clientSocket->disconnectFromServer();
      return;
    }

    const auto versionId = QStringLiteral("%1+%2").arg(kVersion, kVersionGitSha);
    const auto clientVersion = parts.at(1);
    LOG_DEBUG("%s ipc server got hello message (version: %s)", m_typeName.constData(), versionId.toUtf8().constData());

    if (clientVersion != versionId) {
      LOG_WARN(
          "%s ipc client version mismatch (client: %s, server: %s)", m_typeName.constData(),
          clientVersion.toUtf8().constData(), versionId.toUtf8().constData()
      );
      writeToClientSocket(clientSocket, QStringLiteral("versionMismatch=%1").arg(versionId));
      clientSocket->flush();
      return;
    }

    LOG_DEBUG("%s ipc server sending hello back", m_typeName.constData());
    writeToClientSocket(clientSocket, QStringLiteral("hello=%1").arg(versionId));

    // Replay messages that were queued before any clients connected.
    LOG_VERBOSE("ipc server replaying %d pending messages", m_pendingMessages.size());
    for (const auto &pending : std::as_const(m_pendingMessages)) {
      LOG_VERBOSE("%s ipc server replaying: %s", m_typeName.constData(), pending.toUtf8().constData());
      writeToClientSocket(clientSocket, pending);
    }
    m_pendingMessages.clear();
  } else if (command == QStringLiteral("noop")) {
    LOG_DEBUG("%s ipc server got noop message", m_typeName.constData());
    writeToClientSocket(clientSocket, QStringLiteral("ok"));
  } else {
    processCommand(clientSocket, command, parts);
  }

  clientSocket->flush();
}

bool IpcServer::hasClients() const
{
  return !m_clients.isEmpty();
}

void IpcServer::requestStopProcess()
{
  Q_EMIT stopProcessRequested();
}

void IpcServer::broadcastCommand(const QString &command, const QString &args)
{
  const auto message = args.isEmpty() ? command : QStringLiteral("%1=%2").arg(command, args);

  if (m_clients.isEmpty()) {
    LOG_VERBOSE(
        "%s ipc server has no clients, message queued: %s", m_typeName.constData(), message.toUtf8().constData()
    );
    m_pendingMessages.append(message);
    return;
  }

  LOG_VERBOSE(
      "%s ipc server broadcasting message to %d clients: %s", m_typeName.constData(), m_clients.size(),
      message.toUtf8().constData()
  );
  for (auto *client : std::as_const(m_clients)) {
    writeToClientSocket(client, message);
    client->flush();
  }
}

void IpcServer::broadcastCommandIfClients(const QString &command, const QString &args)
{
  if (m_clients.isEmpty()) {
    LOG_VERBOSE(
        "%s ipc server has no clients, dropping (no queue): %s", m_typeName.constData(), command.toUtf8().constData()
    );
    return;
  }
  broadcastCommand(command, args);
}

void IpcServer::requestLocalCoreRestart()
{
  // Mutually exclusive on the owning thread: never queue restartCore when
  // empty (stale fire later), and never both broadcast and stop.
  if (hasClients()) {
    LOG_INFO("keyboard rescue: 5x Esc — requesting core restart via GUI");
    broadcastCommandIfClients(QStringLiteral("restartCore"));
    return;
  }
#if defined(Q_OS_WIN)
  // No GUI, but on Windows the daemon watchdog owns this process: ask it to
  // relaunch us (DaemonIpcServer "restartCore" -> MSWindowsWatchdog::
  // requestRestart). The daemon does the stop AND the start, so unlike the
  // old stop-and-pray path there is always something left to respawn.
  // Short blocking waits are fine: this runs on the IPC owning thread and a
  // rescue is a once-in-a-blue-moon event.
  QLocalSocket daemon;
  daemon.connectToServer(QString::fromLatin1(kDaemonIpcName));
  if (daemon.waitForConnected(500)) {
    LOG_INFO("keyboard rescue: 5x Esc — no GUI IPC client; asking the daemon to relaunch the core");
    daemon.write("restartCore\n");
    daemon.flush();
    daemon.waitForBytesWritten(500);
    daemon.disconnectFromServer();
    return;
  }
  LOG_ERR(
      "keyboard rescue: 5x Esc — no GUI IPC client and the daemon is unreachable (%s); ignoring",
      daemon.errorString().toUtf8().constData()
  );
#else
  // NEVER stop the core here. Nothing guarantees a respawn, so with no GUI
  // running this would turn the rescue into a kill switch -- the machine
  // went dead and had to be restarted by hand. A rescue that cannot restart
  // must do nothing rather than destroy the thing it was meant to rescue.
  LOG_ERR(
      "keyboard rescue: 5x Esc — no GUI IPC client to restart the core; "
      "ignoring (stopping it here would leave this machine dead)"
  );
#endif
}

void IpcServer::requestLocalStopAll()
{
#if defined(Q_OS_WIN)
  // The daemon owns every core and the service: it terminates each
  // deskflow-core.exe / deskflow.exe under the install root in every session
  // and then stops itself cleanly (SERVICE_STOPPED with NO_ERROR, so the
  // SCM recovery actions `restart/1000/...` + failureflag 1 do NOT relaunch
  // it -- those fire only on a crash or a non-zero exit code).
  QLocalSocket daemon;
  daemon.connectToServer(QString::fromLatin1(kDaemonIpcName));
  if (daemon.waitForConnected(500)) {
    LOG_INFO("[rescue] asking the daemon to stop every Deskflow process and the service");
    daemon.write("stopAll\n");
    daemon.flush();
    daemon.waitForBytesWritten(500);
    daemon.disconnectFromServer();
    // The daemon terminates us. If it has not within the grace period
    // (broken service), still take the GUI down and leave ourselves.
    QTimer::singleShot(5000, this, [this] {
      LOG_WARN("[rescue] still alive 5 s after asking the daemon; stopping the GUI and this core directly");
      terminateProcessesUnderRoot(L"deskflow.exe", QCoreApplication::applicationDirPath());
      requestStopProcess();
    });
    return;
  }
  LOG_WARN(
      "[rescue] daemon unreachable (%s); stopping the GUI and this core directly",
      daemon.errorString().toUtf8().constData()
  );
  // GUI first: in Desktop process mode it would otherwise relaunch the core.
  terminateProcessesUnderRoot(L"deskflow.exe", QCoreApplication::applicationDirPath());
  requestStopProcess();
#else
  // macOS runs the launchd sequence in coordination/OSXRescueStopAll.mm;
  // nothing here owns a service on other platforms.
  LOG_WARN("[rescue] stop-all via IPC is Windows-only; stopping this core");
  requestStopProcess();
#endif
}

void IpcServer::writeToClientSocket(QLocalSocket *&clientSocket, const QString &message) const
{
  QByteArray messageData = message.toUtf8() + '\n';
  qint64 bytesWritten = clientSocket->write(messageData);
  if (bytesWritten != messageData.size()) {
    LOG_ERR("%s ipc server failed to write full message to client socket", m_typeName.constData());
  } else {
    LOG_VERBOSE(
        "%s ipc server wrote message to client socket: %s", m_typeName.constData(), message.toUtf8().constData()
    );
  }
}

} // namespace deskflow::core::ipc
