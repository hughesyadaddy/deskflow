/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/CoordinationMesh.h"

#include "base/Log.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketLen = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketLen = socklen_t;
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>

namespace deskflow::coordination {

namespace {

const int kSendConnectTimeoutMs = 700;
const int kClientReadTimeoutMs = 2000;
const size_t kMaxLineBytes = 16 * 1024;
const int kMaxConcurrentClients = 8;

void platformCloseSocket(int fd)
{
#if defined(_WIN32)
  ::closesocket(fd);
#else
  ::close(fd);
#endif
}

void setReceiveTimeout(int fd, int timeoutMs)
{
#if defined(_WIN32)
  DWORD value = static_cast<DWORD>(timeoutMs);
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&value), sizeof(value));
#else
  timeval value{};
  value.tv_sec = timeoutMs / 1000;
  value.tv_usec = (timeoutMs % 1000) * 1000;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&value), sizeof(value));
#endif
}

//! Resolve + connect with a bounded timeout; returns fd or -1.
int connectWithTimeout(const std::string &host, int port, int timeoutMs)
{
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *results = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &results) != 0 || results == nullptr) {
    return -1;
  }

  int fd = -1;
  for (addrinfo *entry = results; entry != nullptr; entry = entry->ai_next) {
    fd = static_cast<int>(::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol));
    if (fd < 0) {
      continue;
    }

#if defined(_WIN32)
    u_long nonBlocking = 1;
    ::ioctlsocket(fd, FIONBIO, &nonBlocking);
#else
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif

    const auto rc = ::connect(fd, entry->ai_addr, static_cast<SocketLen>(entry->ai_addrlen));
    bool connected = (rc == 0);
    if (!connected) {
      fd_set writeSet;
      FD_ZERO(&writeSet);
      FD_SET(fd, &writeSet);
      timeval timeout{};
      timeout.tv_sec = timeoutMs / 1000;
      timeout.tv_usec = (timeoutMs % 1000) * 1000;
      if (::select(fd + 1, nullptr, &writeSet, nullptr, &timeout) == 1) {
        int error = 0;
        SocketLen errorLen = sizeof(error);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &errorLen);
        connected = (error == 0);
      }
    }

    if (connected) {
#if defined(_WIN32)
      u_long blocking = 0;
      ::ioctlsocket(fd, FIONBIO, &blocking);
#else
      const int restored = ::fcntl(fd, F_GETFL, 0);
      ::fcntl(fd, F_SETFL, restored & ~O_NONBLOCK);
#endif
      break;
    }
    platformCloseSocket(fd);
    fd = -1;
  }

  ::freeaddrinfo(results);
  return fd;
}

bool sendAll(int fd, const std::string &payload)
{
  size_t sent = 0;
  while (sent < payload.size()) {
    const auto wrote = ::send(fd, payload.data() + sent, payload.size() - sent, 0);
    if (wrote <= 0) {
      return false;
    }
    sent += static_cast<size_t>(wrote);
  }
  return true;
}

} // namespace

CoordinationMesh::CoordinationMesh(int port, std::string token, Receiver receiver)
    : m_port(port),
      m_token(std::move(token)),
      m_receiver(std::move(receiver))
{
  // do nothing
}

CoordinationMesh::~CoordinationMesh()
{
  stop();
}

bool CoordinationMesh::start()
{
  m_listenFd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
  if (m_listenFd < 0) {
    LOG_WARN("coordination: mesh socket() failed: %s", std::strerror(errno));
    return false;
  }
#if !defined(_WIN32)
  // Close-on-exec: spawned children (e.g. peer wakeCommand hooks) must not
  // inherit the listen socket, or a hung child holds the mesh port across
  // deskflow restarts.
  ::fcntl(m_listenFd, F_SETFD, FD_CLOEXEC);
#endif
  const int reuse = 1;
  ::setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(m_port));
  address.sin_addr.s_addr = htonl(INADDR_ANY); // peers are remote by definition

  if (::bind(m_listenFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
    LOG_WARN("coordination: mesh failed to bind port %d: %s", m_port, std::strerror(errno));
    platformCloseSocket(m_listenFd);
    m_listenFd = -1;
    return false;
  }
  if (::listen(m_listenFd, 4) != 0) {
    LOG_WARN("coordination: mesh listen() failed: %s", std::strerror(errno));
    platformCloseSocket(m_listenFd);
    m_listenFd = -1;
    return false;
  }

  if (m_port == 0) {
    // Ephemeral port (tests): report what the OS actually assigned.
    sockaddr_in bound{};
    SocketLen boundLen = sizeof(bound);
    if (::getsockname(m_listenFd, reinterpret_cast<sockaddr *>(&bound), &boundLen) == 0) {
      m_port = ntohs(bound.sin_port);
    }
  }

  m_running = true;
  m_handlers.reserve(kHandlerPoolSize);
  for (int i = 0; i < kHandlerPoolSize; ++i) {
    m_handlers.emplace_back([this] { handlerLoop(); });
  }
  m_thread = std::thread([this] { serveLoop(); });
  LOG_INFO("coordination: mesh listening on port %d", m_port);
  return true;
}

