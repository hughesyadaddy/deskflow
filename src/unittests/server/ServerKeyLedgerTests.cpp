/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ServerKeyLedgerTests.h"

#include "../deskflow/MockKeyState.h"
#include "arch/Arch.h"
#include "base/EventQueue.h"
#include "base/ILogOutputter.h"
#include "base/Log.h"
#include "common/Settings.h"
#include "coordination/KeyboardRescue.h"
#include "coordination/RelayKeyEvent.h"
#include "deskflow/PlatformScreen.h"
#include "deskflow/Screen.h"
#include "deskflow/ipc/CoreIpcServer.h"
#include "io/IStream.h"
#include "server/Config.h"
#include "server/PrimaryClient.h"
#include "server/Server.h"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <chrono>
#include <memory>
#include <string>
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

//! Primary platform screen with a scriptable OS lock state.
class TestPlatformScreen : public PlatformScreen
{
public:
  explicit TestPlatformScreen(IEventQueue *events) : PlatformScreen(events), m_keyState(*events)
  {
  }

  KeyModifierMask osModifiers = 0;
  int sanitizeCalls = 0; // full freshness sweep (boundary-only)
  int releaseCalls = 0;  // ledger-only release (enter/verifier)

  void *getEventTarget() const override
  {
    return const_cast<TestPlatformScreen *>(this);
  }
  void sanitizeInjectedKeys() override
  {
    ++sanitizeCalls;
    PlatformScreen::sanitizeInjectedKeys();
  }
  void releaseInjectedKeys() override
  {
    ++releaseCalls;
    PlatformScreen::releaseInjectedKeys();
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
  KeyModifierMask pollActiveModifiers() const override
  {
    return osModifiers;
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

//! The same platform screen in the client role.
class SecondaryPlatformScreen : public TestPlatformScreen
{
public:
  using TestPlatformScreen::TestPlatformScreen;

  bool isPrimary() const override
  {
    return false;
  }
};

//! Counts log lines containing a needle (the fleet-health grep contract).
//! Registers itself with the live Log on construction and unregisters on
//! destruction, so a failed assertion cannot leave a dangling outputter.
class NeedleOutputter : public ILogOutputter
{
public:
  explicit NeedleOutputter(QString needle) : m_needle(std::move(needle))
  {
    CLOG->insert(this);
  }
  ~NeedleOutputter() override
  {
    CLOG->remove(this);
  }
  void open(const QString &) override
  {
  }
  void close() override
  {
  }
  bool write(LogLevel::Level, const QString &message) override
  {
    if (message.contains(m_needle)) {
      ++hits;
    }
    // Log::output() stops the chain on FALSE (despite the interface doc):
    // keep passing the line on so the other needle and the console see it.
    return true;
  }
  int hits = 0;

private:
  QString m_needle;
};

//! Pump \p events until \p done() or \p seconds elapse (timers fire from
//! getEvent(); dispatchEvent() runs their handlers).
template <typename Done> void pumpUntil(EventQueue &events, double seconds, Done done)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    Event event;
    if (events.getEvent(event, 0.05)) {
      events.dispatchEvent(event);
    }
  }
}

struct RecordedKey
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
  KeyButton button = 0;
};

//! Remote client proxy that records every key and every enter/leave.
class RecordingClient : public BaseClientProxy
{
public:
  explicit RecordingClient(std::string name) : BaseClientProxy(std::move(name))
  {
  }

  std::vector<RecordedKey> keys;
  int enterCalls = 0;
  int leaveCalls = 0;
  //! keys.size() at the moment of each leave(): keys[i] with i below it
  //! went out before kMsgCLeave, at or above it after.
  std::vector<size_t> leaveAtKeyIndex;

