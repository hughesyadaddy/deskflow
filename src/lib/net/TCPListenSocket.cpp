/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "net/TCPListenSocket.h"

#include "arch/Arch.h"
#include "arch/ArchException.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "io/IOException.h"
#include "net/NetworkAddress.h"
#include "net/SocketException.h"
#include "net/SocketMultiplexer.h"
#include "net/TCPSocket.h"
#include "net/TSocketMultiplexerMethodJob.h"

#include <atomic>

//
// TCPListenSocket
//

TCPListenSocket::TCPListenSocket(
    IEventQueue *events, SocketMultiplexer *socketMultiplexer, IArchNetwork::AddressFamily family
)
    : m_events(events),
      m_socketMultiplexer(socketMultiplexer)
{
  try {
    m_socket = ARCH->newSocket(family, IArchNetwork::SocketType::Stream);
  } catch (ArchNetworkException &e) {
    throw SocketCreateException(e.what());
  }
}

TCPListenSocket::~TCPListenSocket()
{
  try {
    if (m_socket != nullptr) {
      m_socketMultiplexer->removeSocket(this);
      ARCH->closeSocket(m_socket);
    }
  } catch (...) {
    // ignore
    LOG_WARN("error while closing TCP socket");
  }
}

void TCPListenSocket::bind(const NetworkAddress &addr)
{
  LOG_DEBUG("binding to address: %s:%d", addr.getHostname().c_str(), addr.getPort());
  try {
    std::scoped_lock lock{m_mutex};

#if defined(Q_OS_UNIX)
    // Only reuse socket addr on Unix so we can restart the server quickly (Unix holds the port
    // in TIME_WAIT for a few mins after close). This is not needed on Windows and can cause issues
    // because binding to a re-use port makes it look like the server is listening when it is not.
    ARCH->setReuseAddrOnSocket(m_socket, true);
#endif

    ARCH->bindSocket(m_socket, addr.getAddress());
    ARCH->listenOnSocket(m_socket);
    m_socketMultiplexer->addSocket(
        this, new TSocketMultiplexerMethodJob<TCPListenSocket>(
                  this, &TCPListenSocket::serviceListening, m_socket, true, false
              )
    );
  } catch (ArchNetworkAddressInUseException &e) {
    throw SocketAddressInUseException(e.what());
  } catch (ArchNetworkException &e) {
    throw SocketBindException(e.what());
  }
}

void TCPListenSocket::close()
{
  std::scoped_lock lock{m_mutex};
  if (m_socket == nullptr) {
    throw IOClosedException();
  }
  try {
    m_socketMultiplexer->removeSocket(this);
    ARCH->closeSocket(m_socket);
    m_socket = nullptr;
  } catch (ArchNetworkException &e) {
    throw SocketIOCloseException(e.what());
  }
}

void *TCPListenSocket::getEventTarget() const
{
  return const_cast<void *>(static_cast<const void *>(this));
}

std::unique_ptr<IDataSocket> TCPListenSocket::accept()
{
  ListenRearm rearm{*this};
  ArchSocket raw = acceptRaw();
  if (raw == nullptr) {
    return nullptr;
  }
  try {
    return std::make_unique<TCPSocket>(m_events, m_socketMultiplexer, raw);
  } catch (std::exception &e) {
    // The connection was accepted but could not be set up: typically a
    // SocketCreateException because the peer already reset it (the wedge
    // probe closes with SO_LINGER 0; on macOS accept() still succeeds and
    // setsockopt(TCP_NODELAY) on the reset fd fails with EINVAL). This used
    // to be rethrown -- sliced to a bare std::exception -- out of the
    // server's event loop, which ended the epoch and leaked its listener.
    // Our own probe does this every 30 s on a server seat, so only the
    // first rejection and every 120th (about hourly) are WARN; the rest are
    // DEBUG so the level still means something for a real rejection.
    static std::atomic<unsigned long long> s_rejected{0};
    const unsigned long long n = ++s_rejected;
    if (n == 1 || n % 120 == 0) {
      LOG_WARN("rejected incoming connection (%llu so far): %s", n, e.what());
    } else {
      LOG_DEBUG("rejected incoming connection (%llu so far): %s", n, e.what());
    }
    return nullptr;
  }
}

ArchSocket TCPListenSocket::acceptRaw()
{
  ArchSocket listening = nullptr;
  {
    std::scoped_lock lock{m_mutex};
    listening = m_socket;
  }
  if (listening == nullptr) {
    return nullptr; // closed underneath a queued ListenSocketConnecting
  }
  try {
    // nullptr when nothing is waiting (EAGAIN); never hand that to a socket.
    return ARCH->acceptSocket(listening, nullptr);
  } catch (ArchNetworkException &e) {
    // ECONNABORTED and friends: the peer went away between the poll and
    // the accept. Nothing to adopt; the listener is re-armed by the caller.
    LOG_DEBUG("accept failed: %s", e.what());
    return nullptr;
  }
}

void TCPListenSocket::rearmListening()
{
  // serviceListening() dropped the multiplexer job when the connection
  // arrived ("stop polling until the client accepts"); without this the
  // listener is never polled again and the server is alive but accepts
  // nothing -- for every outcome of accept(), not only the happy path.
  try {
    std::scoped_lock lock{m_mutex};
    if (m_socket != nullptr) {
      setListeningJob();
    }
  } catch (std::exception &e) {
    LOG_WARN("cannot resume listening for clients: %s", e.what());
  }
}

void TCPListenSocket::setListeningJob()
{
  m_socketMultiplexer->addSocket(
      this,
      new TSocketMultiplexerMethodJob<TCPListenSocket>(this, &TCPListenSocket::serviceListening, m_socket, true, false)
  );
}

ISocketMultiplexerJob *TCPListenSocket::serviceListening(ISocketMultiplexerJob *job, bool read, bool, bool error)
{
  if (error) {
    close();
    return nullptr;
  }
  if (read) {
    m_events->addEvent(Event(EventTypes::ListenSocketConnecting, this));
    // stop polling on this socket until the client accepts
    return nullptr;
  }
  return job;
}