void CoordinationMesh::stop()
{
  m_running = false;
  if (m_listenFd >= 0) {
    platformCloseSocket(m_listenFd);
    m_listenFd = -1;
  }
  if (m_thread.joinable()) {
    m_thread.join();
  }
  // Unblock every in-flight handler (pending fds included: they sit in
  // m_clientFds too), then join the pool so a handler thread can never
  // touch *this after destruction. Handlers are time-bounded (2 s read
  // timeout, sockets shut down here), so the join converges.
  {
    std::scoped_lock lock{m_clientsMutex};
    for (const int fd : m_clientFds) {
#if defined(_WIN32)
      ::shutdown(fd, SD_BOTH);
#else
      ::shutdown(fd, SHUT_RDWR);
#endif
    }
  }
  m_pendingReady.notify_all();
  for (auto &handler : m_handlers) {
    if (handler.joinable()) {
      handler.join();
    }
  }
  m_handlers.clear();
  std::scoped_lock lock{m_clientsMutex};
  for (const int fd : m_pendingFds) {
    m_clientFds.erase(fd);
    platformCloseSocket(fd);
  }
  m_pendingFds.clear();
}

bool CoordinationMesh::sendTo(const std::string &host, const std::string &line)
{
  const int fd = connectWithTimeout(host, m_port, kSendConnectTimeoutMs);
  if (fd < 0) {
    return false; // unreachable peers are normal (asleep / off)
  }
  std::string payload = line;
  payload.push_back('\n');
  if (!sendAll(fd, payload)) {
    platformCloseSocket(fd);
    return false;
  }
  platformCloseSocket(fd);
  return true;
}

bool CoordinationMesh::probe(const std::string &host, int timeoutMs)
{
  const int fd = connectWithTimeout(host, m_port, timeoutMs);
  if (fd < 0) {
    return false;
  }
  platformCloseSocket(fd);
  return true;
}

bool CoordinationMesh::probeDeskflowPort(int port, int timeoutMs)
{
  const int fd = connectWithTimeout("127.0.0.1", port, timeoutMs);
  if (fd < 0) {
    return false;
  }
  platformCloseSocket(fd);
  return true;
}

std::string CoordinationMesh::query(const std::string &host, const std::string &line)
{
  const int fd = connectWithTimeout(host, m_port, kSendConnectTimeoutMs);
  if (fd < 0) {
    return {};
  }
  std::string payload = line;
  payload.push_back('\n');
  std::string reply;
  if (sendAll(fd, payload)) {
    setReceiveTimeout(fd, 1000);
    char buffer[4096];
    while (reply.find('\n') == std::string::npos && reply.size() < kMaxLineBytes) {
      const auto received = ::recv(fd, buffer, sizeof(buffer), 0);
      if (received <= 0) {
        break;
      }
      reply.append(buffer, static_cast<size_t>(received));
    }
  }
  platformCloseSocket(fd);
  const auto newline = reply.find('\n');
  return newline == std::string::npos ? reply : reply.substr(0, newline);
}

bool CoordinationMesh::tokenOk(const Message &message) const
{
  if (m_token.empty()) {
    return true;
  }
  if (message.token.size() != m_token.size()) {
    return false;
  }
  // Constant-time compare; the mesh is reachable from the LAN.
  unsigned char diff = 0;
  for (size_t i = 0; i < m_token.size(); ++i) {
    diff |= static_cast<unsigned char>(m_token[i] ^ message.token[i]);
  }
  return diff == 0;
}

