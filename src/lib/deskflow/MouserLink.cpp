/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/MouserLink.h"

#include "base/Log.h"
#include "base/ThreadJoin.h"
#include "common/Settings.h"
#include "common/VersionInfo.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QUuid>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketLen = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketLen = socklen_t;
#endif

#include <cerrno>
#include <cstring>

namespace deskflow {

namespace {

constexpr int kProto = 2;
constexpr size_t kMaxLineBytes = 64 * 1024;
constexpr size_t kMaxQueued = 256;
constexpr auto kIdlePoll = std::chrono::milliseconds(1000);

// Link-thread-private: set by helloLego() when the peer on the lego port turned
// out to be an OLD Mouser (its RemoteDeviceServer answers a proto-2 hello with
// {"ok":false,"error":"unauthorized"} and no "reason"), consumed by runLego().
// Without this the hello classified as Failed and backed off forever, never
// falling back to the v1 connector even though the token file existed.
thread_local bool t_helloRejectedByOldMouser = false;
thread_local bool t_oldMouserLogged = false;

void platformClose(int fd)
{
#if defined(_WIN32)
  ::closesocket(fd);
#else
  ::close(fd);
#endif
}

void platformShutdown(int fd)
{
#if defined(_WIN32)
  ::shutdown(fd, SD_BOTH);
#else
  ::shutdown(fd, SHUT_RDWR);
#endif
}

bool lastErrorIsTimeout()
{
#if defined(_WIN32)
  const int error = WSAGetLastError();
  return error == WSAETIMEDOUT || error == WSAEWOULDBLOCK;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

void setSocketTimeout(int fd, int option, std::chrono::milliseconds duration)
{
#if defined(_WIN32)
  const DWORD ms = static_cast<DWORD>(duration.count());
  ::setsockopt(fd, SOL_SOCKET, option, reinterpret_cast<const char *>(&ms), sizeof(ms));
#else
  timeval timeout{};
  timeout.tv_sec = static_cast<long>(duration.count() / 1000);
  timeout.tv_usec = static_cast<int>((duration.count() % 1000) * 1000);
  ::setsockopt(fd, SOL_SOCKET, option, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
#endif
}

void setBlocking(int fd, bool blocking)
{
#if defined(_WIN32)
  u_long nonBlocking = blocking ? 0 : 1;
  ::ioctlsocket(fd, FIONBIO, &nonBlocking);
#else
  const int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
#endif
}

bool waitReadable(int fd, std::chrono::milliseconds duration)
{
  fd_set readSet;
  FD_ZERO(&readSet);
  FD_SET(fd, &readSet);
  timeval timeout{};
  timeout.tv_sec = static_cast<long>(duration.count() / 1000);
  timeout.tv_usec = static_cast<int>((duration.count() % 1000) * 1000);
  return ::select(fd + 1, &readSet, nullptr, nullptr, &timeout) == 1;
}

// Constant-time-ish comparison (legacy listener token check).
bool tokenMatches(const std::string &expected, const std::string &provided)
{
  if (expected.empty()) {
    return false;
  }
  unsigned char diff = expected.size() == provided.size() ? 0 : 1;
  for (size_t i = 0; i < expected.size(); ++i) {
    const char p = i < provided.size() ? provided[i] : 0;
    diff |= static_cast<unsigned char>(expected[i] ^ p);
  }
  return diff == 0;
}

QJsonObject parseObject(const std::string &line)
{
  const auto doc = QJsonDocument::fromJson(QByteArray(line.data(), static_cast<int>(line.size())));
  return doc.isObject() ? doc.object() : QJsonObject();
}

std::string serialize(const QJsonObject &object)
{
  return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}

std::string typed(const char *type)
{
  QJsonObject object;
  object[QStringLiteral("t")] = QString::fromLatin1(type);
  return serialize(object);
}

std::string focusLineLego(const std::string &screen, bool here)
{
  QJsonObject object;
  object[QStringLiteral("t")] = QStringLiteral("focus");
  object[QStringLiteral("screen")] = QString::fromStdString(screen);
  object[QStringLiteral("here")] = here;
  return serialize(object);
}

std::string focusLineLegacy(const std::string &screen, bool local)
{
  QJsonObject object;
  object[QStringLiteral("type")] = QStringLiteral("focus");
  object[QStringLiteral("screen")] = QString::fromStdString(screen);
  object[QStringLiteral("local")] = local;
  return serialize(object);
}

std::string roleLine(MouserLink::Role role)
{
  QJsonObject object;
  object[QStringLiteral("t")] = QStringLiteral("role");
  object[QStringLiteral("role")] = QString::fromLatin1(MouserLink::roleName(role));
  return serialize(object);
}

} // namespace

// ---------------------------------------------------------------------------
// construction / singleton

MouserLink::MouserLink(Options options)
    : m_options(std::move(options)),
      m_sessionId(QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString()),
      m_backoff(m_options.initialBackoff)
{
  if (m_options.maxBackoff < m_options.initialBackoff) {
    m_options.maxBackoff = m_options.initialBackoff;
  }
}

MouserLink::~MouserLink()
{
  stop("shutdown");
}

MouserLink &MouserLink::shared()
{
  static MouserLink link(optionsFromSettings());
  link.start();
  return link;
}

MouserLink::Options MouserLink::optionsFromSettings()
{
  Options options;
  options.appVersion = kVersion;
  if (qEnvironmentVariable("DESKFLOW_MOUSER_LINK") == QStringLiteral("off")) {
    // Unit tests (and debugging) that build a Server must not dial the
    // developer's real Mouser: no token file, no legacy fallback.
    options.tokenFile = "/nonexistent/mouser-link-off";
    options.legacyEnabled = false;
    return options;
  }
  // The lego port is Mouser's, not ours; the client-side setting kept the
  // same default (19795) so honour it as an override for odd setups.
  const int port = Settings::value(Settings::Client::MouserPort).toInt();
  if (port > 0) {
    options.port = port;
  }
  options.legacyServerEnabled = Settings::value(Settings::Server::MouserBridgeEnabled).toBool();
  options.legacyServerPort = Settings::value(Settings::Server::MouserBridgePort).toInt();
  options.legacyServerToken = Settings::value(Settings::Server::MouserBridgeToken).toString().toStdString();
  options.legacyClientEnabled = Settings::value(Settings::Client::MouserEnabled).toBool();
  options.legacyClientPort = Settings::value(Settings::Client::MouserPort).toInt();
  options.legacyClientToken = Settings::value(Settings::Client::MouserToken).toString().toStdString();
  return options;
}

std::string MouserLink::defaultTokenFile()
{
  if (const auto env = qEnvironmentVariable("DESKFLOW_MOUSER_TOKEN_FILE"); !env.isEmpty()) {
    return env.toStdString();
  }
#if defined(Q_OS_WIN)
  // Mouser writes %APPDATA%\Mouser\bridge.token (roaming). Qt's
  // GenericDataLocation is %LOCALAPPDATA%, so the old lookup never found it
  // and every Windows seat silently ran the legacy path.
  const auto base = qEnvironmentVariable("APPDATA");
#else
  const auto base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
#endif
  if (base.isEmpty()) {
    return {};
  }
  return QDir(base).filePath(QStringLiteral("Mouser/bridge.token")).toStdString();
}

const char *MouserLink::roleName(Role role)
{
  switch (role) {
  case Role::Server:
    return "server";
  case Role::Client:
    return "client";
  case Role::None:
    break;
  }
  return "none";
}

const char *MouserLink::modeName(Mode mode)
{
  switch (mode) {
  case Mode::Lego:
    return "lego";
  case Mode::Legacy:
    return "legacy";
  case Mode::Off:
    break;
  }
  return "off";
}

// ---------------------------------------------------------------------------
// lifecycle

void MouserLink::start()
{
  if (m_started.exchange(true)) {
    return;
  }
  m_running = true;
  m_writer = std::thread([this] { writerLoop(); });
  m_thread = std::thread([this] { linkLoop(); });
}

void MouserLink::stop(std::string_view reason)
{
  if (!m_started) {
    return;
  }
  if (!m_running.exchange(false)) {
    return;
  }

  // Writer first: the farewell below bypasses the queue and must be the
  // last (and an un-torn) line on the wire.
  m_queueCv.notify_all();
  joinNoThrow(m_writer);
  {
    std::scoped_lock lock{m_fdMutex};
    if (m_fd >= 0 && m_mode == Mode::Lego) {
      QJsonObject bye;
      bye[QStringLiteral("t")] = QStringLiteral("bye");
      bye[QStringLiteral("reason")] = QString::fromUtf8(reason.data(), static_cast<int>(reason.size()));
      sendNow(m_fd, serialize(bye) + "\n");
    }
    if (m_fd >= 0) {
      platformShutdown(m_fd);
    }
  }
  if (const int listenFd = m_listenFd.exchange(-1); listenFd >= 0) {
    platformShutdown(listenFd);
    platformClose(listenFd);
  }

  ++m_wakeEpoch;
  m_sleepCv.notify_all();
  joinNoThrow(m_thread);
  dropFd();
}

// ---------------------------------------------------------------------------
// public API

void MouserLink::setRole(Role role)
{
  {
    std::scoped_lock lock{m_stateMutex};
    if (m_role == role) {
      return;
    }
    m_role = role;
  }
  if (m_mode == Mode::Lego) {
    enqueueJson(roleLine(role));
  }
  // The legacy backend is role-specific: wake the loop so it re-evaluates.
  ++m_wakeEpoch;
  m_sleepCv.notify_all();
}

MouserLink::Role MouserLink::role() const
{
  std::scoped_lock lock{m_stateMutex};
  return m_role;
}

void MouserLink::notifyFocus(const std::string &screen, bool here)
{
  Role role;
  {
    std::scoped_lock lock{m_stateMutex};
    m_focusScreen = screen;
    m_focusHere = here;
    m_focusKnown = true;
    role = m_role;
  }
  switch (m_mode.load()) {
  case Mode::Lego:
    enqueueJson(focusLineLego(screen, here));
    break;
  case Mode::Legacy:
    if (role == Role::Server) {
      enqueueJson(focusLineLegacy(screen, here));
    }
    break;
  case Mode::Off:
    break;
  }
}

void MouserLink::deliver(const std::string &line)
{
  const QJsonObject object = parseObject(line);
  const QString type = object[QStringLiteral("type")].toString();
  if (type == QStringLiteral("connect")) {
    std::scoped_lock lock{m_stateMutex};
    m_lastConnectLine = line;
  } else if (type == QStringLiteral("disconnect")) {
    std::scoped_lock lock{m_stateMutex};
    m_lastConnectLine.clear();
  }

  if (m_mode == Mode::Legacy) {
    deliverLegacy(line);
    return;
  }
  enqueueJson(line);
}

void MouserLink::deliverReport(const std::string &frame)
{
  enqueue(frame);
}

void MouserLink::setInboundHandler(InboundHandler handler)
{
  std::scoped_lock lock{m_stateMutex};
  m_inbound = std::move(handler);
}

bool MouserLink::connected() const
{
  return m_connected;
}

MouserLink::Mode MouserLink::mode() const
{
  return m_mode;
}

std::string MouserLink::sessionId() const
{
  return m_sessionId;
}

// ---------------------------------------------------------------------------
// link thread

void MouserLink::linkLoop()
{
  while (m_running) {
    const std::string token = readTokenFile();
    if (!token.empty()) {
      if (m_lastLoggedMode != Mode::Lego) {
        LOG_INFO(
            "mouser link: lego mode (token file %s, mouser listener 127.0.0.1:%d)", tokenFilePath().c_str(),
            m_options.port
        );
        m_lastLoggedMode = Mode::Lego;
      }
      runLego(token);
      continue;
    }

    if (m_options.legacyEnabled) {
      if (m_lastLoggedMode != Mode::Legacy) {
        LOG_INFO(
            "mouser link: legacy mode, no token file at %s (un-upgraded mouser; 19796 listener / v1 connector)",
            tokenFilePath().c_str()
        );
        m_lastLoggedMode = Mode::Legacy;
      }
      runLegacy();
      continue;
    }

    if (m_lastLoggedMode != Mode::Off) {
      LOG_INFO("mouser link: off (no token file at %s)", tokenFilePath().c_str());
      m_lastLoggedMode = Mode::Off;
    }
    m_mode = Mode::Off;
    sleepInterruptible(kIdlePoll);
  }
}

bool MouserLink::runLego(const std::string &token)
{
  m_mode = Mode::Lego;
  const int fd = connectLoopback(m_options.port);
  if (fd < 0) {
    if (!m_absentLogged) {
      LOG_INFO("mouser link: no mouser listening on 127.0.0.1:%d; retrying with backoff", m_options.port);
      m_absentLogged = true;
      ++m_stats.absentLogs;
    } else {
      LOG_DEBUG("mouser link: connect to 127.0.0.1:%d failed", m_options.port);
    }
    sleepInterruptible(m_backoff);
    escalateBackoff();
    return false;
  }

  switch (helloLego(fd, token)) {
  case Hello::Accepted:
    break;
  case Hello::ProtoMismatch:
    platformClose(fd);
    ++m_stats.protoMismatches;
    if (!m_protoLogged) {
      LOG_WARN(
          "mouser link: mouser rejected proto %d; sleeping %lld ms between retries", kProto,
          static_cast<long long>(m_options.protoMismatchSleep.count())
      );
      m_protoLogged = true;
      ++m_stats.protoMismatchLogs;
    }
    sleepInterruptible(m_options.protoMismatchSleep);
    return false;
  case Hello::AuthRejected:
    // Mouser rotated its token (or we read a stale file): the next attempt
    // re-reads bridge.token from scratch.
    platformClose(fd);
    ++m_stats.authRejections;
    LOG_WARN(
        "mouser link: mouser rejected token from %s; re-reading it in %lld ms", tokenFilePath().c_str(),
        static_cast<long long>(m_options.authRetry.count())
    );
    sleepInterruptible(m_options.authRetry);
    return false;
  case Hello::Failed:
    platformClose(fd);
    if (t_helloRejectedByOldMouser) {
      t_helloRejectedByOldMouser = false;
      // Token file present but the listener is an old Mouser: use the v1
      // connector for this cycle instead of backing off forever on the lego
      // hello. The next loop iteration retries lego (Mouser may be upgraded).
      const bool canFallBack = m_options.legacyEnabled && role() == Role::Client && m_options.legacyClientEnabled &&
                               !m_options.legacyClientToken.empty();
      if (canFallBack) {
        if (!t_oldMouserLogged) {
          LOG_INFO(
              "mouser link: mouser on 127.0.0.1:%d is pre-lego (unauthorized, no reason); using legacy v1 connector",
              m_options.port
          );
          t_oldMouserLogged = true;
        }
        m_mode = Mode::Legacy;
        runLegacyConnector(m_options.legacyClientPort, m_options.legacyClientToken);
        return false;
      }
      LOG_WARN("mouser link: mouser on 127.0.0.1:%d is pre-lego and no legacy connector is configured", m_options.port);
    }
    sleepInterruptible(m_backoff);
    escalateBackoff();
    return false;
  }

  m_absentLogged = false;
  m_protoLogged = false;
  t_oldMouserLogged = false;
  resetBackoff();
  ++m_stats.connects;
  ++m_stats.hellosAccepted;
  publishFd(fd, Mode::Lego);
  LOG_INFO("mouser link: connected to mouser (proto %d, session %s)", kProto, m_sessionId.c_str());
  announceSession();

  bool byeReceived = false;
  readLoopLego(fd, byeReceived);
  dropFd();
  if (!m_running) {
    return true;
  }
  if (byeReceived) {
    // Mouser said goodbye on purpose (restart/upgrade): it will be back
    // shortly, so reconnect at once and keep the backoff untouched.
    LOG_INFO("mouser link: mouser said bye; reconnecting");
    resetBackoff();
    return true;
  }
  LOG_INFO("mouser link: session dropped; reconnecting in %lld ms", static_cast<long long>(m_backoff.count()));
  sleepInterruptible(m_backoff);
  return true;
}

int MouserLink::connectLoopback(int port) const
{
  const int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
  if (fd < 0) {
    return -1;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  // Bounded connect: a wedged peer with a full accept queue must not hang
  // the link thread.
  setBlocking(fd, false);
  const auto rc = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
  bool connected = (rc == 0);
  if (!connected) {
    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(fd, &writeSet);
    timeval timeout{};
    timeout.tv_sec = static_cast<long>(m_options.connectTimeout.count() / 1000);
    timeout.tv_usec = static_cast<int>((m_options.connectTimeout.count() % 1000) * 1000);
    if (::select(fd + 1, nullptr, &writeSet, nullptr, &timeout) == 1) {
      int error = 0;
      SocketLen errorLen = sizeof(error);
      ::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &errorLen);
      connected = (error == 0);
    }
  }
  if (!connected) {
    platformClose(fd);
    return -1;
  }
  setBlocking(fd, true);
  const int noDelay = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&noDelay), sizeof(noDelay));
  setSocketTimeout(fd, SO_SNDTIMEO, std::chrono::milliseconds(5000));
  return fd;
}

MouserLink::Hello MouserLink::helloLego(int fd, const std::string &token)
{
  setSocketTimeout(fd, SO_RCVTIMEO, m_options.helloTimeout);

  QJsonObject hello;
  hello[QStringLiteral("t")] = QStringLiteral("hello");
  hello[QStringLiteral("proto")] = kProto;
  hello[QStringLiteral("app")] = QString::fromStdString(m_options.appName);
  hello[QStringLiteral("ver")] = QString::fromStdString(m_options.appVersion);
  hello[QStringLiteral("pid")] = static_cast<qint64>(QCoreApplication::applicationPid());
  hello[QStringLiteral("role")] = QString::fromLatin1(roleName(role()));
  hello[QStringLiteral("caps")] = QJsonArray{QStringLiteral("focus"), QStringLiteral("hidr"), QStringLiteral("decode")};
  hello[QStringLiteral("token")] = QString::fromStdString(token);
  if (!sendNow(fd, serialize(hello) + "\n")) {
    return Hello::Failed;
  }

  std::string carry;
  std::string reply;
  bool timedOut = false;
  if (!readLine(fd, carry, reply, timedOut)) {
    LOG_DEBUG("mouser link: no hello reply (%s)", timedOut ? "timeout" : "closed");
    return Hello::Failed;
  }
  const QJsonObject object = parseObject(reply);
  t_helloRejectedByOldMouser = false;
  if (!object[QStringLiteral("ok")].toBool()) {
    const QString reason = object[QStringLiteral("reason")].toString();
    if (reason == QStringLiteral("proto")) {
      return Hello::ProtoMismatch;
    }
    if (reason == QStringLiteral("auth")) {
      return Hello::AuthRejected;
    }
    // Pre-lego Mouser: its RemoteDeviceServer (v1 protocol) sits on the same
    // port and rejects our proto-2 hello as {"ok":false,"error":"unauthorized"}
    // with no "reason" key. runLego() runs the v1 connector for this cycle.
    if (!object.contains(QStringLiteral("reason")) &&
        object[QStringLiteral("error")].toString() == QStringLiteral("unauthorized")) {
      t_helloRejectedByOldMouser = true;
      return Hello::Failed;
    }
    LOG_WARN("mouser link: hello rejected: %.128s", reply.c_str());
    return Hello::Failed;
  }
  if (object.contains(QStringLiteral("proto")) && object[QStringLiteral("proto")].toInt() != kProto) {
    return Hello::ProtoMismatch;
  }
  setSocketTimeout(fd, SO_RCVTIMEO, m_options.pingInterval);
  return Hello::Accepted;
}

void MouserLink::announceSession()
{
  QJsonObject attach;
  attach[QStringLiteral("t")] = QStringLiteral("attach");
  attach[QStringLiteral("session_id")] = QString::fromStdString(m_sessionId);
  enqueueJson(serialize(attach));
  ++m_stats.attachesSent;

  Role role;
  std::string focusScreen;
  bool focusHere = true;
  bool focusKnown = false;
  std::string connectLine;
  {
    std::scoped_lock lock{m_stateMutex};
    role = m_role;
    focusScreen = m_focusScreen;
    focusHere = m_focusHere;
    focusKnown = m_focusKnown;
    connectLine = m_lastConnectLine;
  }
  if (role != Role::None) {
    enqueueJson(roleLine(role));
  }
  if (focusKnown) {
    enqueueJson(focusLineLego(focusScreen, focusHere));
  }
  if (role == Role::Client && !connectLine.empty()) {
    enqueueJson(connectLine);
  }
}

void MouserLink::readLoopLego(int fd, bool &byeReceived)
{
  std::string carry;
  std::string line;
  auto lastPing = std::chrono::steady_clock::now();
  bool inboundSincePing = true;
  int unanswered = 0;

  while (m_running) {
    bool timedOut = false;
    const bool gotLine = readLine(fd, carry, line, timedOut);
    if (!gotLine && !timedOut) {
      return; // peer closed or error
    }
    if (gotLine) {
      inboundSincePing = true;
      if (!line.empty()) {
        handleLegoInbound(line, byeReceived);
        if (byeReceived) {
          return;
        }
      }
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - lastPing >= m_options.pingInterval) {
      if (inboundSincePing) {
        unanswered = 0;
      } else if (++unanswered >= m_options.pingMisses) {
        ++m_stats.pingDrops;
        LOG_WARN("mouser link: %d pings unanswered; dropping session", unanswered);
        return;
      }
      enqueueJson(typed("ping"));
      lastPing = now;
      inboundSincePing = false;
    }
  }
}

void MouserLink::handleLegoInbound(const std::string &line, bool &byeReceived)
{
  const QJsonObject object = parseObject(line);
  const QString type = object[QStringLiteral("t")].toString();
  if (type == QStringLiteral("ping")) {
    enqueueJson(typed("pong"));
    return;
  }
  if (type == QStringLiteral("pong")) {
    return;
  }
  if (type == QStringLiteral("bye")) {
    ++m_stats.byesReceived;
    byeReceived = true;
    return;
  }
  if (type.isEmpty() && object.contains(QStringLiteral("ok"))) {
    return; // stray ack
  }
  dispatchInbound(line);
}

// ---------------------------------------------------------------------------
// legacy backend (token file absent)

void MouserLink::runLegacy()
{
  m_mode = Mode::Legacy;
  const Role current = role();
  if (current == Role::Server && m_options.legacyServerEnabled && !m_options.legacyServerToken.empty()) {
    runLegacyListener(m_options.legacyServerPort, m_options.legacyServerToken);
    return;
  }
  if (current == Role::Client && m_options.legacyClientEnabled && !m_options.legacyClientToken.empty()) {
    runLegacyConnector(m_options.legacyClientPort, m_options.legacyClientToken);
    return;
  }
  sleepInterruptible(kIdlePoll);
}

void MouserLink::runLegacyListener(int port, const std::string &token)
{
  const int listenFd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
  if (listenFd < 0) {
    LOG_WARN("mouser link: legacy socket() failed: %s", std::strerror(errno));
    sleepInterruptible(m_backoff);
    escalateBackoff();
    return;
  }
  const int reuse = 1;
  ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(listenFd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || ::listen(listenFd, 1) != 0) {
    LOG_WARN("mouser link: legacy bind 127.0.0.1:%d failed: %s", port, std::strerror(errno));
    platformClose(listenFd);
    sleepInterruptible(m_backoff);
    escalateBackoff();
    return;
  }
  resetBackoff();
  m_listenFd = listenFd;
  LOG_INFO("mouser link: legacy listener on 127.0.0.1:%d", port);

  while (m_running && role() == Role::Server && readTokenFile().empty()) {
    if (!waitReadable(listenFd, kIdlePoll)) {
      continue;
    }
    sockaddr_in peer{};
    SocketLen peerLen = sizeof(peer);
    const int clientFd = static_cast<int>(::accept(listenFd, reinterpret_cast<sockaddr *>(&peer), &peerLen));
    if (clientFd < 0) {
      break; // listener closed by stop()
    }
    std::string carry;
    if (!legacyAuthenticate(clientFd, token, carry)) {
      platformClose(clientFd);
      continue;
    }
    setSocketTimeout(clientFd, SO_SNDTIMEO, std::chrono::milliseconds(5000));
    ++m_stats.connects;
    publishFd(clientFd, Mode::Legacy);
    LOG_INFO("mouser link: legacy mouser connected");
    {
      std::scoped_lock lock{m_stateMutex};
      if (m_focusKnown) {
        enqueue(focusLineLegacy(m_focusScreen, m_focusHere) + "\n");
      }
    }
    readLoopLegacy(clientFd, true);
    dropFd();
    LOG_INFO("mouser link: legacy mouser disconnected");
  }

  if (const int fd = m_listenFd.exchange(-1); fd >= 0) {
    platformClose(fd);
  }
}

bool MouserLink::legacyAuthenticate(int fd, const std::string &token, std::string &carry)
{
  setSocketTimeout(fd, SO_RCVTIMEO, m_options.helloTimeout);
  std::string line;
  bool timedOut = false;
  if (!readLine(fd, carry, line, timedOut)) {
    return false;
  }
  const QJsonObject hello = parseObject(line);
  const std::string provided = hello[QStringLiteral("token")].toString().toStdString();
  if (hello[QStringLiteral("type")].toString() != QStringLiteral("hello") || !tokenMatches(token, provided)) {
    LOG_WARN("mouser link: legacy listener rejected unauthenticated connection");
    sendNow(fd, R"({"ok": false, "error": "unauthorized"})" "\n");
    return false;
  }
  return sendNow(fd, R"({"ok": true, "server": "deskflow-bridge", "version": 1})" "\n");
}

void MouserLink::runLegacyConnector(int port, const std::string &token)
{
  const int fd = connectLoopback(port);
  if (fd < 0) {
    if (!m_absentLogged) {
      LOG_INFO("mouser link: no legacy mouser remote-device port on 127.0.0.1:%d; retrying with backoff", port);
      m_absentLogged = true;
      ++m_stats.absentLogs;
    }
    sleepInterruptible(m_backoff);
    escalateBackoff();
    return;
  }
  setSocketTimeout(fd, SO_RCVTIMEO, m_options.helloTimeout);
  QJsonObject hello;
  hello[QStringLiteral("type")] = QStringLiteral("hello");
  hello[QStringLiteral("token")] = QString::fromStdString(token);
  hello[QStringLiteral("version")] = 1;
  std::string carry;
  std::string reply;
  bool timedOut = false;
  if (!sendNow(fd, serialize(hello) + "\n") || !readLine(fd, carry, reply, timedOut) ||
      !parseObject(reply)[QStringLiteral("ok")].toBool()) {
    LOG_WARN("mouser link: legacy mouser rejected hello: %.128s", reply.c_str());
    platformClose(fd);
    sleepInterruptible(m_backoff);
    escalateBackoff();
    return;
  }
  m_absentLogged = false;
  resetBackoff();
  ++m_stats.connects;
  publishFd(fd, Mode::Legacy);
  LOG_INFO("mouser link: connected to legacy mouser on 127.0.0.1:%d", port);
  {
    std::scoped_lock lock{m_stateMutex};
    if (!m_lastConnectLine.empty()) {
      enqueue(m_lastConnectLine + "\n");
    }
  }
  readLoopLegacy(fd, false);
  dropFd();
  LOG_INFO("mouser link: legacy mouser session ended");
  sleepInterruptible(m_backoff);
}

void MouserLink::readLoopLegacy(int fd, bool dispatch)
{
  setSocketTimeout(fd, SO_RCVTIMEO, kIdlePoll);
  std::string carry;
  std::string line;
  while (m_running) {
    bool timedOut = false;
    if (!readLine(fd, carry, line, timedOut)) {
      if (!timedOut) {
        break;
      }
      // Legacy backends are role-bound and only a stopgap: leave when the
      // role moved on or Mouser got upgraded (token file appeared).
      const Role current = role();
      if ((dispatch && current != Role::Server) || (!dispatch && current != Role::Client)) {
        break;
      }
      if (!readTokenFile().empty()) {
        break;
      }
      continue;
    }
    if (dispatch && !line.empty()) {
      dispatchInbound(line);
    }
  }
  if (dispatch && m_running) {
    // The legacy Mouser vanished: the remote virtual device must not
    // linger (same path as an explicit disconnect line).
    dispatchInbound(R"({"type": "disconnect"})");
  }
}

void MouserLink::deliverLegacy(const std::string &line)
{
  // The relay vocabulary stays v1 on the wire; only the new focus notices
  // need translating into the connect/disconnect the old Mouser expects.
  const QJsonObject object = parseObject(line);
  const QString t = object[QStringLiteral("t")].toString();
  if (t.isEmpty()) {
    const QString type = object[QStringLiteral("type")].toString();
    if (type == QStringLiteral("connect")) {
      m_legacyAttached = true;
    } else if (type == QStringLiteral("disconnect")) {
      m_legacyAttached = false;
    }
    enqueueJson(line);
    return;
  }
  if (t != QStringLiteral("focus")) {
    return; // role/attach/etc. have no legacy equivalent
  }
  if (object[QStringLiteral("here")].toBool()) {
    std::string connectLine;
    {
      std::scoped_lock lock{m_stateMutex};
      connectLine = m_lastConnectLine;
    }
    if (!m_legacyAttached && !connectLine.empty()) {
      m_legacyAttached = true;
      enqueueJson(connectLine);
    }
  } else if (m_legacyAttached) {
    m_legacyAttached = false;
    enqueueJson(R"({"type": "disconnect"})");
  }
}

// ---------------------------------------------------------------------------
// plumbing

void MouserLink::publishFd(int fd, Mode mode)
{
  {
    std::scoped_lock lock{m_queueMutex};
    m_queue.clear();
  }
  std::scoped_lock lock{m_fdMutex};
  m_fd = fd;
  m_mode = mode;
  m_connected = true;
}

void MouserLink::dropFd()
{
  int fd = -1;
  {
    std::scoped_lock lock{m_fdMutex};
    fd = m_fd;
    m_fd = -1;
    m_connected = false;
  }
  if (fd >= 0) {
    platformClose(fd);
  }
  std::scoped_lock lock{m_queueMutex};
  m_queue.clear();
}

void MouserLink::enqueue(std::string bytes, bool requireConnected)
{
  if (requireConnected && !m_connected) {
    return;
  }
  {
    std::scoped_lock lock{m_queueMutex};
    if (m_queue.size() >= kMaxQueued) {
      m_queue.pop_front();
    }
    m_queue.push_back(Outbound{std::move(bytes)});
  }
  m_queueCv.notify_one();
}

void MouserLink::enqueueJson(const std::string &line, bool requireConnected)
{
  enqueue(line + "\n", requireConnected);
}

void MouserLink::writerLoop()
{
  while (m_running) {
    Outbound item;
    {
      std::unique_lock lock{m_queueMutex};
      m_queueCv.wait(lock, [this] { return !m_running || !m_queue.empty(); });
      if (!m_running) {
        return;
      }
      item = std::move(m_queue.front());
      m_queue.pop_front();
    }
    int fd = -1;
    {
      std::scoped_lock lock{m_fdMutex};
      fd = m_fd;
    }
    if (fd < 0) {
      continue; // session went away under us; the line is stale
    }
    if (!sendNow(fd, item.bytes)) {
      // Wake the reader so the session is torn down and rebuilt.
      std::scoped_lock lock{m_fdMutex};
      if (m_fd == fd) {
        platformShutdown(fd);
      }
    }
  }
}

bool MouserLink::sendNow(int fd, const std::string &bytes) const
{
  size_t sent = 0;
  while (sent < bytes.size()) {
    const auto wrote = ::send(fd, bytes.data() + sent, bytes.size() - sent, 0);
    if (wrote <= 0) {
      return false;
    }
    sent += static_cast<size_t>(wrote);
  }
  return true;
}

bool MouserLink::readLine(int fd, std::string &carry, std::string &line, bool &timedOut) const
{
  timedOut = false;
  while (true) {
    const auto newline = carry.find('\n');
    if (newline != std::string::npos) {
      line = carry.substr(0, newline);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      carry.erase(0, newline + 1);
      return true;
    }
    if (carry.size() > kMaxLineBytes) {
      LOG_WARN("mouser link: dropping oversized line");
      return false;
    }
    char buffer[4096];
    const auto received = ::recv(fd, buffer, sizeof(buffer), 0);
    if (received > 0) {
      carry.append(buffer, static_cast<size_t>(received));
      continue;
    }
    if (received < 0 && lastErrorIsTimeout()) {
      timedOut = true;
    }
    return false;
  }
}

bool MouserLink::sleepInterruptible(std::chrono::milliseconds duration)
{
  if (duration.count() <= 0) {
    return m_running;
  }
  std::unique_lock lock{m_sleepMutex};
  const unsigned epoch = m_wakeEpoch;
  m_sleepCv.wait_for(lock, duration, [this, epoch] { return !m_running || m_wakeEpoch != epoch; });
  return m_running;
}

std::string MouserLink::tokenFilePath() const
{
  return m_options.tokenFile.empty() ? defaultTokenFile() : m_options.tokenFile;
}

std::string MouserLink::readTokenFile() const
{
  const auto path = tokenFilePath();
  if (path.empty()) {
    return {};
  }
  QFile file(QString::fromStdString(path));
  if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
    return {};
  }
  return file.readAll().trimmed().toStdString();
}

void MouserLink::dispatchInbound(const std::string &line)
{
  // Held across the call on purpose: a Server clearing the handler from
  // its destructor must never race a dispatch into a dead object.
  std::scoped_lock lock{m_stateMutex};
  if (m_inbound) {
    m_inbound(line);
  }
}

void MouserLink::resetBackoff()
{
  m_backoff = m_options.initialBackoff;
}

void MouserLink::escalateBackoff()
{
  m_backoff = std::min(m_options.maxBackoff, m_backoff * 2);
}

} // namespace deskflow
