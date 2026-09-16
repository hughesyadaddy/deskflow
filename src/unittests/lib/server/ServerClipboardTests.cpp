/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

// Hotspot D4-server-clipboard-copies: the server must hold exactly one copy
// of each clipboard, a screen switch must not re-marshal (copy) it to check
// its size, and an unchanged clipboard must not be rebroadcast.

#include "../../deskflow/MockKeyState.h"
#include "arch/Arch.h"
#include "base/EventQueue.h"
#include "base/Log.h"
#include "common/Settings.h"
#include "deskflow/Clipboard.h"
#include "deskflow/PlatformScreen.h"
#include "deskflow/Screen.h"
#include "deskflow/ipc/CoreIpcServer.h"
#include "io/IStream.h"
#include "server/BaseClientProxy.h"
#include "server/Config.h"
#include "server/PrimaryClient.h"
#include "server/Server.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(_WIN32)
#include <malloc.h>
#else
#include <malloc.h>
#endif

//
// Byte-accounting allocator.  Replacing the global operator new in the test
// executable lets the tests measure how many bytes the server allocates for
// a clipboard update and a screen switch, which is the whole point of D4.
//

namespace allocprobe {

std::atomic<uint64_t> cumulativeBytes{0}; // bytes requested, never decremented
std::atomic<int64_t> liveBytes{0};        // usable bytes currently allocated

size_t usableSize(void *p)
{
#if defined(__APPLE__)
  return malloc_size(p);
#elif defined(_WIN32)
  return _msize(p);
#else
  return malloc_usable_size(p);
#endif
}

void *allocate(std::size_t n)
{
  void *p = std::malloc(n == 0 ? 1 : n);
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  cumulativeBytes.fetch_add(n, std::memory_order_relaxed);
  liveBytes.fetch_add(static_cast<int64_t>(usableSize(p)), std::memory_order_relaxed);
  return p;
}

void release(void *p) noexcept
{
  if (p == nullptr) {
    return;
  }
  liveBytes.fetch_sub(static_cast<int64_t>(usableSize(p)), std::memory_order_relaxed);
  std::free(p);
}

} // namespace allocprobe