void CoordinationMesh::serveLoop()
{
  while (m_running) {
    sockaddr_in peer{};
    SocketLen peerLen = sizeof(peer);
    const int clientFd = static_cast<int>(::accept(m_listenFd, reinterpret_cast<sockaddr *>(&peer), &peerLen));
    if (clientFd < 0) {
      break; // listener closed by stop()
    }
    if (!m_running) {
      platformCloseSocket(clientFd);
      break;
    }
    {
      std::scoped_lock lock{m_clientsMutex};
      if (m_activeClients.load() + static_cast<int>(m_pendingFds.size()) >= kMaxConcurrentClients) {
        // A stalled or hostile peer set must not starve the mesh; excess
        // connections are refused rather than queued behind them.
        platformCloseSocket(clientFd);
        continue;
      }
      m_clientFds.insert(clientFd);
      m_pendingFds.push_back(clientFd);
    }
    m_pendingReady.notify_one();
  }
}

void CoordinationMesh::handlerLoop()
{
  while (true) {
    int clientFd = -1;
    {
      std::unique_lock lock{m_clientsMutex};
      m_pendingReady.wait(lock, [this] { return !m_running || !m_pendingFds.empty(); });
      if (!m_running) {
        return; // stop() closes whatever is still pending
      }
      clientFd = m_pendingFds.front();
      m_pendingFds.pop_front();
      ++m_activeClients;
    }
    handleClient(clientFd);
    {
      std::scoped_lock lock{m_clientsMutex};
      m_clientFds.erase(clientFd);
    }
    platformCloseSocket(clientFd);
    --m_activeClients;
  }
}

void CoordinationMesh::handleClient(int clientFd)
{
  setReceiveTimeout(clientFd, kClientReadTimeoutMs);

  std::string carry;
  char buffer[4096];
  while (m_running) {
    const auto newline = carry.find('\n');
    if (newline != std::string::npos) {
      const std::string line = carry.substr(0, newline);
      carry.erase(0, newline + 1);
      if (line.empty()) {
        continue;
      }
      const Message message = protocol::decode(line);
      if (message.type == Message::Type::Invalid) {
        continue;
      }
      if (!tokenOk(message)) {
        LOG_DEBUG("coordination: dropping message with bad token");
        continue;
      }
      if (message.type == Message::Type::Cursor || message.type == Message::Type::KeyFwd) {
        // Legacy mesh v1 traffic; drop the message but keep the connection
        // (it may carry pipelined valid messages), like the bad-token case.
        LOG_DEBUG("coordination: dropping legacy mesh v1 message");
        continue;
      }
      m_receiver(message, [clientFd](const std::string &reply) {
        std::string payload = reply;
        payload.push_back('\n');
        sendAll(clientFd, payload);
      });
      continue;
    }
    if (carry.size() > kMaxLineBytes) {
      return;
    }
    const auto received = ::recv(clientFd, buffer, sizeof(buffer), 0);
    if (received <= 0) {
      return; // EOF or timeout: legacy senders are one-shot
    }
    carry.append(buffer, static_cast<size_t>(received));
  }
}

//
// PeerOutbox
//

PeerOutbox::PeerOutbox(std::string ip, std::string lan, Transport transport, Clock clock)
    : m_ip(std::move(ip)),
      m_lan(std::move(lan)),
      m_transport(std::move(transport)),
      m_clock(std::move(clock)),
      m_preferLan(!m_lan.empty())
{
  // do nothing
}

PeerOutbox::~PeerOutbox()
{
  stop();
}

void PeerOutbox::start()
{
  std::scoped_lock lock{m_mutex};
  if (m_thread.joinable()) {
    return;
  }
  m_stop = false;
  m_thread = std::thread([this] { run(); });
}

void PeerOutbox::stop()
{
  {
    std::scoped_lock lock{m_mutex};
    m_stop = true;
  }
  m_wake.notify_all();
  m_jobDone.notify_all();
  if (m_thread.joinable()) {
    m_thread.join(); // at most one bounded connect attempt in flight
  }
}

void PeerOutbox::post(std::string line, ReplyHandler onReply)
{
  {
    std::scoped_lock lock{m_mutex};
    m_queue.push_back(Job{std::move(line), std::move(onReply)});
    ++m_posted;
    if (m_queue.size() > kMaxQueuedLines) {
      m_queue.pop_front();
      ++m_resolved; // dropped counts as resolved for forward() waiters
    }
  }
  m_wake.notify_all();
}

