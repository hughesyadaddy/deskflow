/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "arch/IArchNetwork.h"
#include "base/Event.h"
#include "io/StreamBuffer.h"
#include "mt/CondVar.h"
#include "mt/Mutex.h"
#include "net/IDataSocket.h"

#include <atomic>

class Mutex;
class Thread;
class ISocketMultiplexerJob;
class IEventQueue;
class SocketMultiplexer;

//! TCP data socket
/*!
A data socket using TCP.
*/
class TCPSocket : public IDataSocket
{
public:
  TCPSocket(
      IEventQueue *events, SocketMultiplexer *socketMultiplexer,
      IArchNetwork::AddressFamily family = IArchNetwork::AddressFamily::INet
  );
  TCPSocket(IEventQueue *events, SocketMultiplexer *socketMultiplexer, ArchSocket socket);
  TCPSocket(TCPSocket const &) = delete;
  TCPSocket(TCPSocket &&) = delete;
  ~TCPSocket() override;

  TCPSocket &operator=(TCPSocket const &) = delete;
  TCPSocket &operator=(TCPSocket &&) = delete;

  // ISocket overrides
  void bind(const NetworkAddress &) override;
  void close() override;
  void *getEventTarget() const override;
  void onMultiplexerShutdown() override;

  // IStream overrides
  uint32_t read(void *buffer, uint32_t n) override;
  void write(const void *buffer, uint32_t n) override;
  void flush() override;
  void shutdownInput() override;
  void shutdownOutput() override;
  bool isReady() const override;
  bool isFatal() const override;
  uint32_t getSize() const override;

  // IDataSocket overrides
  void connect(const NetworkAddress &) override;

  virtual ISocketMultiplexerJob *newJob();

  //! @name output buffer bound
  //@{

  //! Default cap on bytes queued for a peer that is not draining (8 MiB).
  static constexpr uint32_t kDefaultMaxOutputBufferSize = 8 * 1024 * 1024;

  //! Cap on bytes slurped from the wire ahead of the reader (1 MiB).
  static constexpr uint32_t kMaxInputBufferSize = 1024 * 1024;

  //! Bytes handed to a single writeSocket()/SSL_write() pass.
  /*!
  Bounds how much of the output buffer is consolidated into one contiguous
  span per pass, so a large queued transfer is never copied whole.
  */
  static constexpr uint32_t kMaxWritePassSize = 64 * 1024;

  //! Set the process-wide default output cap used by new sockets.
  static void setDefaultMaxOutputBufferSize(uint32_t bytes);
  static uint32_t defaultMaxOutputBufferSize();

  //! Set this socket's output cap.
  /*!
  When a \c write() would push the queued output past the cap the socket
  discards its queued output, shuts its output side and raises
  \c StreamOutputError: the peer is not draining and buffering further only
  grows without bound.  The stream is packet framed, so dropping individual
  writes is not an option; the owner (ClientProxy / Client) treats the error
  as a disconnect and the peer reconnects.
  */
  void setMaxOutputBufferSize(uint32_t bytes);
  uint32_t maxOutputBufferSize() const;

  //! Bytes currently queued for output.
  uint32_t outputBufferSize() const;

  //! True once a write was refused because the cap was exceeded.
  bool outputOverflowed() const;

  //@}

protected:
  enum class JobResult
  {
    Break = -1, //!< Break the Job chain
    Retry,      //!< Retry the same job
    New         //!< Require a new job
  };

  ArchSocket getSocket()
  {
    return m_socket;
  }
  IEventQueue *getEvents()
  {
    return m_events;
  }
  virtual JobResult doRead();
  virtual JobResult doWrite();

  void setJob(ISocketMultiplexerJob *);

  bool isConnected() const
  {
    return m_connected;
  }

  void setConnected(bool connected)
  {
    if (m_connected == connected)
      return;
    m_connected = connected;
  }

  bool isReadable() const
  {
    return m_readable;
  }

  void setReadable(bool readable)
  {
    if (m_readable == readable)
      return;
    m_readable = readable;
  }

  bool isWritable() const
  {
    return m_writable;
  }

  void setWritable(bool canWrite)
  {
    if (canWrite == m_writable)
      return;
    m_writable = canWrite;
  }

  Mutex &getMutex()
  {
    return m_mutex;
  }

  void sendEvent(EventTypes);
  void discardWrittenData(int bytesWrote);

  StreamBuffer m_inputBuffer;
  StreamBuffer m_outputBuffer;

private:
  void init();

  void sendConnectionFailedEvent(const char *);
  void onConnected();
  void onInputShutdown();
  void onOutputShutdown();
  void onDisconnected();

  ISocketMultiplexerJob *serviceConnecting(ISocketMultiplexerJob *, bool, bool, bool);
  ISocketMultiplexerJob *serviceConnected(ISocketMultiplexerJob *, bool, bool, bool);

  bool m_readable;
  bool m_writable;
  bool m_connected;
  bool m_outputOverflowed = false;
  uint32_t m_maxOutputBufferSize;
  static std::atomic<uint32_t> s_defaultMaxOutputBufferSize;
  Mutex m_mutex;
  ArchSocket m_socket;
  IEventQueue *m_events;
  CondVar<bool> m_flushed;
  SocketMultiplexer *m_socketMultiplexer;
};