void *operator new(std::size_t n)
{
  return allocprobe::allocate(n);
}
void *operator new[](std::size_t n)
{
  return allocprobe::allocate(n);
}
void *operator new(std::size_t n, const std::nothrow_t &) noexcept
{
  try {
    return allocprobe::allocate(n);
  } catch (...) {
    return nullptr;
  }
}
void *operator new[](std::size_t n, const std::nothrow_t &) noexcept
{
  try {
    return allocprobe::allocate(n);
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void *p) noexcept
{
  allocprobe::release(p);
}
void operator delete[](void *p) noexcept
{
  allocprobe::release(p);
}
void operator delete(void *p, std::size_t) noexcept
{
  allocprobe::release(p);
}
void operator delete[](void *p, std::size_t) noexcept
{
  allocprobe::release(p);
}
void operator delete(void *p, const std::nothrow_t &) noexcept
{
  allocprobe::release(p);
}
void operator delete[](void *p, const std::nothrow_t &) noexcept
{
  allocprobe::release(p);
}

namespace {

constexpr size_t kPayloadBytes = 1024 * 1024; // 1 MB

class NullTestStream : public deskflow::IStream
{
public:
  void close() override
  {
  }
  uint32_t read(void *, uint32_t) override
  {
    return 0;
  }
  void write(const void *, uint32_t) override
  {
  }
  void flush() override
  {
  }
  void shutdownInput() override
  {
  }
  void shutdownOutput() override
  {
  }
  void *getEventTarget() const override
  {
    return nullptr;
  }
  bool isReady() const override
  {
    return false;
  }
  uint32_t getSize() const override
  {
    return 0;
  }
};

// Primary screen whose clipboard contents the test controls.
class ClipboardPlatformScreen : public PlatformScreen
{
public:
  explicit ClipboardPlatformScreen(IEventQueue *events) : PlatformScreen(events), m_keyState(*events)
  {
  }

  void setText(ClipboardID id, const std::string &text)
  {
    Clipboard &clip = m_clipboards[id];
    if (clip.open(0)) {
      clip.empty();
      clip.add(IClipboard::Format::Text, text);
      clip.close();
    }
  }

  void *getEventTarget() const override
  {
    return const_cast<ClipboardPlatformScreen *>(this);
  }

  bool getClipboard(ClipboardID id, IClipboard *dst) const override
  {
    return Clipboard::copy(dst, &m_clipboards[id]);
  }

  void getShape(int32_t &x, int32_t &y, int32_t &width, int32_t &height) const override
  {
    x = 0;
    y = 0;
    width = 1024;
    height = 768;
  }

  void getCursorPos(int32_t &x, int32_t &y) const override
  {
    x = 512;
    y = 384;
  }

  void reconfigure(uint32_t) override
  {
  }
  uint32_t activeSides() override
  {
    return 0;
  }
  void warpCursor(int32_t, int32_t) override
  {
  }
  uint32_t registerHotKey(KeyID, KeyModifierMask) override
  {
    return 0;
  }
  void unregisterHotKey(uint32_t) override
  {
  }
  void fakeInputBegin() override
  {
  }
  void fakeInputEnd() override
  {
  }
  int32_t getJumpZoneSize() const override
  {
    return 0;
  }
  bool isAnyMouseButtonDown(uint32_t &) const override
  {
    return false;
  }
  void getCursorCenter(int32_t &x, int32_t &y) const override
  {
    x = 512;
    y = 384;
  }
  void fakeMouseButton(ButtonID, bool) override
  {
  }
  void fakeMouseMove(int32_t, int32_t) override
  {
  }
  void fakeMouseRelativeMove(int32_t, int32_t) const override
  {
  }
  void fakeMouseWheel(ScrollDelta) const override
  {
  }
  void enable() override
  {
  }
  void disable() override
  {
  }
  void enter() override
  {
  }
  bool canLeave() override
  {
    return true;
  }
  void leave() override
  {
  }
  bool setClipboard(ClipboardID, const IClipboard *) override
  {
    return false;
  }
  void checkClipboards() override
  {
  }
  void openScreensaver(bool) override
  {
  }
  void closeScreensaver() override
  {
  }
  void screensaver(bool) override
  {
  }
  void resetOptions() override
  {
  }
  void setOptions(const OptionsList &) override
  {
  }
  void setSequenceNumber(uint32_t) override
  {
  }
  std::string getSecureInputApp() const override
  {
    return {};
  }
  bool isPrimary() const override
  {
    return true;
  }

protected:
  void updateButtons() override
  {
  }
  IKeyState *getKeyState() const override
  {
    return const_cast<MockKeyState *>(&m_keyState);
  }
  void handleSystemEvent(const Event &) override
  {
  }

private:
  MockKeyState m_keyState;
  Clipboard m_clipboards[kClipboardEnd];
};

// Remote proxy that records what the server pushes to it and never copies.
class RecordingProxy : public BaseClientProxy
{
public:
  explicit RecordingProxy(std::string name) : BaseClientProxy(std::move(name))
  {
  }

  void *getEventTarget() const override
  {
    return const_cast<RecordingProxy *>(this);
  }
  bool getClipboard(ClipboardID, IClipboard *) const override
  {
    return false;
  }
  void getShape(int32_t &x, int32_t &y, int32_t &width, int32_t &height) const override
  {
    x = 0;
    y = 0;
    width = 1024;
    height = 768;
  }
  void getCursorPos(int32_t &x, int32_t &y) const override
  {
    x = 100;
    y = 200;
  }
  void enter(int32_t, int32_t, uint32_t, KeyModifierMask, bool) override
  {
  }
  bool leave() override
  {
    return true;
  }
  void setClipboard(ClipboardID id, const IClipboard *) override
  {
    ++m_setClipboard[id];
  }
  void grabClipboard(ClipboardID id) override
  {
    ++m_grabClipboard[id];
  }
  void setClipboardDirty(ClipboardID id, bool dirty) override
  {
    if (dirty) {
      ++m_markedDirty[id];
    }
  }
  void keyDown(KeyID, KeyModifierMask, KeyButton, const std::string &) override
  {
  }
  void keyRepeat(KeyID, KeyModifierMask, int32_t, KeyButton, const std::string &) override
  {
  }
  void keyUp(KeyID, KeyModifierMask, KeyButton) override
  {
  }
  void mouseDown(ButtonID) override
  {
  }
  void mouseUp(ButtonID) override
  {
  }
  void mouseMove(int32_t, int32_t) override
  {
  }
  void mouseRelativeMove(int32_t, int32_t) override
  {
  }
  void mouseWheel(int32_t, int32_t) override
  {
  }
  void screensaver(bool) override
  {
  }
  void resetOptions() override
  {
  }
  void setOptions(const OptionsList &) override
  {
  }
  void sendDragInfo(uint32_t, const char *, size_t) override
  {
  }
  void fileChunkSending(uint8_t, char *, size_t) override
  {
  }
  std::string getSecureInputApp() const override
  {
    return {};
  }
  void secureInputNotification(const std::string &) const override
  {
  }
  deskflow::IStream *getStream() const override
  {
    return const_cast<NullTestStream *>(&m_stream);
  }

  int m_setClipboard[kClipboardEnd] = {0, 0};
  int m_grabClipboard[kClipboardEnd] = {0, 0};
  int m_markedDirty[kClipboardEnd] = {0, 0};

private:
  NullTestStream m_stream;
};

// Mirrors LeakedServerFixture in ServerTests: the Server takes ownership of
// the primary client / screen and deletes them, so these are raw pointers.
struct Fixture
{
  EventQueue events;
  deskflow::server::Config config;
  ClipboardPlatformScreen *platform = nullptr;
  deskflow::Screen *screen = nullptr;
  PrimaryClient *primary = nullptr;

  Fixture() : config(&events)
  {
    config.addScreen("server");
    config.addScreen("remoteA");
    config.addScreen("remoteB");
    config.connect("server", Direction::Right, 0.0f, 1.0f, "remoteA", 0.0f, 1.0f);
    config.connect("remoteA", Direction::Left, 0.0f, 1.0f, "server", 0.0f, 1.0f);
    config.connect("remoteA", Direction::Right, 0.0f, 1.0f, "remoteB", 0.0f, 1.0f);
    config.connect("remoteB", Direction::Left, 0.0f, 1.0f, "remoteA", 0.0f, 1.0f);
    platform = new ClipboardPlatformScreen(&events);
    screen = new deskflow::Screen(platform, &events);
    primary = new PrimaryClient("server", screen);
  }
};

std::string makePayload(size_t bytes, char seed)
{
  std::string s(bytes, seed);
  for (size_t i = 0; i < bytes; i += 251) {
    s[i] = static_cast<char>('a' + (i % 26));
  }
  return s;
}

size_t marshalledSize(const std::string &text)
{
  // IClipboard::marshall(): 4-byte count + (4-byte id + 4-byte len + data)
  return 4 + 4 + 4 + text.size();
}

} // namespace

class ServerClipboardTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();
  void fingerprint_matchesMarshallAndSurvivesRoundTrip();
  void setClipboard_1MB_holdsExactlyOneCopy();
  void switchScreen_doesNotMarshalOrCopyClipboard();
  void unchangedClipboard_isNotRebroadcast();
  void oversizedClipboard_isNotSentOnSwitch();

private:
  std::unique_ptr<Arch> m_arch;
  Log m_log;
  std::unique_ptr<QCoreApplication> m_app;
  std::unique_ptr<deskflow::core::ipc::CoreIpcServer> m_ipc;
  std::unique_ptr<QTemporaryDir> m_settingsDir;
};