bool PeerOutbox::forward(std::string line, int graceMs)
{
  uint64_t ticket = 0;
  {
    std::scoped_lock lock{m_mutex};
    if (m_state == State::Backoff) {
      return false;
    }
    m_queue.push_back(Job{std::move(line), {}});
    ticket = ++m_posted;
    if (m_queue.size() > kMaxQueuedLines) {
      m_queue.pop_front();
      ++m_resolved;
    }
  }
  m_wake.notify_all();

  std::unique_lock lock{m_mutex};
  if (m_state == State::Reachable) {
    return true;
  }
  // Unknown: give the lane thread a short, bounded chance to resolve this
  // very job. A timeout is reported as "not delivered" (the key stays
  // local); the attempt itself keeps running and settles the state.
  m_jobDone.wait_for(lock, std::chrono::milliseconds(graceMs), [&] { return m_resolved >= ticket || m_stop; });
  return m_resolved >= ticket && m_state == State::Reachable;
}

PeerOutbox::State PeerOutbox::state() const
{
  std::scoped_lock lock{m_mutex};
  return m_state;
}

std::string PeerOutbox::preferredAddress() const
{
  std::scoped_lock lock{m_mutex};
  return (m_preferLan && !m_lan.empty()) ? m_lan : m_ip;
}

double PeerOutbox::nextAttemptAt() const
{
  std::scoped_lock lock{m_mutex};
  return m_nextAttemptAt;
}

bool PeerOutbox::idle() const
{
  std::scoped_lock lock{m_mutex};
  return m_queue.empty() && !m_inFlight;
}

std::string PeerOutbox::otherAddressLocked(const std::string &host) const
{
  if (m_lan.empty() || m_lan == m_ip) {
    return {};
  }
  return host == m_lan ? m_ip : m_lan;
}

void PeerOutbox::pump(double now)
{
  while (true) {
    Job job;
    std::string host;
    bool alternateOnFailure = false;
    {
      std::scoped_lock lock{m_mutex};
      if (m_queue.empty() || now < m_nextAttemptAt || m_stop) {
        return;
      }
      job = std::move(m_queue.front());
      m_queue.pop_front();
      host = (m_preferLan && !m_lan.empty()) ? m_lan : m_ip;
      // Only a peer we believed reachable (or never tried) earns a second
      // address on the same attempt; in backoff it is one connect per window.
      alternateOnFailure = m_state != State::Backoff;
      m_inFlight = true;
    }

    std::string reply;
    std::string *replyOut = job.onReply ? &reply : nullptr;
    bool ok = m_transport(host, job.line, replyOut);
    if (!ok && alternateOnFailure) {
      std::string other;
      {
        std::scoped_lock lock{m_mutex};
        other = m_stop ? std::string{} : otherAddressLocked(host);
      }
      if (!other.empty()) {
        reply.clear();
        ok = m_transport(other, job.line, replyOut);
        if (ok) {
          host = other;
        }
      }
    }

    {
      std::scoped_lock lock{m_mutex};
      m_inFlight = false;
      if (ok) {
        m_state = State::Reachable;
        m_backoffS = 0.0;
        m_nextAttemptAt = 0.0;
        m_preferLan = !m_lan.empty() && host == m_lan;
        ++m_resolved;
      } else {
        m_backoffS = m_backoffS <= 0.0 ? kBackoffMinS : std::min(m_backoffS * 2.0, kBackoffMaxS);
        m_state = State::Backoff;
        m_nextAttemptAt = m_clock() + m_backoffS;
        if (!otherAddressLocked(host).empty()) {
          m_preferLan = !m_preferLan; // alternate lan/ip across windows
        }
        // Everything behind the failed line is stale by now (>= one
        // connect timeout old); periodic senders re-post.
        m_queue.clear();
        m_resolved = m_posted;
      }
    }
    m_jobDone.notify_all();

    if (!ok) {
      return;
    }
    if (job.onReply) {
      job.onReply(reply);
    }
  }
}

void PeerOutbox::run()
{
  std::unique_lock lock{m_mutex};
  while (!m_stop) {
    if (m_queue.empty()) {
      m_wake.wait(lock);
      continue;
    }
    const double now = m_clock();
    if (now < m_nextAttemptAt) {
      m_wake.wait_for(lock, std::chrono::duration<double>(m_nextAttemptAt - now));
      continue;
    }
    lock.unlock();
    pump(now);
    lock.lock();
  }
}

} // namespace deskflow::coordination
