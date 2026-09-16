/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "TCPSocketTests.h"

#include "arch/Arch.h"
#include "base/Event.h"
#include "base/EventTypes.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "net/TCPSocket.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

Log g_log;
std::unique_ptr<Arch> g_arch;

//! Minimal event queue that only records what the socket raises.
/*!
The real EventQueue parks events in a pending list until \c loop() runs, so
a synchronous test cannot observe them.  The socket only ever calls
\c addEvent() on this path.
*/
class RecordingEventQueue : public IEventQueue
{
public:
  std::vector<EventTypes> types;

  int count(EventTypes type) const
  {
    return static_cast<int>(std::count(types.begin(), types.end(), type));
  }

  int loop() override
  {
    return 0;
  }
  void adoptBuffer(IEventQueueBuffer *) override
  {
  }
  bool getEvent(Event &, double) override
  {
    return false;
  }
  bool dispatchEvent(const Event &) override
  {
    return false;
  }
  void addEvent(Event &&event) override
  {
    types.push_back(event.getType());
    Event::deleteData(event);
  }
  EventQueueTimer *newTimer(double, void *) override
  {
    return nullptr;
  }
  EventQueueTimer *newOneShotTimer(double, void *) override
  {
    return nullptr;
  }
  void deleteTimer(EventQueueTimer *) override
  {
  }
  void addHandler(EventTypes, void *, const EventHandler &) override
  {
  }
  void removeHandler(EventTypes, void *) override
  {
  }
  void removeHandlers(void *) override
  {
  }
  void waitForReady() const override
  {
  }
  void *getSystemTarget() override
  {
    return nullptr;
  }
};

//! A connected-state socket with no multiplexer: nothing drains it.
/*!
Models a peer that has stopped reading.  The accepted-socket constructor
puts the socket straight into the connected/writable state, and a null
multiplexer means no service thread ever calls doWrite(), so everything
written stays queued -- exactly the stalled-client case the cap exists for.
*/
std::unique_ptr<TCPSocket> stalledSocket(IEventQueue *events)
{
  ArchSocket raw = ARCH->newSocket(IArchNetwork::AddressFamily::INet, IArchNetwork::SocketType::Stream);
  return std::make_unique<TCPSocket>(events, nullptr, raw);
}

const std::vector<uint8_t> kBlock(4096, 0xAB);

} // namespace

void TCPSocketTests::initTestCase()
{
  g_arch = std::make_unique<Arch>();
  g_log.setFilter(LogLevel::Level::Error);
}

void TCPSocketTests::cleanupTestCase()
{
  g_arch.reset();
}

void TCPSocketTests::caps_defaultsAreDocumentedValues()
{
  // output cap: 8 MiB; input cap: unchanged at 1 MiB
  QCOMPARE(TCPSocket::kDefaultMaxOutputBufferSize, 8u * 1024 * 1024);
  QCOMPARE(TCPSocket::defaultMaxOutputBufferSize(), 8u * 1024 * 1024);
  QCOMPARE(TCPSocket::kMaxInputBufferSize, 1024u * 1024);
  QVERIFY(TCPSocket::kMaxWritePassSize < TCPSocket::kDefaultMaxOutputBufferSize);
}

void TCPSocketTests::defaultCap_appliesToNewSockets()
{
  RecordingEventQueue events;
  {
    auto socket = stalledSocket(&events);
    QCOMPARE(socket->maxOutputBufferSize(), TCPSocket::kDefaultMaxOutputBufferSize);
  }

  TCPSocket::setDefaultMaxOutputBufferSize(1234);
  {
    auto socket = stalledSocket(&events);
    QCOMPARE(socket->maxOutputBufferSize(), 1234u);
    socket->setMaxOutputBufferSize(99);
    QCOMPARE(socket->maxOutputBufferSize(), 99u);
  }
  TCPSocket::setDefaultMaxOutputBufferSize(TCPSocket::kDefaultMaxOutputBufferSize);
  QCOMPARE(TCPSocket::defaultMaxOutputBufferSize(), TCPSocket::kDefaultMaxOutputBufferSize);
}

void TCPSocketTests::writeWithinCap_buffersWithoutError()
{
  RecordingEventQueue events;
  auto socket = stalledSocket(&events);

  const uint32_t total = 1024 * 1024;
  for (uint32_t written = 0; written < total; written += kBlock.size()) {
    socket->write(kBlock.data(), static_cast<uint32_t>(kBlock.size()));
  }

  QCOMPARE(socket->outputBufferSize(), total);
  QVERIFY(!socket->outputOverflowed());
  QCOMPARE(events.count(EventTypes::StreamOutputError), 0);
  QCOMPARE(events.count(EventTypes::StreamOutputShutdown), 0);
}

void TCPSocketTests::writePastCap_raisesBackpressureAndDropsQueue()
{
  RecordingEventQueue events;
  auto socket = stalledSocket(&events);

  const uint32_t cap = 64 * 1024;
  socket->setMaxOutputBufferSize(cap);

  // fill exactly to the cap: allowed
  for (uint32_t written = 0; written < cap; written += kBlock.size()) {
    socket->write(kBlock.data(), static_cast<uint32_t>(kBlock.size()));
  }
  QCOMPARE(socket->outputBufferSize(), cap);
  QVERIFY(!socket->outputOverflowed());
  QCOMPARE(events.count(EventTypes::StreamOutputError), 0);

  // one more byte: backpressure
  const uint8_t one = 0x01;
  socket->write(&one, 1);
  QVERIFY(socket->outputOverflowed());
  QCOMPARE(events.count(EventTypes::StreamOutputError), 1);
  // queued output released, not pinned behind a peer that will never drain
  QCOMPARE(socket->outputBufferSize(), 0u);
  // flush() must not block on data that will never be sent
  socket->flush();

  // subsequent writes are refused (output side is shut) and stay unbuffered
  socket->write(kBlock.data(), static_cast<uint32_t>(kBlock.size()));
  QCOMPARE(socket->outputBufferSize(), 0u);
  QCOMPARE(events.count(EventTypes::StreamOutputError), 2);
}

QTEST_MAIN(TCPSocketTests)
