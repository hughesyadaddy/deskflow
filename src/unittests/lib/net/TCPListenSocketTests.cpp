/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

// K8 epoch rebind (2026-09-25 fleet outage). The coordinator's wedge probe
// connects to the server port and closes with SO_LINGER 0 (RST). On macOS
// the server still accept()s that connection, but TCP_NODELAY on the
// already-reset fd fails with EINVAL; TCPSocket::init() turned that into a
// SocketCreateException, and TCPListenSocket::accept() rethrew it BY VALUE
// (`throw ex;` -- sliced to a bare std::exception) out of the server's
// event loop: "FATAL: an error occurred: std::exception", epoch dead,
// listener leaked in-process, every rebuild EADDRINUSE. These tests pin:
//  - a bad incoming connection never escapes accept(), and the listener
//    keeps accepting afterwards (the job is re-armed on every outcome);
//  - a listener + multiplexer torn down (bound or EADDRINUSE) leaves the
//    process fd table exactly as it was: no listen socket, no poll-unblock
//    pipe (ArchNetworkBSD used to attach that pipe to the calling thread
//    and never close it: +4 fds per failed epoch, ~2600 fds in 10 min).

#include "arch/Arch.h"
#include "base/Event.h"
#include "base/EventTypes.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "net/IDataSocket.h"
#include "net/NetworkAddress.h"
#include "net/SocketException.h"
#include "net/SocketMultiplexer.h"
#include "net/TCPListenSocket.h"

#include <QTest>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

class TCPListenSocketTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();
  void acceptAfterPeerResetNeverThrowsAndKeepsListening();
  void bindOnBusyPortThrowsAddressInUseWithoutLeakingFds();
  void listenerTeardownLeavesNoFdsBehind();
};

