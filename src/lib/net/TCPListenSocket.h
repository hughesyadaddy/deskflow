/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "arch/IArchNetwork.h"
#include "net/IListenSocket.h"

#include <mutex>

class ISocketMultiplexerJob;
class IEventQueue;
class SocketMultiplexer;

//! TCP listen socket
/*!
A listen socket using TCP.
*/
class TCPListenSocket : public IListenSocket
{
public:
  TCPListenSocket(IEventQueue *events, SocketMultiplexer *socketMultiplexer, IArchNetwork::AddressFamily family);
  TCPListenSocket(TCPListenSocket const &) = delete;
  TCPListenSocket(TCPListenSocket &&) = delete;
  ~TCPListenSocket() override;

  TCPListenSocket &operator=(TCPListenSocket const &) = delete;
  TCPListenSocket &operator=(TCPListenSocket &&) = delete;

  // ISocket overrides
  void bind(const NetworkAddress &) override;
  void close() override;
  void *getEventTarget() const override;

  // IListenSocket overrides
  std::unique_ptr<IDataSocket> accept() override;

  ISocketMultiplexerJob *serviceListening(ISocketMultiplexerJob *, bool, bool, bool);

protected:
  void setListeningJob();

  //! Accept the waiting connection; nullptr (never a throw) when there is
  //! none or the accept itself failed.
  ArchSocket acceptRaw();

  //! Put the listen socket back under the multiplexer after an accept
  //! attempt (any outcome). No-op once closed; never throws.
  void rearmListening();

  //! Scope guard: re-arm listening on every exit path of accept().
  struct ListenRearm
  {
    TCPListenSocket &listen;
    ~ListenRearm()
    {
      listen.rearmListening();
    }
  };

  ArchSocket socket() const
  {
    return m_socket;
  }

  IEventQueue *events() const
  {
    return m_events;
  }

  SocketMultiplexer *socketMultiplexer() const
  {
    return m_socketMultiplexer;
  }

private:
  ArchSocket m_socket;
  IEventQueue *m_events;
  SocketMultiplexer *m_socketMultiplexer;
  std::mutex m_mutex;
};