void ServerClipboardTests::initTestCase()
{
  static int argc = 1;
  static char arg0[] = "ServerClipboardTests";
  static char *argv[] = {arg0, nullptr};
  m_app = std::make_unique<QCoreApplication>(argc, argv);
  m_ipc = std::make_unique<deskflow::core::ipc::CoreIpcServer>(m_app.get());
  m_arch = std::make_unique<Arch>();
  m_log.setFilter(LogLevel::Level::Error);
  // isolate settings so nothing lands in the developer's real Deskflow.conf
  m_settingsDir = std::make_unique<QTemporaryDir>();
  QVERIFY(m_settingsDir->isValid());
  Settings::setSettingsFile(m_settingsDir->filePath(QStringLiteral("Deskflow.conf")));
  QVERIFY(Settings::settingsFile().startsWith(m_settingsDir->path()));
  Settings::setValue(Settings::Server::MouserBridgeEnabled, false);
  Settings::setValue(Settings::Core::ComputerName, QStringLiteral("server"));
}

void ServerClipboardTests::cleanupTestCase()
{
  m_ipc.reset();
  m_app.reset();
  m_arch.reset();
  m_settingsDir.reset();
}

void ServerClipboardTests::fingerprint_matchesMarshallAndSurvivesRoundTrip()
{
  Clipboard a;
  QVERIFY(a.open(0));
  a.empty();
  a.add(IClipboard::Format::Text, "hello");
  a.add(IClipboard::Format::Bitmap, std::string(4096, '\x7f'));
  a.close();

  const auto fpA = Server::fingerprintClipboard(a);
  QCOMPARE(fpA.m_size, a.marshall().size());

  Clipboard b;
  b.unmarshall(a.marshall(), 0);
  QVERIFY(Server::fingerprintClipboard(b) == fpA);

  Clipboard c;
  QVERIFY(c.open(0));
  c.empty();
  c.add(IClipboard::Format::Text, "hellO");
  c.add(IClipboard::Format::Bitmap, std::string(4096, '\x7f'));
  c.close();
  const auto fpC = Server::fingerprintClipboard(c);
  QCOMPARE(fpC.m_size, fpA.m_size);
  QVERIFY(fpC.m_hash != fpA.m_hash);

  Clipboard empty;
  const auto fpEmpty = Server::fingerprintClipboard(empty);
  QCOMPARE(fpEmpty.m_size, empty.marshall().size());
}

