/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2014 - 2016 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ServerTests.h"

#include "../deskflow/MockKeyState.h"
#include "arch/Arch.h"
#include "base/EventQueue.h"
#include "base/Log.h"
#include "common/Settings.h"
#include "deskflow/PlatformScreen.h"
#include "deskflow/Screen.h"
#include "deskflow/ipc/CoreIpcServer.h"
#include "io/IStream.h"
#include "server/Config.h"
#include "server/ChordRemapTypes.h"
#include "server/PrimaryClient.h"
#include "server/Server.h"
#include "server/TopologyLink.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QTest>

#if defined(__APPLE__)
#include <Carbon/Carbon.h>
#endif

#include <memory>
#include <sstream>
#include <utility>
#include <vector>

namespace {

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

class TestPlatformScreen : public PlatformScreen
{
public:
  explicit TestPlatformScreen(IEventQueue *events) : PlatformScreen(events), m_keyState(*events)
  {
    // do nothing
  }

  void *getEventTarget() const override
  {
    return const_cast<TestPlatformScreen *>(this);
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
};

class TestClientProxy : public BaseClientProxy
{
public:
  explicit TestClientProxy(std::string name) : BaseClientProxy(std::move(name))
  {
  }

  void *getEventTarget() const override
  {
    return const_cast<TestClientProxy *>(this);
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

  void enter(int32_t x, int32_t y, uint32_t, KeyModifierMask, bool) override
  {
    m_enterCalls.emplace_back(x, y);
  }

  bool leave() override
  {
    return true;
  }

  void setClipboard(ClipboardID, const IClipboard *) override
  {
  }

  void grabClipboard(ClipboardID) override
  {
  }

  void setClipboardDirty(ClipboardID, bool) override
  {
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

  void clearEnterLog()
  {
    m_enterCalls.clear();
  }

  int enterCallCount() const
  {
    return static_cast<int>(m_enterCalls.size());
  }

  std::pair<int32_t, int32_t> lastEnterPos() const
  {
    return m_enterCalls.back();
  }

private:
  NullTestStream m_stream;
  std::vector<std::pair<int32_t, int32_t>> m_enterCalls;
};

struct RecordedKeyEvent
{
  enum class Kind
  {
    Down,
    Up,
    Repeat,
  };

  Kind kind = Kind::Down;
  KeyID id = kKeyNone;
  KeyModifierMask mask = 0;
};

class RecordingRemoteClient : public TestClientProxy
{
public:
  explicit RecordingRemoteClient(std::string name) : TestClientProxy(std::move(name))
  {
  }

  void keyDown(KeyID id, KeyModifierMask mask, KeyButton, const std::string &) override
  {
    m_keys.push_back({RecordedKeyEvent::Kind::Down, id, mask});
  }

  void keyRepeat(KeyID id, KeyModifierMask mask, int32_t, KeyButton, const std::string &) override
  {
    m_keys.push_back({RecordedKeyEvent::Kind::Repeat, id, mask});
  }

  void keyUp(KeyID id, KeyModifierMask mask, KeyButton) override
  {
    m_keys.push_back({RecordedKeyEvent::Kind::Up, id, mask});
  }

  const std::vector<RecordedKeyEvent> &keys() const
  {
    return m_keys;
  }

  void clearKeys()
  {
    m_keys.clear();
  }

private:
  std::vector<RecordedKeyEvent> m_keys;
};

void loadConfigWithSuperTabRemap(deskflow::server::Config &config)
{
  const std::string conf =
      "section: screens\n"
      "\tserver:\n"
      "\ttiny11:\n"
      "end\n\n"
      "section: links\n"
      "\tserver:\n"
      "\t\tright = tiny11\n"
      "end\n\n"
      "section: options\n"
      "end\n\n"
      "section: chordRemaps\n"
      "\ttiny11:\n"
      "\t\tchordRemap(Super+Tab) = Alt+Tab\n"
      "end\n\n";
  std::istringstream in(conf);
  deskflow::server::ConfigReadContext context(in);
  config.read(context);
}

struct LeakedServerFixture
{
  EventQueue events;
  deskflow::server::Config config;
  TestPlatformScreen *platform = nullptr;
  deskflow::Screen *screen = nullptr;
  PrimaryClient *primary = nullptr;

  explicit LeakedServerFixture() : config(&events)
  {
  }

  void init(const char *primaryName)
  {
    platform = new TestPlatformScreen(&events);
    screen = new deskflow::Screen(platform, &events);
    primary = new PrimaryClient(primaryName, screen);
  }
};

} // namespace

std::unique_ptr<Arch> g_arch;
Log g_log;
std::unique_ptr<QCoreApplication> g_app;
std::unique_ptr<deskflow::core::ipc::CoreIpcServer> g_ipc;
std::unique_ptr<QTemporaryDir> g_settingsDir;

void ServerTests::initTestCase()
{
  static int argc = 1;
  static char arg0[] = "ServerTests";
  static char *argv[] = {arg0, nullptr};
  g_app = std::make_unique<QCoreApplication>(argc, argv);
  g_ipc = std::make_unique<deskflow::core::ipc::CoreIpcServer>(g_app.get());
  g_arch = std::make_unique<Arch>();
  g_log.setFilter(LogLevel::Level::Error);
  // CRITICAL: isolate settings before any write. Without this the writes
  // below land in the developer's real ~/Library/Deskflow/Deskflow.conf,
  // renaming their machine to "server" and disabling the Mouser bridge on
  // every test/debug run.
  g_settingsDir = std::make_unique<QTemporaryDir>();
  QVERIFY(g_settingsDir->isValid());
  Settings::setSettingsFile(g_settingsDir->filePath(QStringLiteral("Deskflow.conf")));
  QVERIFY(Settings::settingsFile().startsWith(g_settingsDir->path()));
  Settings::setValue(Settings::Server::MouserBridgeEnabled, false);
  Settings::setValue(Settings::Core::ComputerName, QStringLiteral("server"));
}

void ServerTests::cleanupTestCase()
{
  g_ipc.reset();
  g_app.reset();
  g_arch.reset();
  g_settingsDir.reset();
}

void ServerTests::SwitchToScreenInfo_alloc_screen()
{
  auto actual = new Server::SwitchToScreenInfo("test");
  QCOMPARE(actual->m_screen, "test");
  delete actual;
}

void ServerTests::KeyboardBroadcastInfo_alloc_stateAndSceens()
{
  auto info = new Server::KeyboardBroadcastInfo(Server::KeyboardBroadcastInfo::State::kOn, "test");
  QCOMPARE(info->m_state, Server::KeyboardBroadcastInfo::State::kOn);
  QCOMPARE(info->m_screens, "test");
  delete info;
}

void ServerTests::adoptClient_resyncsEnterWhenActiveMatches()
{
  LeakedServerFixture fixture;
  QVERIFY(fixture.config.addScreen("server"));
  QVERIFY(fixture.config.addScreen("remote"));
  QVERIFY(fixture.config.connect("server", Direction::Right, 0.0f, 1.0f, "remote", 0.0f, 1.0f));
  QVERIFY(fixture.config.connect("remote", Direction::Left, 0.0f, 1.0f, "server", 0.0f, 1.0f));
  fixture.init("server");
  TestClientProxy remote("remote");

  {
    Server server(fixture.config, fixture.primary, fixture.screen, &fixture.events);
    QVERIFY(server.m_clients.emplace("remote", &remote).second);
    server.switchScreen(&remote, 50, 60, false);
    remote.clearEnterLog();

    server.resyncEnterIfActiveClient(&remote);

    QCOMPARE(remote.enterCallCount(), 1);
    QCOMPARE(remote.lastEnterPos().first, 50);
    QCOMPARE(remote.lastEnterPos().second, 60);
    server.m_clients.erase("remote");
  }
}

void ServerTests::peekConfiguredNeighbor_returnsLinkedScreen()
{
  LeakedServerFixture fixture;
  QVERIFY(fixture.config.addScreen("server"));
  QVERIFY(fixture.config.addScreen("remote"));
  QVERIFY(fixture.config.connect("server", Direction::Right, 0.0f, 1.0f, "remote", 0.0f, 1.0f));
  fixture.init("server");

  {
    Server server(fixture.config, fixture.primary, fixture.screen, &fixture.events);
    int32_t x = 1023;
    int32_t y = 384;
    QCOMPARE(server.peekConfiguredNeighbor(fixture.primary, Direction::Right, x, y), "remote");
  }
}

void ServerTests::peekConfiguredNeighbor_usesFleetTopology()
{
  LeakedServerFixture fixture;
  QVERIFY(fixture.config.addScreen("server"));
  QVERIFY(fixture.config.addScreen("remote"));
  fixture.init("server");

  {
    Server server(fixture.config, fixture.primary, fixture.screen, &fixture.events);
    server.setFleetTopologySource(true);
    server.setFleetTopologyLinks({deskflow::server::TopologyLink{"server", "remote", Direction::Right}});

    int32_t x = 1023;
    int32_t y = 384;
    QCOMPARE(server.peekConfiguredNeighbor(fixture.primary, Direction::Right, x, y), "remote");
    QCOMPARE(server.peekConfiguredNeighbor(fixture.primary, Direction::Left, x, y), std::string());
  }
}

void ServerTests::queuedSwitch_executesWhenNeighborConnects()
{
  LeakedServerFixture fixture;
  QVERIFY(fixture.config.addScreen("server"));
  QVERIFY(fixture.config.addScreen("remote"));
  QVERIFY(fixture.config.connect("server", Direction::Right, 0.0f, 1.0f, "remote", 0.0f, 1.0f));
  fixture.init("server");
  TestClientProxy remote("remote");

  {
    Server server(fixture.config, fixture.primary, fixture.screen, &fixture.events);
    server.switchScreen(fixture.primary, 512, 384, false);
    server.queueSwitchForScreen("remote", Direction::Right, 1020, 384);
    QVERIFY(server.m_clients.emplace("remote", &remote).second);
    QCOMPARE(server.m_active, fixture.primary);

    server.tryExecuteQueuedSwitch(&remote);

    QCOMPARE(server.m_active, &remote);
    server.m_clients.erase("remote");
  }
}

void ServerTests::requestWakePeer_postsEventOncePerThrottleWindow()
{
  LeakedServerFixture fixture;
  QVERIFY(fixture.config.addScreen("server"));
  fixture.init("server");

  {
    Server server(fixture.config, fixture.primary, fixture.screen, &fixture.events);

    int wakeEvents = 0;
    fixture.events.addHandler(EventTypes::ServerWakePeerRequested, &server, [&wakeEvents](const Event &event) {
      const auto *info = dynamic_cast<const Server::SwitchToScreenInfo *>(event.getDataObject());
      QVERIFY(info != nullptr);
      QCOMPARE(info->m_screen, std::string("remote"));
      ++wakeEvents;
    });

    // Pushing the cursor at a dead edge calls this per mouse move; only
    // the first request inside the 1 s throttle window posts an event.
    server.requestWakePeer("remote");
    server.requestWakePeer("remote");
    server.requestWakePeer("remote");

    // Drain the queue: posted events sit pending until a loop runs.
    fixture.events.addEvent(Event(EventTypes::Quit));
    fixture.events.loop();

    QCOMPARE(wakeEvents, 1);
    fixture.events.removeHandler(EventTypes::ServerWakePeerRequested, &server);
  }
}

void ServerTests::requestWakePeer_refiresAfterThrottleWindow()
{
  LeakedServerFixture fixture;
  QVERIFY(fixture.config.addScreen("server"));
  fixture.init("server");

  {
    Server server(fixture.config, fixture.primary, fixture.screen, &fixture.events);

    int wakeEvents = 0;
    fixture.events.addHandler(EventTypes::ServerWakePeerRequested, &server, [&wakeEvents](const Event &) {
      ++wakeEvents;
    });

    server.requestWakePeer("remote");
    // Simulate the 1 s window expiring; the throttle must re-arm or a
    // sleeping peer is never woken again after the first attempt.
    server.m_lastWakeRequest = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    server.requestWakePeer("remote");

    fixture.events.addEvent(Event(EventTypes::Quit));
    fixture.events.loop();

    QCOMPARE(wakeEvents, 2);
    fixture.events.removeHandler(EventTypes::ServerWakePeerRequested, &server);
  }
}

void ServerTests::fiveEsc_requestsLocalCoreRestartAndSwallows()
{
  LeakedServerFixture fixture;
  QVERIFY(fixture.config.addScreen("server"));
  QVERIFY(fixture.config.addScreen("remote"));
  QVERIFY(fixture.config.connect("server", Direction::Right, 0.0f, 1.0f, "remote", 0.0f, 1.0f));
  fixture.init("server");
  RecordingRemoteClient remote("remote");

  {
    Server server(fixture.config, fixture.primary, fixture.screen, &fixture.events);
    QVERIFY(server.m_clients.emplace("remote", &remote).second);
    server.switchScreen(&remote, 50, 60, false);
    QCOMPARE(server.m_active, &remote);

    int restartCalls = 0;
    server.m_localCoreRestartHook = [&restartCalls] { ++restartCalls; };

    for (int i = 0; i < deskflow::coordination::EscTapRescue::kTaps - 1; ++i) {
      server.onKeyDown(kKeyEscape, 0, 1, "en", nullptr);
      QCOMPARE(restartCalls, 0);
    }
    QCOMPARE(remote.keys().size(), static_cast<size_t>(deskflow::coordination::EscTapRescue::kTaps - 1));
    for (const auto &key : remote.keys()) {
      QCOMPARE(key.kind, RecordedKeyEvent::Kind::Down);
      QCOMPARE(key.id, kKeyEscape);
    }
    remote.clearKeys();
    server.onKeyDown(kKeyEscape, 0, 1, "en", nullptr);
    QCOMPARE(restartCalls, 1);
    QVERIFY(remote.keys().empty());
    QCOMPARE(server.m_active, &remote);

    server.m_clients.erase("remote");
  }
}

void ServerTests::chordRemapHoldThrough_superTabKeepsAltUntilSuperUp()
{
  LeakedServerFixture fixture;
  loadConfigWithSuperTabRemap(fixture.config);
  fixture.init("server");
  RecordingRemoteClient remote("tiny11");

  {
    Server server(fixture.config, fixture.primary, fixture.screen, &fixture.events);
    QVERIFY(server.m_clients.emplace("tiny11", &remote).second);
    server.switchScreen(&remote, 50, 60, false);
    QCOMPARE(server.m_active, &remote);

    server.onKeyDown(kKeyTab, KeyModifierSuper, 0, "en", nullptr);
    QCOMPARE(remote.keys().size(), 2u);
    QCOMPARE(remote.keys()[0].kind, RecordedKeyEvent::Kind::Down);
    QCOMPARE(remote.keys()[0].id, kKeySetModifiers);
    QCOMPARE(remote.keys()[0].mask, KeyModifierAlt);
    QCOMPARE(remote.keys()[1].id, kKeyTab);
    QCOMPARE(remote.keys()[1].mask, KeyModifierAlt);
    QVERIFY(server.m_chordRemapSession.active);

    remote.clearKeys();
    server.onKeyUp(kKeyTab, KeyModifierSuper, 0, nullptr);
    QCOMPARE(remote.keys().size(), 1u);
    QCOMPARE(remote.keys()[0].kind, RecordedKeyEvent::Kind::Up);
    QCOMPARE(remote.keys()[0].id, kKeyTab);
    QCOMPARE(remote.keys()[0].mask, KeyModifierAlt);
    QVERIFY(server.m_chordRemapSession.active);

    remote.clearKeys();
    server.onKeyUp(kKeySuper_L, 0, 0, nullptr);
    QCOMPARE(remote.keys().size(), 2u);
    QCOMPARE(remote.keys()[0].id, kKeyClearModifiers);
    QCOMPARE(remote.keys()[0].mask, KeyModifierAlt);
    QCOMPARE(remote.keys()[1].kind, RecordedKeyEvent::Kind::Up);
    QCOMPARE(remote.keys()[1].id, kKeySuper_L);
    QVERIFY(!server.m_chordRemapSession.active);

    server.m_clients.erase("tiny11");
  }
}

void ServerTests::chordRemapHoldThrough_relaySuperUpClearsSession()
{
#if defined(__APPLE__)
  LeakedServerFixture fixture;
  loadConfigWithSuperTabRemap(fixture.config);
  fixture.init("server");
  RecordingRemoteClient remote("tiny11");

  {
    Server server(fixture.config, fixture.primary, fixture.screen, &fixture.events);
    QVERIFY(server.m_clients.emplace("tiny11", &remote).second);
    server.switchScreen(&remote, 50, 60, false);

    server.onKeyDown(kKeyTab, KeyModifierSuper, 0, "en", nullptr);
    QVERIFY(server.m_chordRemapSession.active);
    remote.clearKeys();

    // Fleet keyboard relay clears KeyID on key-up; modifier release is in button.
    server.onKeyUp(kKeyNone, 0, static_cast<KeyButton>(kVK_Command), nullptr);
    QCOMPARE(remote.keys().size(), 2u);
    QCOMPARE(remote.keys()[0].id, kKeyClearModifiers);
    QCOMPARE(remote.keys()[0].mask, KeyModifierAlt);
    QCOMPARE(remote.keys()[1].kind, RecordedKeyEvent::Kind::Up);
    QCOMPARE(remote.keys()[1].id, kKeyNone);
    QVERIFY(!server.m_chordRemapSession.active);

    server.m_clients.erase("tiny11");
  }
#else
  QSKIP("Relay modifier key-up uses platform virtual-key codes");
#endif
}

void ServerTests::chordRemapHoldThrough_tabRepeatKeepsAltMask()
{
  LeakedServerFixture fixture;
  loadConfigWithSuperTabRemap(fixture.config);
  fixture.init("server");
  RecordingRemoteClient remote("tiny11");

  {
    Server server(fixture.config, fixture.primary, fixture.screen, &fixture.events);
    QVERIFY(server.m_clients.emplace("tiny11", &remote).second);
    server.switchScreen(&remote, 50, 60, false);

    server.onKeyDown(kKeyTab, KeyModifierSuper, 0, "en", nullptr);
    remote.clearKeys();

    server.onKeyRepeat(kKeyTab, KeyModifierSuper, 1, 0, "en");
    QCOMPARE(remote.keys().size(), 1u);
    QCOMPARE(remote.keys()[0].kind, RecordedKeyEvent::Kind::Repeat);
    QCOMPARE(remote.keys()[0].id, kKeyTab);
    QCOMPARE(remote.keys()[0].mask, KeyModifierAlt);
    QVERIFY(server.m_chordRemapSession.active);

    server.m_clients.erase("tiny11");
  }
}

void ServerTests::chordRemapHoldThrough_fiveEscCancelsSession()
{
  LeakedServerFixture fixture;
  loadConfigWithSuperTabRemap(fixture.config);
  fixture.init("server");
  RecordingRemoteClient remote("tiny11");

  {
    Server server(fixture.config, fixture.primary, fixture.screen, &fixture.events);
    QVERIFY(server.m_clients.emplace("tiny11", &remote).second);
    server.switchScreen(&remote, 50, 60, false);

    server.onKeyDown(kKeyTab, KeyModifierSuper, 0, "en", nullptr);
    QVERIFY(server.m_chordRemapSession.active);

    int restartCalls = 0;
    server.m_localCoreRestartHook = [&restartCalls] { ++restartCalls; };

    for (int i = 0; i < 4; ++i) {
      server.onKeyDown(kKeyEscape, 0, 0, "en", nullptr);
    }
    remote.clearKeys();
    server.onKeyDown(kKeyEscape, 0, 0, "en", nullptr);

    QCOMPARE(restartCalls, 1);
    QVERIFY(!server.m_chordRemapSession.active);
    QCOMPARE(remote.keys().size(), 1u);
    QCOMPARE(remote.keys()[0].id, kKeyClearModifiers);
    QCOMPARE(remote.keys()[0].mask, KeyModifierAlt);
    QCOMPARE(server.m_active, &remote);

    server.m_clients.erase("tiny11");
  }
}

void ServerTests::chordRemapHoldThrough_cancelsOnScreenSwitch()
{
  LeakedServerFixture fixture;
  loadConfigWithSuperTabRemap(fixture.config);
  fixture.init("server");
  RecordingRemoteClient remote("tiny11");

  {
    Server server(fixture.config, fixture.primary, fixture.screen, &fixture.events);
    QVERIFY(server.m_clients.emplace("tiny11", &remote).second);
    server.switchScreen(&remote, 50, 60, false);

    server.onKeyDown(kKeyTab, KeyModifierSuper, 0, "en", nullptr);
    QVERIFY(server.m_chordRemapSession.active);
    remote.clearKeys();

    server.switchScreen(fixture.primary, 100, 200, false);
    QCOMPARE(server.m_active, fixture.primary);
    QVERIFY(!server.m_chordRemapSession.active);
    QCOMPARE(remote.keys().size(), 1u);
    QCOMPARE(remote.keys()[0].id, kKeyClearModifiers);
    QCOMPARE(remote.keys()[0].mask, KeyModifierAlt);

    server.m_clients.erase("tiny11");
  }
}

void ServerTests::chordRemapPendingClear_flushesOnReconnect()
{
  LeakedServerFixture fixture;
  loadConfigWithSuperTabRemap(fixture.config);
  fixture.init("server");
  RecordingRemoteClient remote("tiny11");

  {
    Server server(fixture.config, fixture.primary, fixture.screen, &fixture.events);
    QVERIFY(server.m_clients.emplace("tiny11", &remote).second);
    server.switchScreen(&remote, 50, 60, false);
    server.onKeyDown(kKeyTab, KeyModifierSuper, 0, "en", nullptr);
    QVERIFY(server.m_chordRemapSession.active);

    // TCP drop mid-chord: the clear can't reach the dying connection, so it
    // must be queued for the reconnect instead of silently lost.
    server.forceLeaveClient(&remote);
    QVERIFY(!server.m_chordRemapSession.active);
    QCOMPARE(server.m_pendingChordModClears.size(), static_cast<size_t>(1));
    QCOMPARE(server.m_pendingChordModClears.at("tiny11"), KeyModifierAlt);

    // Fresh connection for the same screen (adoptClient calls this): the
    // queued clear is delivered exactly once.
    RecordingRemoteClient reconnected("tiny11");
    server.flushPendingChordModClear(&reconnected);
    QCOMPARE(reconnected.keys().size(), 1u);
    QCOMPARE(reconnected.keys()[0].kind, RecordedKeyEvent::Kind::Down);
    QCOMPARE(reconnected.keys()[0].id, kKeyClearModifiers);
    QCOMPARE(reconnected.keys()[0].mask, KeyModifierAlt);
    QVERIFY(server.m_pendingChordModClears.empty());

    // Second flush is a no-op.
    reconnected.clearKeys();
    server.flushPendingChordModClear(&reconnected);
    QVERIFY(reconnected.keys().empty());

    // Casing mismatch: the pending key comes from the chord CONFIG's casing,
    // the flush looks up by canonical screen name -- the map must be
    // caseless like every other chord comparison.
    server.m_clients.emplace("tiny11", &remote);
    server.switchScreen(&remote, 50, 60, false);
    server.onKeyDown(kKeyTab, KeyModifierSuper, 0, "en", nullptr);
    QVERIFY(server.m_chordRemapSession.active);
    server.m_chordRemapSession.entry.screen = "TINY11";
    server.forceLeaveClient(&remote);
    QCOMPARE(server.m_pendingChordModClears.size(), static_cast<size_t>(1));
    reconnected.clearKeys();
    server.flushPendingChordModClear(&reconnected);
    QCOMPARE(reconnected.keys().size(), 1u);
    QCOMPARE(reconnected.keys()[0].id, kKeyClearModifiers);
    QVERIFY(server.m_pendingChordModClears.empty());

    server.m_clients.erase("tiny11");
  }
}

void ServerTests::chordRemapSession_clearsOnServerTeardown()
{
  LeakedServerFixture fixture;
  loadConfigWithSuperTabRemap(fixture.config);
  fixture.init("server");
  RecordingRemoteClient remote("tiny11");

  {
    Server server(fixture.config, fixture.primary, fixture.screen, &fixture.events);
    QVERIFY(server.m_clients.emplace("tiny11", &remote).second);
    server.switchScreen(&remote, 50, 60, false);
    server.onKeyDown(kKeyTab, KeyModifierSuper, 0, "en", nullptr);
    QVERIFY(server.m_chordRemapSession.active);
    remote.clearKeys();

    server.m_clients.erase("tiny11");
    // Server destroyed mid-chord (role flip / five-Esc restart of a server
    // core): the held out-mods must be released while the link is alive.
  }

  QVERIFY(!remote.keys().empty());
  QCOMPARE(remote.keys()[0].kind, RecordedKeyEvent::Kind::Down);
  QCOMPARE(remote.keys()[0].id, kKeyClearModifiers);
  QCOMPARE(remote.keys()[0].mask, KeyModifierAlt);
}

void ServerTests::chordRemapSecondChord_keepsSingleSlotSession()
{
  LeakedServerFixture fixture;
  loadConfigWithSuperTabRemap(fixture.config);
  fixture.init("server");
  RecordingRemoteClient remote("tiny11");

  {
    Server server(fixture.config, fixture.primary, fixture.screen, &fixture.events);
    QVERIFY(server.m_clients.emplace("tiny11", &remote).second);
    server.switchScreen(&remote, 50, 60, false);
    server.onKeyDown(kKeyTab, KeyModifierSuper, 0, "en", nullptr);
    QVERIFY(server.m_chordRemapSession.active);
    remote.clearKeys();

    // A second matching chord while a session is active must not restart the
    // session or emit another kKeySetModifiers (single-slot semantics).
    server.onKeyDown(kKeyTab, KeyModifierSuper, 0, "en", nullptr);
    QCOMPARE(remote.keys().size(), 1u);
    QCOMPARE(remote.keys()[0].kind, RecordedKeyEvent::Kind::Down);
    QCOMPARE(remote.keys()[0].id, kKeyTab);
    QCOMPARE(remote.keys()[0].mask, KeyModifierAlt);
    QVERIFY(server.m_chordRemapSession.active);
    QCOMPARE(server.m_chordRemapSession.heldOutMods, KeyModifierAlt);

    server.m_clients.erase("tiny11");
  }
}

void ServerTests::fleetWalk_skipsDisconnectedScreens()
{
  LeakedServerFixture fixture;
  QVERIFY(fixture.config.addScreen("server"));
  fixture.init("server");
  TestClientProxy far("far");

  {
    Server server(fixture.config, fixture.primary, fixture.screen, &fixture.events);
    server.setFleetTopologySource(true);
    // server -> middle -> far; middle never connects and must be hopped.
    server.setFleetTopologyLinks({
        deskflow::server::TopologyLink{"server", "middle", Direction::Right},
        deskflow::server::TopologyLink{"middle", "far", Direction::Right},
    });
    QVERIFY(server.m_clients.emplace("far", &far).second);

    int32_t x = 1023;
    int32_t y = 384;
    QCOMPARE(server.getNeighbor(fixture.primary, Direction::Right, x, y), &far);
    server.m_clients.erase("far");
  }
}

void ServerTests::fleetWalk_cyclicLinksTerminate()
{
  LeakedServerFixture fixture;
  QVERIFY(fixture.config.addScreen("server"));
  fixture.init("server");

  {
    Server server(fixture.config, fixture.primary, fixture.screen, &fixture.events);
    server.setFleetTopologySource(true);
    // server -> ghostA -> ghostB -> ghostA...: neither ghost is connected;
    // the maxHops bound must terminate the walk with no neighbor.
    server.setFleetTopologyLinks({
        deskflow::server::TopologyLink{"server", "ghostA", Direction::Right},
        deskflow::server::TopologyLink{"ghostA", "ghostB", Direction::Right},
        deskflow::server::TopologyLink{"ghostB", "ghostA", Direction::Right},
    });

    int32_t x = 1023;
    int32_t y = 384;
    QCOMPARE(server.getNeighbor(fixture.primary, Direction::Right, x, y), nullptr);
  }
}

QTEST_MAIN(ServerTests)