namespace {

Log g_log;
std::unique_ptr<Arch> g_arch;

//! Thread-safe recorder: the multiplexer thread posts ListenSocketConnecting.
class CountingEventQueue : public IEventQueue
{
public:
  std::atomic<int> connecting{0};

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
    if (event.getType() == EventTypes::ListenSocketConnecting) {
      ++connecting;
    }
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

#if !defined(_WIN32)
int openFdCount()
{
  DIR *dir = opendir("/dev/fd");
  if (dir == nullptr) {
    return -1;
  }
  int count = 0;
  while (readdir(dir) != nullptr) {
    ++count;
  }
  closedir(dir);
  return count; // includes ".", ".." and the DIR's own fd: constant offsets
}

//! A loopback port nobody is listening on right now.
int freeLoopbackPort()
{
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
  socklen_t len = sizeof(addr);
  ::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len);
  const int port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

//! A resolved listen address (NetworkAddress does not resolve in its ctor).
NetworkAddress loopback(int port)
{
  NetworkAddress address("127.0.0.1", port);
  address.resolve();
  return address;
}

//! Connect to 127.0.0.1:port; returns the fd (caller closes).
int connectLoopback(int port)
{
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

//! The wedge probe's exact pattern: connect, then reset (SO_LINGER 0).
void probeAndReset(int port)
{
  const int fd = connectLoopback(port);
  QVERIFY(fd >= 0);
  linger reset{};
  reset.l_onoff = 1;
  reset.l_linger = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char *>(&reset), sizeof(reset));
  ::close(fd);
}

bool waitFor(const std::function<bool()> &condition, std::chrono::milliseconds limit = std::chrono::seconds(3))
{
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!condition()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}
#endif

} // namespace

void TCPListenSocketTests::initTestCase()
{
  g_arch = std::make_unique<Arch>();
}

void TCPListenSocketTests::cleanupTestCase()
{
  g_arch.reset();
}

void TCPListenSocketTests::acceptAfterPeerResetNeverThrowsAndKeepsListening()
{
#if defined(_WIN32)
  QSKIP("loopback fd tests are POSIX-only");
#else
  CountingEventQueue events;
  SocketMultiplexer multiplexer;
  TCPListenSocket listen(&events, &multiplexer, IArchNetwork::AddressFamily::INet);
  const int port = freeLoopbackPort();
  listen.bind(loopback(port));

  // 1. The probe: connect + RST. The multiplexer flags the connection and
  //    drops the listen job until accept() is called.
  probeAndReset(port);
  QVERIFY(waitFor([&] { return events.connecting.load() >= 1; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(50)); // let the RST land
  std::unique_ptr<IDataSocket> first;
  try {
    first = listen.accept();
  } catch (std::exception &e) {
    QFAIL(qPrintable(QString("accept() must not throw for a reset connection: %1").arg(e.what())));
  }
  // nullptr (rejected) or a socket: either is fine, an exception is not.
  first.reset();

  // 2. The listener must still accept a real client afterwards: without
  //    the re-arm, ListenSocketConnecting never fires again (the "alive
  //    but not accepting" server the wedge probe was written for).
  const int client = connectLoopback(port);
  QVERIFY(client >= 0);
  QVERIFY2(waitFor([&] { return events.connecting.load() >= 2; }), "listener stopped accepting after a bad connection");
  std::unique_ptr<IDataSocket> second;
  try {
    second = listen.accept();
  } catch (std::exception &e) {
    QFAIL(qPrintable(QString("accept() threw for a healthy connection: %1").arg(e.what())));
  }
  QVERIFY(second != nullptr);
  ::close(client);
  second.reset();

  // 3. And again after a second reset: the guard re-arms every time.
  probeAndReset(port);
  QVERIFY(waitFor([&] { return events.connecting.load() >= 3; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  QVERIFY(!listen.accept() || true);
  const int client2 = connectLoopback(port);
  QVERIFY(client2 >= 0);
  QVERIFY(waitFor([&] { return events.connecting.load() >= 4; }));
  auto third = listen.accept();
  QVERIFY(third != nullptr);
  ::close(client2);
#endif
}

void TCPListenSocketTests::bindOnBusyPortThrowsAddressInUseWithoutLeakingFds()
{
#if defined(_WIN32)
  QSKIP("loopback fd tests are POSIX-only");
#else
  // Somebody else holds the port (another process -- or, before the
  // teardown fix, this very process's leaked listener).
  const int port = freeLoopbackPort();
  const int holder = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  QCOMPARE(::bind(holder, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), 0);
  QCOMPARE(::listen(holder, 1), 0);

  CountingEventQueue events;
  // One warm-up so any lazily created per-thread state (the main thread's
  // own unblock pipe) is not mistaken for a per-epoch leak.
  {
    SocketMultiplexer multiplexer;
    TCPListenSocket listen(&events, &multiplexer, IArchNetwork::AddressFamily::INet);
    try {
      listen.bind(loopback(port));
    } catch (SocketAddressInUseException &) {
    }
  }

  const int before = openFdCount();
  QVERIFY(before > 0);
  const int failedEpochs = 25;
  for (int i = 0; i < failedEpochs; ++i) {
    SocketMultiplexer multiplexer;
    TCPListenSocket listen(&events, &multiplexer, IArchNetwork::AddressFamily::INet);
    bool threw = false;
    try {
      listen.bind(loopback(port));
    } catch (SocketAddressInUseException &) {
      threw = true;
    }
    QVERIFY(threw);
  }
  const int after = openFdCount();
  QVERIFY2(
      after == before,
      qPrintable(QString("%1 fd(s) leaked over %2 failed epochs (before=%3 after=%4)")
                     .arg(after - before)
                     .arg(failedEpochs)
                     .arg(before)
                     .arg(after))
  );
  ::close(holder);
#endif
}

void TCPListenSocketTests::listenerTeardownLeavesNoFdsBehind()
{
#if defined(_WIN32)
  QSKIP("loopback fd tests are POSIX-only");
#else
  CountingEventQueue events;
  const int port = freeLoopbackPort();
  // Warm-up epoch (per-thread lazies), then measure across real epochs
  // that bind, accept a client and tear everything down.
  {
    SocketMultiplexer multiplexer;
    TCPListenSocket listen(&events, &multiplexer, IArchNetwork::AddressFamily::INet);
    listen.bind(loopback(port));
  }
  const int before = openFdCount();
  for (int epoch = 0; epoch < 10; ++epoch) {
    SocketMultiplexer multiplexer;
    TCPListenSocket listen(&events, &multiplexer, IArchNetwork::AddressFamily::INet);
    listen.bind(loopback(port));
    const int expected = events.connecting.load() + 1;
    const int client = connectLoopback(port);
    QVERIFY(client >= 0);
    QVERIFY(waitFor([&] { return events.connecting.load() >= expected; }));
    auto accepted = listen.accept();
    QVERIFY(accepted != nullptr);
    ::close(client);
    accepted.reset();
  }
  const int after = openFdCount();
  QVERIFY2(
      after == before, qPrintable(QString("%1 fd(s) leaked across 10 listener epochs").arg(after - before))
  );
  // The port is free again immediately: the next epoch can bind it.
  {
    SocketMultiplexer multiplexer;
    TCPListenSocket listen(&events, &multiplexer, IArchNetwork::AddressFamily::INet);
    listen.bind(loopback(port));
  }
#endif
}

QTEST_MAIN(TCPListenSocketTests)

#include "TCPListenSocketTests.moc"