void ServerClipboardTests::setClipboard_1MB_holdsExactlyOneCopy()
{
  Fixture fixture;
  const std::string payload = makePayload(kPayloadBytes, 'x');
  fixture.platform->setText(kClipboardClipboard, payload);

  {
    Server server(fixture.config, fixture.primary, fixture.screen, &fixture.events);

    const auto scansBefore = Server::clipboardScanCountForTests();
    const auto liveBefore = allocprobe::liveBytes.load();

    server.onClipboardChanged(fixture.primary, kClipboardClipboard, 0);

    const auto liveDelta = allocprobe::liveBytes.load() - liveBefore;
    const auto scans = Server::clipboardScanCountForTests() - scansBefore;

    // one persistent copy of the payload on the server: not two (the old
    // Clipboard + marshalled-string pair) and not zero.
    QVERIFY2(liveDelta >= static_cast<int64_t>(kPayloadBytes), qPrintable(QString::number(liveDelta)));
    QVERIFY2(liveDelta < static_cast<int64_t>(kPayloadBytes + kPayloadBytes / 2), qPrintable(QString::number(liveDelta)));
    QCOMPARE(scans, 1u);

    const auto &info = server.m_clipboards[kClipboardClipboard];
    QCOMPARE(info.m_fingerprint.m_size, marshalledSize(payload));
    QCOMPARE(info.m_fingerprint, Server::fingerprintClipboard(info.m_clipboard));
  }
}