  void *getEventTarget() const override
  {
    return const_cast<RecordingClient *>(this);
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
    ++enterCalls;
  }
  bool leave() override
  {
    ++leaveCalls;
    leaveAtKeyIndex.push_back(keys.size());
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
  void keyDown(KeyID id, KeyModifierMask mask, KeyButton button, const std::string &) override
  {
    keys.push_back({RecordedKey::Kind::Down, id, mask, button});
  }
  void keyRepeat(KeyID id, KeyModifierMask mask, int32_t, KeyButton button, const std::string &) override
  {
    keys.push_back({RecordedKey::Kind::Repeat, id, mask, button});
  }
  void keyUp(KeyID id, KeyModifierMask mask, KeyButton button) override
  {
    keys.push_back({RecordedKey::Kind::Up, id, mask, button});
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

  int count(RecordedKey::Kind kind, KeyID id) const
  {
    int n = 0;
    for (const auto &k : keys) {
      if (k.kind == kind && k.id == id) {
        ++n;
      }
    }
    return n;
  }

  const RecordedKey *findUp(KeyID id) const
  {
    for (const auto &k : keys) {
      if (k.kind == RecordedKey::Kind::Up && k.id == id) {
        return &k;
      }
    }
    return nullptr;
  }

private:
  NullTestStream m_stream;
};

//! Leaks its screen/primary on purpose: Server's destructor disables and
//! removes the primary and the real app never deletes these either.
struct Fixture
{
  EventQueue events;
  deskflow::server::Config config;
  TestPlatformScreen *platform = nullptr;
  deskflow::Screen *screen = nullptr;
  PrimaryClient *primary = nullptr;

  Fixture() : config(&events)
  {
  }

  void init(std::initializer_list<const char *> remotes)
  {
    QVERIFY(config.addScreen("server"));
    for (const char *name : remotes) {
      QVERIFY(config.addScreen(name));
    }
    platform = new TestPlatformScreen(&events);
    screen = new deskflow::Screen(platform, &events);
    primary = new PrimaryClient("server", screen);
  }
};

constexpr KeyButton kButtonA = 0x1E;
constexpr KeyButton kButtonShift = 0x2A;
constexpr KeyButton kButtonCaps = 0x3A;
constexpr KeyID kKeyA = static_cast<KeyID>('a');

} // namespace

std::unique_ptr<Arch> g_arch;
Log g_log;
std::unique_ptr<QCoreApplication> g_app;
std::unique_ptr<deskflow::core::ipc::CoreIpcServer> g_ipc;
std::unique_ptr<QTemporaryDir> g_settingsDir;

void ServerKeyLedgerTests::initTestCase()
{
  static int argc = 1;
  static char arg0[] = "ServerKeyLedgerTests";
  static char *argv[] = {arg0, nullptr};
  g_app = std::make_unique<QCoreApplication>(argc, argv);
  g_ipc = std::make_unique<deskflow::core::ipc::CoreIpcServer>(g_app.get());
  g_arch = std::make_unique<Arch>();
  g_log.setFilter(LogLevel::Level::Error);
  // isolate settings before any write (see ServerTests)
  g_settingsDir = std::make_unique<QTemporaryDir>();
  QVERIFY(g_settingsDir->isValid());
  Settings::setSettingsFile(g_settingsDir->filePath(QStringLiteral("Deskflow.conf")));
  QVERIFY(Settings::settingsFile().startsWith(g_settingsDir->path()));
  Settings::setValue(Settings::Server::MouserBridgeEnabled, false);
  Settings::setValue(Settings::Core::ComputerName, QStringLiteral("server"));
}

void ServerKeyLedgerTests::cleanupTestCase()
{
  g_ipc.reset();
  g_app.reset();
  g_arch.reset();
}

void ServerKeyLedgerTests::ledger_recordsEveryKeyAndReleasesOnSwitch()
{
  // I1: ordinary keys, not just modifiers, are held on the target and must
  // be released to the screen being ABANDONED, with mask 0.
  Fixture f;
  f.init({"remote"});
  RecordingClient remote("remote");
  {
    Server server(f.config, f.primary, f.screen, &f.events);
    QVERIFY(server.m_clients.emplace("remote", &remote).second);
    server.switchScreen(&remote, 50, 60, false);

    server.onKeyDown(kKeyA, 0, kButtonA, "en", nullptr);
    server.onKeyDown(kKeyShift_L, KeyModifierShift, kButtonShift, "en", nullptr);
    QCOMPARE(server.m_keysHeldOnActive.size(), 2u);
    QCOMPARE(server.m_keysHeldOnActive.at(kButtonA), kKeyA);
    remote.keys.clear();

    server.switchScreen(f.primary, 512, 384, false);

    QVERIFY(server.m_keysHeldOnActive.empty());
    const auto *upA = remote.findUp(kKeyA);
    QVERIFY(upA != nullptr);
    QCOMPARE(upA->button, kButtonA);
    QCOMPARE(upA->mask, static_cast<KeyModifierMask>(0));
    QVERIFY(remote.findUp(kKeyShift_L) != nullptr);
    // released BEFORE the leave-side cut-over: nothing lands on the primary
    QCOMPARE(remote.leaveCalls, 1);

    server.m_clients.erase("remote");
  }
}

void ServerKeyLedgerTests::ledger_keyUpForgetsEntry()
{
  Fixture f;
  f.init({"remote"});
  RecordingClient remote("remote");
  {
    Server server(f.config, f.primary, f.screen, &f.events);
    QVERIFY(server.m_clients.emplace("remote", &remote).second);
    server.switchScreen(&remote, 50, 60, false);

    server.onKeyDown(kKeyA, 0, kButtonA, "en", nullptr);
    server.onKeyUp(kKeyA, 0, kButtonA, nullptr);
    QVERIFY(server.m_keysHeldOnActive.empty());
    remote.keys.clear();

    // nothing left to release on the way out
    server.switchScreen(f.primary, 512, 384, false);
    QCOMPARE(remote.count(RecordedKey::Kind::Up, kKeyA), 0);

    server.m_clients.erase("remote");
  }
}

void ServerKeyLedgerTests::ledger_ignoresKeysSentToPrimary()
{
  // The primary is never a target: keys typed while it is active are not
  // "held" anywhere the server must release.
  Fixture f;
  f.init({"remote"});
  {
    Server server(f.config, f.primary, f.screen, &f.events);
    server.onKeyDown(kKeyA, 0, kButtonA, "en", nullptr);
    QVERIFY(server.m_keysHeldOnActive.empty());
  }
}

void ServerKeyLedgerTests::teardown_releasesLedgerAndSendsLeave()
{
  // Every epoch flip / role flip destroys the Server. The active client
  // must get its releases and a leave while the link is still alive.
  Fixture f;
  f.init({"remote"});
  RecordingClient remote("remote");
  {
    Server server(f.config, f.primary, f.screen, &f.events);
    QVERIFY(server.m_clients.emplace("remote", &remote).second);
    server.switchScreen(&remote, 50, 60, false);
    server.onKeyDown(kKeyA, 0, kButtonA, "en", nullptr);
    remote.keys.clear();
    remote.leaveCalls = 0;
    server.m_clients.erase("remote");
  }
  QVERIFY(remote.findUp(kKeyA) != nullptr);
  QCOMPARE(remote.leaveCalls, 1);
  // and no stray key traffic after the leave
  QCOMPARE(remote.keys.size(), 1u);
}

void ServerKeyLedgerTests::forceLeave_sendsBestEffortReleases()
{
  // A client leaving the config (or dying) still gets the releases sent:
  // a dead socket merely drops them, a live one is cleaned up.
  Fixture f;
  f.init({"remote"});
  RecordingClient remote("remote");
  {
    Server server(f.config, f.primary, f.screen, &f.events);
    QVERIFY(server.m_clients.emplace("remote", &remote).second);
    server.switchScreen(&remote, 50, 60, false);
    server.onKeyDown(kKeyA, 0, kButtonA, "en", nullptr);
    server.onKeyDown(kKeyShift_L, KeyModifierShift, kButtonShift, "en", nullptr);
    remote.keys.clear();

    server.forceLeaveClient(&remote);

    QVERIFY(server.m_keysHeldOnActive.empty());
    QVERIFY(remote.findUp(kKeyA) != nullptr);
    QVERIFY(remote.findUp(kKeyShift_L) != nullptr);
    QCOMPARE(server.m_active, f.primary);

    server.m_clients.erase("remote");
  }
}

void ServerKeyLedgerTests::escRescue_releasesLedger()
{
  Fixture f;
  f.init({"remote"});
  RecordingClient remote("remote");
  {
    Server server(f.config, f.primary, f.screen, &f.events);
    QVERIFY(server.m_clients.emplace("remote", &remote).second);
    server.switchScreen(&remote, 50, 60, false);
    int restarts = 0;
    server.m_localCoreRestartHook = [&restarts] { ++restarts; };

    // a key is stuck down on the target; the user mashes Esc
    server.onKeyDown(kKeyA, 0, kButtonA, "en", nullptr);
    for (int i = 0; i < deskflow::coordination::EscTapRescue::kTaps - 1; ++i) {
      server.onKeyDown(kKeyEscape, 0, 1, "en", nullptr);
      server.onKeyUp(kKeyEscape, 0, 1, nullptr);
    }
    remote.keys.clear();
    server.onKeyDown(kKeyEscape, 0, 1, "en", nullptr);

    QCOMPARE(restarts, 1);
    QVERIFY(server.m_keysHeldOnActive.empty());
    QVERIFY(remote.findUp(kKeyA) != nullptr);
    QCOMPARE(remote.count(RecordedKey::Kind::Down, kKeyEscape), 0);

    server.m_clients.erase("remote");
  }
}

void ServerKeyLedgerTests::broadcast_keysReleasedPerScreen()
{
  // Broadcast bypasses the active ledger; it keeps its own, per screen,
  // and a screen leaving gets only ITS releases.
  Fixture f;
  f.init({"one", "two"});
  RecordingClient one("one");
  RecordingClient two("two");
  {
    Server server(f.config, f.primary, f.screen, &f.events);
    QVERIFY(server.m_clients.emplace("one", &one).second);
    QVERIFY(server.m_clients.emplace("two", &two).second);
    server.m_keyboardBroadcasting = true;
    server.m_keyboardBroadcastingScreens = "*";

    server.onKeyDown(kKeyA, 0, kButtonA, "en", nullptr);
    QCOMPARE(one.count(RecordedKey::Kind::Down, kKeyA), 1);
    QCOMPARE(two.count(RecordedKey::Kind::Down, kKeyA), 1);
    QCOMPARE(server.m_keysHeldOnBroadcast.size(), 2u);
    QVERIFY(!server.m_keysHeldOnBroadcast.contains("server"));
    QVERIFY(server.m_keysHeldOnActive.empty());
    one.keys.clear();
    two.keys.clear();

    // "two" leaves the config: only "two" is released
    server.forceLeaveClient(&two);
    QVERIFY(two.findUp(kKeyA) != nullptr);
    QVERIFY(one.findUp(kKeyA) == nullptr);
    QCOMPARE(server.m_keysHeldOnBroadcast.size(), 1u);
    QVERIFY(server.m_keysHeldOnBroadcast.contains("one"));
    server.m_clients.erase("two");

    // a broadcast key up forgets the entry
    server.onKeyUp(kKeyA, 0, kButtonA, nullptr);
    QVERIFY(server.m_keysHeldOnBroadcast.at("one").empty());
    one.keys.clear();

    // and teardown releases whatever is still held on each remaining screen
    server.onKeyDown(kKeyShift_L, KeyModifierShift, kButtonShift, "en", nullptr);
    one.keys.clear();
    server.m_clients.erase("one");
    // "one" is no longer in m_clients, so its ledger entry is dropped
    // without a send (nothing to send to); verify that path is clean
  }
  QCOMPARE(one.count(RecordedKey::Kind::Up, kKeyShift_L), 0);
}

void ServerKeyLedgerTests::broadcast_offReleasesHeldKeys()
{
  // Ups only go where downs went: turning broadcast off would strand keys
  // pressed under it, so the toggle releases them first.
  Fixture f;
  f.init({"one"});
  RecordingClient one("one");
  {
    Server server(f.config, f.primary, f.screen, &f.events);
    QVERIFY(server.m_clients.emplace("one", &one).second);
    server.m_keyboardBroadcasting = true;
    server.m_keyboardBroadcastingScreens = "*";
    server.onKeyDown(kKeyA, 0, kButtonA, "en", nullptr);
    one.keys.clear();

    Server::KeyboardBroadcastInfo info(Server::KeyboardBroadcastInfo::kOff, std::string{});
    server.handleKeyboardBroadcastEvent(
        Event(EventTypes::ServerKeyboardBroadcast, &server, static_cast<void *>(&info), Event::EventFlags::NoFlags)
    );

    QVERIFY(!server.m_keyboardBroadcasting);
    QVERIFY(one.findUp(kKeyA) != nullptr);
    QVERIFY(server.m_keysHeldOnBroadcast.empty());

    server.m_clients.erase("one");
  }
}

void ServerKeyLedgerTests::lockChange_pushesStateToActiveClient()
{
  // I2: Caps is STATE. When the primary's lock state changes while a client
  // is active, the fresh lock mask is pushed as a lock-key event carrying
  // the mask (wire-compatible: old clients ignore the lock KeyID).
  Fixture f;
  f.init({"remote"});
  RecordingClient remote("remote");
  {
    Server server(f.config, f.primary, f.screen, &f.events);
    QVERIFY(server.m_clients.emplace("remote", &remote).second);
    f.platform->osModifiers = 0;
    server.switchScreen(&remote, 50, 60, false);
    QCOMPARE(server.m_toggleMaskSentToActive, static_cast<KeyModifierMask>(0));
    remote.keys.clear();

    // user taps Caps on the primary: the OS flips it on
    f.platform->osModifiers = KeyModifierCapsLock;
    server.onKeyDown(kKeyCapsLock, KeyModifierCapsLock, kButtonCaps, "en", nullptr);

    QCOMPARE(server.m_toggleMaskSentToActive, KeyModifierCapsLock);
    // the relayed physical key AND the state update both carry the mask
    QCOMPARE(remote.count(RecordedKey::Kind::Down, kKeyCapsLock), 2);
    bool sawStateUpdate = false;
    for (const auto &k : remote.keys) {
      if (k.kind == RecordedKey::Kind::Down && k.id == kKeyCapsLock && k.button == 0) {
        sawStateUpdate = true;
        QCOMPARE(k.mask, KeyModifierCapsLock);
      }
    }
    QVERIFY(sawStateUpdate);
    // the state update is never left in the ledger (button 0 is not held)
    QVERIFY(!server.m_keysHeldOnActive.contains(0));
    remote.keys.clear();

    // the release settles nothing new: no second update
    server.onKeyUp(kKeyCapsLock, KeyModifierCapsLock, kButtonCaps, nullptr);
    QCOMPARE(remote.count(RecordedKey::Kind::Down, kKeyCapsLock), 0);

    // a hook that reported the pre-toggle state on the down is caught on
    // the up
    remote.keys.clear();
    server.onKeyDown(kKeyCapsLock, KeyModifierCapsLock, kButtonCaps, "en", nullptr);
    f.platform->osModifiers = 0;
    server.onKeyUp(kKeyCapsLock, 0, kButtonCaps, nullptr);
    QCOMPARE(server.m_toggleMaskSentToActive, static_cast<KeyModifierMask>(0));
    QCOMPARE(remote.count(RecordedKey::Kind::Down, kKeyCapsLock), 2); // relay + update

    // re-entering re-baselines from the enter mask
    server.switchScreen(f.primary, 512, 384, false);
    f.platform->osModifiers = KeyModifierNumLock;
    server.switchScreen(&remote, 50, 60, false);
    QCOMPARE(server.m_toggleMaskSentToActive, KeyModifierNumLock);

    server.m_clients.erase("remote");
  }
}

QTEST_MAIN(ServerKeyLedgerTests)

namespace {

deskflow::coordination::RelayKeyEvent relayed(
    const char *from, deskflow::coordination::RelayKeyPhase phase, KeyID id, KeyButton button,
    KeyModifierMask mask = 0
)
{
  deskflow::coordination::RelayKeyEvent event;
  event.from = from;
  event.phase = phase;
  event.id = id;
  event.button = button;
  event.mask = mask;
  event.lang = "en";
  return event;
}

} // namespace

void ServerKeyLedgerTests::clearAll_releasesOnlyThatSendersKeysOnTheActiveClient()
{
  // The cursor is on a secondary; two fleet peers relay keys through the
  // server, which routes them to the ACTIVE CLIENT (not the primary). When
  // one peer's lane fails, its KeyClearAll must release exactly the keys it
  // relayed, on the client that holds them -- the primary sweep alone left
  // them stuck, and a blanket sweep over-released the other peer's keys.
  using deskflow::coordination::RelayKeyPhase;
  Fixture f;
  f.init({"remote"});
  RecordingClient remote("remote");
  {
    Server server(f.config, f.primary, f.screen, &f.events);
    QVERIFY(server.m_clients.emplace("remote", &remote).second);
    server.switchScreen(&remote, 50, 60, false);

    server.relayForwardedKey(relayed("tiny11", RelayKeyPhase::Down, kKeyShift_L, kButtonShift, KeyModifierShift));
    server.relayForwardedKey(relayed("tiny11", RelayKeyPhase::Down, kKeyA, kButtonA, KeyModifierShift));
    server.relayForwardedKey(relayed("macbookpro", RelayKeyPhase::Down, kKeyCapsLock, kButtonCaps));
    server.relayForwardedKey(relayed("macbookpro", RelayKeyPhase::Down, static_cast<KeyID>('b'), 0x30));
    QCOMPARE(server.m_forwardedHeld.at("tiny11").size(), 2u);
    QCOMPARE(server.m_forwardedHeld.at("macbookpro").size(), 2u);
    QCOMPARE(server.m_keysHeldOnActive.size(), 4u);

    // a relayed Up drops its entry from the sender's set
    server.relayForwardedKey(relayed("tiny11", RelayKeyPhase::Up, kKeyA, kButtonA));
    QCOMPARE(server.m_forwardedHeld.at("tiny11").size(), 1u);
    remote.keys.clear();

    server.releaseForwardedKeys("tiny11");

    // exactly tiny11's remaining hold (Shift) was released on the client
    QCOMPARE(remote.count(RecordedKey::Kind::Up, kKeyShift_L), 1);
    const auto *up = remote.findUp(kKeyShift_L);
    QVERIFY(up != nullptr);
    QCOMPARE(up->button, kButtonShift);
    QCOMPARE(up->mask, static_cast<KeyModifierMask>(0));
    QCOMPARE(remote.count(RecordedKey::Kind::Up, kKeyA), 0);
    QCOMPARE(remote.count(RecordedKey::Kind::Up, kKeyCapsLock), 0);
    QCOMPARE(remote.count(RecordedKey::Kind::Up, static_cast<KeyID>('b')), 0);
    QVERIFY(!server.m_forwardedHeld.contains("tiny11"));
    QCOMPARE(server.m_forwardedHeld.at("macbookpro").size(), 2u);
    // ... and the active ledger no longer expects to release it on switch
    QVERIFY(!server.m_keysHeldOnActive.contains(kButtonShift));
    QCOMPARE(server.m_keysHeldOnActive.size(), 2u);

    // an unknown sender releases nothing on the client
    remote.keys.clear();
    server.releaseForwardedKeys("stranger");
    QCOMPARE(remote.count(RecordedKey::Kind::Up, kKeyCapsLock), 0);
    QCOMPARE(remote.count(RecordedKey::Kind::Up, static_cast<KeyID>('b')), 0);

    server.m_clients.erase("remote");
  }
}

void ServerKeyLedgerTests::clearAll_withoutSenderReleasesEveryRelayedKey()
{
  // Legacy clear-all (no sender name): every relayed hold goes.
  using deskflow::coordination::RelayKeyPhase;
  Fixture f;
  f.init({"remote"});
  RecordingClient remote("remote");
  {
    Server server(f.config, f.primary, f.screen, &f.events);
    QVERIFY(server.m_clients.emplace("remote", &remote).second);
    server.switchScreen(&remote, 50, 60, false);

    server.relayForwardedKey(relayed("tiny11", RelayKeyPhase::Down, kKeyShift_L, kButtonShift, KeyModifierShift));
    server.relayForwardedKey(relayed("macbookpro", RelayKeyPhase::Down, static_cast<KeyID>('b'), 0x30));
    remote.keys.clear();

    server.releaseForwardedKeys();

    QCOMPARE(remote.count(RecordedKey::Kind::Up, kKeyShift_L), 1);
    QCOMPARE(remote.count(RecordedKey::Kind::Up, static_cast<KeyID>('b')), 1);
    QVERIFY(server.m_forwardedHeld.empty());
    QVERIFY(server.m_keysHeldOnActive.empty());

    // with the cursor home, keys relayed to the primary are not ledgered
    // here (the primary's own key state holds and sweeps them)
    server.switchScreen(f.primary, 512, 384, false);
    server.relayForwardedKey(relayed("tiny11", RelayKeyPhase::Down, kKeyA, kButtonA));
    QVERIFY(server.m_forwardedHeld.empty());
    server.releaseForwardedKeys("tiny11");

    server.m_clients.erase("remote");
  }
}

void ServerKeyLedgerTests::switch_releasesBeforeLeave()
{
  // K2 gap a: a client that tears down on kMsgCLeave discards whatever
  // follows it, so every kMsgDKeyUp for the screen being abandoned has to
  // hit the wire BEFORE the leave (a proxy leave never refuses).
  Fixture f;
  f.init({"remote"});
  RecordingClient remote("remote");
  {
    Server server(f.config, f.primary, f.screen, &f.events);
    QVERIFY(server.m_clients.emplace("remote", &remote).second);
    server.switchScreen(&remote, 50, 60, false);

    server.onKeyDown(kKeyA, 0, kButtonA, "en", nullptr);
    server.onKeyDown(kKeyShift_L, KeyModifierShift, kButtonShift, "en", nullptr);
    remote.keys.clear();
    remote.leaveAtKeyIndex.clear();

    server.switchScreen(f.primary, 512, 384, false);

    QCOMPARE(remote.leaveAtKeyIndex.size(), size_t(1));
    const size_t leaveAt = remote.leaveAtKeyIndex[0];
    QCOMPARE(remote.count(RecordedKey::Kind::Up, kKeyA), 1);
    QCOMPARE(remote.count(RecordedKey::Kind::Up, kKeyShift_L), 1);
    for (size_t i = 0; i < remote.keys.size(); ++i) {
      if (remote.keys[i].kind == RecordedKey::Kind::Up) {
        QVERIFY2(i < leaveAt, qPrintable(QStringLiteral("key up #%1 sent after leave (at %2)").arg(i).arg(leaveAt)));
      }
    }
    // and nothing at all trails the leave
    QCOMPARE(leaveAt, remote.keys.size());
    QVERIFY(server.m_keysHeldOnActive.empty());

    server.m_clients.erase("remote");
  }
}

void ServerKeyLedgerTests::enterPrimary_releasesOnlyInjectedLedger()
{
  // Server role. The user comes back onto the primary shift-dragging (the
  // OS holds Shift, no hardware flagsChanged for seconds): enter must close
  // what the platform ledger holds and NEVER run the freshness sweep,
  // which would post a Shift UP mid-gesture.
  Fixture f;
  f.init({"remote"});
  RecordingClient remote("remote");
  {
    Server server(f.config, f.primary, f.screen, &f.events);
    QVERIFY(server.m_clients.emplace("remote", &remote).second);
    server.switchScreen(&remote, 50, 60, false);
    const int sanitizeBefore = f.platform->sanitizeCalls;
    const int releaseBefore = f.platform->releaseCalls;

    f.platform->osModifiers = KeyModifierShift;
    server.switchScreen(f.primary, 512, 384, false);

    QCOMPARE(f.platform->releaseCalls, releaseBefore + 1);
    QCOMPARE(f.platform->sanitizeCalls, sanitizeBefore);
    server.m_clients.erase("remote");
  }
}

void ServerKeyLedgerTests::enterSecondary_logsStuckReleaseWhenModifierPersists()
{
  // Client role. The OS still holds Shift after we entered and nobody has
  // typed here: the verifier notes it at the first check, confirms it at
  // the second and closes the ledger (never the freshness sweep -- that
  // Shift may be held on this machine's own keyboard), logging the line
  // fleet-health greps for.
  EventQueue events;
  auto *platform = new SecondaryPlatformScreen(&events); // owned by the Screen
  deskflow::Screen screen(platform, &events);

  // the held= line is INFO, stuck-release is WARNING; the LOG macros write
  // to the singleton (CLOG), which is what fleet-health's grep sees
  struct FilterGuard
  {
    LogLevel::Level previous = CLOG->getFilter();
    ~FilterGuard()
    {
      CLOG->setFilter(previous);
    }
  } filterGuard;
  CLOG->setFilter(LogLevel::Level::Info);
  NeedleOutputter held(QStringLiteral("[keys] post-switch held="));
  NeedleOutputter stuck(QStringLiteral("[keys] stuck-release"));

  screen.enable();
  const int sanitizeAfterEnable = platform->sanitizeCalls; // enable() sweeps once itself
  QCOMPARE(platform->releaseCalls, 0);
  platform->osModifiers = KeyModifierShift;
  screen.enter(0);

  pumpUntil(events, deskflow::Screen::kPostSwitchFirstCheckS + deskflow::Screen::kPostSwitchSecondCheckS + 3.0, [&] {
    return platform->releaseCalls > 0;
  });
  QCOMPARE(platform->releaseCalls, 1);
  QCOMPARE(platform->sanitizeCalls, sanitizeAfterEnable);
  QCOMPARE(held.hits, 1);
  QCOMPARE(stuck.hits, 1);

  // Leave (cancels any timer; no sweep of its own) and come back; this
  // time the server types here before the first check: the held Shift is
  // the user's chord, not a leftover -- no release, no line.
  QVERIFY(screen.leave());
  QCOMPARE(platform->sanitizeCalls, sanitizeAfterEnable);
  screen.enter(0);
  screen.keyDown(kKeyA, KeyModifierShift, kButtonA, "en");
  pumpUntil(events, deskflow::Screen::kPostSwitchFirstCheckS + deskflow::Screen::kPostSwitchSecondCheckS + 0.5, [] {
    return false;
  });
  QCOMPARE(platform->releaseCalls, 1);
  QCOMPARE(platform->sanitizeCalls, sanitizeAfterEnable);
  QCOMPARE(held.hits, 1);
  QCOMPARE(stuck.hits, 1);

  QVERIFY(screen.leave());
  screen.disable();
}