void ServerClipboardTests::switchScreen_doesNotMarshalOrCopyClipboard()
{
  Fixture fixture;
  fixture.platform->setText(kClipboardClipboard, makePayload(kPayloadBytes, 'y'));
  RecordingProxy remoteA("remoteA");
  RecordingProxy remoteB("remoteB");

  {
    Server server(fixture.config, fixture.primary, fixture.screen, &fixture.events);
    QVERIFY(server.m_clients.emplace("remoteA", &remoteA).second);
    QVERIFY(server.m_clients.emplace("remoteB", &remoteB).second);

    // leaving the primary legitimately re-reads its clipboard once; that is
    // protocol behaviour, not the hotspot.  measure the remote->remote hop.
    server.switchScreen(&remoteA, 50, 60, false);
    QCOMPARE(remoteA.m_setClipboard[kClipboardClipboard], 1);

    const auto scansBefore = Server::clipboardScanCountForTests();
    const auto bytesBefore = allocprobe::cumulativeBytes.load();

    server.switchScreen(&remoteB, 50, 60, false);

    const auto bytes = allocprobe::cumulativeBytes.load() - bytesBefore;
    QCOMPARE(Server::clipboardScanCountForTests() - scansBefore, 0u);
    // the switch still posts an event and logs, so allow a little slack, but
    // nothing on the order of the 1 MB clipboard may be allocated.
    QVERIFY2(bytes < 64 * 1024, qPrintable(QString::number(bytes)));
    QCOMPARE(remoteB.m_setClipboard[kClipboardClipboard], 1);

    server.m_clients.erase("remoteA");
    server.m_clients.erase("remoteB");
  }
}

void ServerClipboardTests::unchangedClipboard_isNotRebroadcast()
{
  Fixture fixture;
  fixture.platform->setText(kClipboardClipboard, makePayload(kPayloadBytes, 'z'));
  RecordingProxy remoteA("remoteA");

  {
    Server server(fixture.config, fixture.primary, fixture.screen, &fixture.events);
    QVERIFY(server.m_clients.emplace("remoteA", &remoteA).second);

    server.onClipboardChanged(fixture.primary, kClipboardClipboard, 0);
    QCOMPARE(remoteA.m_markedDirty[kClipboardClipboard], 1);

    server.switchScreen(&remoteA, 50, 60, false);
    QCOMPARE(remoteA.m_setClipboard[kClipboardClipboard], 1);

    // same bytes again: detected via fingerprint, no dirty marks, no resend
    server.onClipboardChanged(fixture.primary, kClipboardClipboard, 0);
    QCOMPARE(remoteA.m_markedDirty[kClipboardClipboard], 1);
    QCOMPARE(remoteA.m_setClipboard[kClipboardClipboard], 1);

    // different bytes of the same length: rebroadcast
    fixture.platform->setText(kClipboardClipboard, makePayload(kPayloadBytes, 'w'));
    server.onClipboardChanged(fixture.primary, kClipboardClipboard, 0);
    QCOMPARE(remoteA.m_markedDirty[kClipboardClipboard], 2);
    QCOMPARE(remoteA.m_setClipboard[kClipboardClipboard], 2);

    server.m_clients.erase("remoteA");
  }
}

void ServerClipboardTests::oversizedClipboard_isNotSentOnSwitch()
{
  Fixture fixture;
  fixture.platform->setText(kClipboardClipboard, makePayload(kPayloadBytes, 'v'));
  RecordingProxy remoteA("remoteA");

  {
    Server server(fixture.config, fixture.primary, fixture.screen, &fixture.events);
    server.m_maximumClipboardSize = 16; // KB
    QVERIFY(server.m_clients.emplace("remoteA", &remoteA).second);

    server.onClipboardChanged(fixture.primary, kClipboardClipboard, 0);
    QCOMPARE(remoteA.m_markedDirty[kClipboardClipboard], 0);
    // the cached size must still reflect the oversized contents
    QVERIFY(server.m_clipboards[kClipboardClipboard].m_fingerprint.m_size > 16 * 1024);

    server.switchScreen(&remoteA, 50, 60, false);
    QCOMPARE(remoteA.m_setClipboard[kClipboardClipboard], 0);
    // the (empty, tiny) selection clipboard is still delivered
    QCOMPARE(remoteA.m_setClipboard[kClipboardSelection], 1);

    server.m_clients.erase("remoteA");
  }
}

QTEST_APPLESS_MAIN(ServerClipboardTests)
#include "ServerClipboardTests.moc"
