/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "KeyStateLedgerTests.h"

#include "MockKeyState.h"
#include "arch/Arch.h"
#include "base/EventQueue.h"
#include "base/Log.h"
#include "common/Settings.h"
#include "deskflow/KeyMap.h"
#include "deskflow/KeyState.h"
#include "deskflow/PlatformScreen.h"
#include "deskflow/Screen.h"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace {

//! What the platform layer was asked to do, in order.
struct PlatformCall
{
  enum class Kind
  {
    KeyDown,
    KeyUp,
    AllKeysUp,
    SetToggle,
    Sanitize,
    ReleaseInjected,
  };
  Kind kind;
  KeyID id = kKeyNone;
  KeyButton button = 0;
  KeyModifierMask mask = 0;
  bool on = false;
};

//! Fake IPlatformScreen that records injections and reports a scripted
//! OS modifier state.
class FakePlatformScreen : public PlatformScreen
{
public:
  FakePlatformScreen(IEventQueue *events, bool primary)
      : PlatformScreen(events),
        m_primary(primary),
        m_keyState(*events)
  {
  }

  // scripted OS truth
  KeyModifierMask osModifiers = 0;
  std::vector<PlatformCall> calls;

  // IScreen
  void *getEventTarget() const override
  {
    return const_cast<FakePlatformScreen *>(this);
  }
  bool getClipboard(ClipboardID, IClipboard *) const override
  {
    return false;
  }
  void getShape(int32_t &x, int32_t &y, int32_t &w, int32_t &h) const override
  {
    x = 0;
    y = 0;
    w = 1024;
    h = 768;
  }
  void getCursorPos(int32_t &x, int32_t &y) const override
  {
    x = 0;
    y = 0;
  }

  // IPrimaryScreen
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
    x = 0;
    y = 0;
  }

  // ISecondaryScreen
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

  // IKeyState (recording)
  void updateKeyMap() override
  {
  }
  void updateKeyState() override
  {
  }
  void fakeKeyDown(KeyID id, KeyModifierMask mask, KeyButton button, const std::string &) override
  {
    calls.push_back({PlatformCall::Kind::KeyDown, id, button, mask});
    m_down.insert(button);
  }
  bool fakeKeyUp(KeyButton button) override
  {
    if (!m_down.contains(button)) {
      return false;
    }
    m_down.erase(button);
    calls.push_back({PlatformCall::Kind::KeyUp, kKeyNone, button});
    return true;
  }
  void fakeAllKeysUp() override
  {
    calls.push_back({PlatformCall::Kind::AllKeysUp});
    m_down.clear();
  }
  KeyModifierMask pollActiveModifiers() const override
  {
    return osModifiers;
  }
  void setToggleState(KeyModifierMask toggle, bool on) override
  {
    calls.push_back({PlatformCall::Kind::SetToggle, kKeyNone, 0, toggle, on});
  }
  void sanitizeInjectedKeys() override
  {
    calls.push_back({PlatformCall::Kind::Sanitize});
  }
  void releaseInjectedKeys(KeyModifierMask keep = 0) override
  {
    calls.push_back({PlatformCall::Kind::ReleaseInjected, kKeyNone, 0, keep});
  }

  // IPlatformScreen
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
    return m_primary;
  }

  int count(PlatformCall::Kind kind) const
  {
    int n = 0;
    for (const auto &c : calls) {
      if (c.kind == kind) {
        ++n;
      }
    }
    return n;
  }

  const PlatformCall *find(PlatformCall::Kind kind) const
  {
    for (const auto &c : calls) {
      if (c.kind == kind) {
        return &c;
      }
    }
    return nullptr;
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
  bool m_primary;
  MockKeyState m_keyState;
  std::set<KeyButton> m_down;
};

//! Real KeyState over a one-key map, recording what it synthesizes.
class RecordingKeyState : public KeyState
{
public:
  RecordingKeyState(IEventQueue *events, deskflow::KeyMap &map) : KeyState(events, map, {"en"}, true)
  {
  }

  KeyModifierMask osModifiers = 0;
  std::vector<std::pair<KeyButton, bool>> strokes; // (button, press)
  std::vector<std::pair<KeyModifierMask, bool>> toggles;

  KeyModifierMask pollActiveModifiers() const override
  {
    return osModifiers;
  }
  int32_t pollActiveGroup() const override
  {
    return 0;
  }
  void pollPressedKeys(KeyButtonSet &) const override
  {
  }
  bool fakeCtrlAltDel() override
  {
    return false;
  }
  void setToggleState(KeyModifierMask toggle, bool on) override
  {
    toggles.emplace_back(toggle, on);
    // the platform overrides (OSX/MSWindows) keep the tracked mask in step
    // with the lock state they just applied; mirror that contract here
    auto &tracked = getActiveModifiersRValue();
    tracked = on ? (tracked | toggle) : (tracked & ~toggle);
  }

protected:
  void getKeyMap(deskflow::KeyMap &) override
  {
  }
  void fakeKey(const Keystroke &stroke) override
  {
    if (stroke.m_type == Keystroke::KeyType::Button) {
      strokes.emplace_back(stroke.m_data.m_button.m_button, stroke.m_data.m_button.m_press);
    }
  }
};

void addOneKey(deskflow::KeyMap &map, KeyID id, KeyButton button)
{
  deskflow::KeyMap::KeyItem item;
  item.m_id = id;
  item.m_group = 0;
  item.m_button = button;
  map.addKeyEntry(item);
  map.finish();
}

//! A Caps-sensitive layout: a Caps Lock key (locking modifier) and an 'A'
//! that requires Caps, the shape a real uchr/scan layout produces.
void addCapsLayout(deskflow::KeyMap &map)
{
  deskflow::KeyMap::KeyItem caps;
  caps.m_id = kKeyCapsLock;
  caps.m_group = 0;
  caps.m_button = 0x3A;
  caps.m_generates = KeyModifierCapsLock;
  caps.m_lock = true;
  map.addKeyEntry(caps);

  deskflow::KeyMap::KeyItem upperA;
  upperA.m_id = static_cast<KeyID>('A');
  upperA.m_group = 0;
  upperA.m_button = 0x1E;
  upperA.m_required = KeyModifierCapsLock;
  upperA.m_sensitive = KeyModifierShift | KeyModifierCapsLock;
  map.addKeyEntry(upperA);
  map.finish();
}

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

struct SecondaryFixture
{
  EventQueue events;
  FakePlatformScreen *platform = nullptr; // owned by screen
  std::unique_ptr<deskflow::Screen> screen;

  explicit SecondaryFixture(bool primary = false)
  {
    platform = new FakePlatformScreen(&events, primary);
    screen = std::make_unique<deskflow::Screen>(platform, &events);
    screen->enable();
    platform->calls.clear();
  }

  ~SecondaryFixture()
  {
    // ~Screen disables and deletes the platform screen
  }
};

} // namespace

std::unique_ptr<Arch> g_arch;
Log g_log;
std::unique_ptr<QCoreApplication> g_app;
std::unique_ptr<QTemporaryDir> g_settingsDir;

void KeyStateLedgerTests::initTestCase()
{
  static int argc = 1;
  static char arg0[] = "KeyStateLedgerTests";
  static char *argv[] = {arg0, nullptr};
  g_app = std::make_unique<QCoreApplication>(argc, argv);
  g_arch = std::make_unique<Arch>();
  g_log.setFilter(LogLevel::Level::Error);
  // isolate Settings (Screen::enter/leave consult the enter/exit command
  // options) so nothing touches the developer's real config
  g_settingsDir = std::make_unique<QTemporaryDir>();
  QVERIFY(g_settingsDir->isValid());
  Settings::setSettingsFile(g_settingsDir->filePath(QStringLiteral("Deskflow.conf")));
}

void KeyStateLedgerTests::cleanupTestCase()
{
  g_app.reset();
  g_arch.reset();
}

void KeyStateLedgerTests::enterSecondary_reassertsHeldShiftWhenOsLacksIt()
{
  // The user crossed with Shift physically held on the primary; our OS has
  // no Shift down. The enter mask says held, so press it here and track it
  // as synthetic.
  SecondaryFixture f;
  f.platform->osModifiers = 0;

  f.screen->enter(KeyModifierShift);

  const auto *down = f.platform->find(PlatformCall::Kind::KeyDown);
  QVERIFY(down != nullptr);
  QCOMPARE(down->id, kKeyShift_L);
  QCOMPARE(f.platform->count(PlatformCall::Kind::KeyDown), 1);
  // Leaving must release it (fakeAllKeysUp covers every synthetic key).
  f.platform->calls.clear();
  QVERIFY(f.screen->leave());
  QCOMPARE(f.platform->count(PlatformCall::Kind::AllKeysUp), 1);
}

void KeyStateLedgerTests::enterSecondary_doesNotReassertWhenOsAlreadyHoldsIt()
{
  // Never double-press: if the OS already reports Shift down, the mask bit
  // is satisfied and no synthetic press is made.
  SecondaryFixture f;
  f.platform->osModifiers = KeyModifierShift;

  f.screen->enter(KeyModifierShift);

  QCOMPARE(f.platform->count(PlatformCall::Kind::KeyDown), 0);
  QVERIFY(f.screen->leave());
}

void KeyStateLedgerTests::enterSecondary_reassertsModifiersIncrementally()
{
  // Win+Shift held across the crossing. Each re-assert press must carry
  // only the state reached so far (OS state + the modifiers pressed before
  // it), never the whole enter mask: handing Shift the Super bit as its
  // desired state made mapKey() tap Super around the press -- a Start-menu
  // flash on every Win+Shift crossing.
  SecondaryFixture f;
  f.platform->osModifiers = KeyModifierControl; // something the OS already holds

  f.screen->enter(KeyModifierShift | KeyModifierSuper | KeyModifierCapsLock);

  QCOMPARE(f.platform->count(PlatformCall::Kind::KeyDown), 2);
  const auto &calls = f.platform->calls;
  std::vector<const PlatformCall *> downs;
  for (const auto &c : calls) {
    if (c.kind == PlatformCall::Kind::KeyDown) {
      downs.push_back(&c);
    }
  }
  // table order: Shift before Super
  QCOMPARE(downs[0]->id, kKeyShift_L);
  QCOMPARE(downs[0]->mask, static_cast<KeyModifierMask>(KeyModifierControl | KeyModifierShift));
  QCOMPARE(downs[1]->id, kKeySuper_L);
  QCOMPARE(downs[1]->mask, static_cast<KeyModifierMask>(KeyModifierControl | KeyModifierShift | KeyModifierSuper));
  // lock bits never ride along as desired state
  for (const auto *d : downs) {
    QCOMPARE(d->mask & IKeyState::s_lockModifierMask, static_cast<KeyModifierMask>(0));
  }
  QVERIFY(f.screen->leave());
}

void KeyStateLedgerTests::enterSecondary_appliesCapsViaSetToggleState()
{
  // I2: Caps is STATE. Primary says Caps on, our OS says off -> apply on.
  SecondaryFixture f;
  f.platform->osModifiers = 0;

  f.screen->enter(KeyModifierCapsLock);

  const auto *toggle = f.platform->find(PlatformCall::Kind::SetToggle);
  QVERIFY(toggle != nullptr);
  QCOMPARE(toggle->mask, KeyModifierCapsLock);
  QVERIFY(toggle->on);
  // a lock bit is never re-asserted as a key press
  QCOMPARE(f.platform->count(PlatformCall::Kind::KeyDown), 0);
  QVERIFY(f.screen->leave());
}

void KeyStateLedgerTests::enterSecondary_clearsCapsWhenPrimaryHasItOff()
{
  // The reverse drift: our OS has Caps on, the primary does not. Apply off
  // -- and when both agree, touch nothing.
  SecondaryFixture f;
  f.platform->osModifiers = KeyModifierCapsLock;

  f.screen->enter(0);
  const auto *toggle = f.platform->find(PlatformCall::Kind::SetToggle);
  QVERIFY(toggle != nullptr);
  QCOMPARE(toggle->mask, KeyModifierCapsLock);
  QVERIFY(!toggle->on);
  QVERIFY(f.screen->leave());

  f.platform->calls.clear();
  f.screen->enter(KeyModifierCapsLock);
  QCOMPARE(f.platform->count(PlatformCall::Kind::SetToggle), 0);
  QVERIFY(f.screen->leave());
}

void KeyStateLedgerTests::keyUp_mapsRealReleaseOntoReassertedModifier()
{
  // The server never sent the Shift DOWN (it was pressed before the
  // crossing) but it does send the UP with the primary's real button. That
  // release must land on the synthetic press instead of being dropped, or
  // Shift stays down until leave.
  SecondaryFixture f;
  f.platform->osModifiers = 0;
  f.screen->enter(KeyModifierShift);
  const auto *down = f.platform->find(PlatformCall::Kind::KeyDown);
  QVERIFY(down != nullptr);
  const KeyButton syntheticButton = down->button;
  f.platform->calls.clear();

  f.screen->keyUp(kKeyShift_L, 0, 0x2A /* primary's real button */);

  const auto *up = f.platform->find(PlatformCall::Kind::KeyUp);
  QVERIFY(up != nullptr);
  QCOMPARE(up->button, syntheticButton);

  // a second up for the same modifier has nothing to release
  f.platform->calls.clear();
  f.screen->keyUp(kKeyShift_R, 0, 0x36);
  QCOMPARE(f.platform->count(PlatformCall::Kind::KeyUp), 0);
  QVERIFY(f.screen->leave());
}

void KeyStateLedgerTests::leaveSecondary_releasesEverySyntheticKey()
{
  SecondaryFixture f;
  f.screen->enter(0);
  f.screen->keyDown(static_cast<KeyID>('a'), 0, 0x1E, "en");
  f.platform->calls.clear();

  QVERIFY(f.screen->leave());

  QCOMPARE(f.platform->count(PlatformCall::Kind::AllKeysUp), 1);
}

void KeyStateLedgerTests::disablePrimary_releasesInjectedKeysAndSanitizes()
{
  // A primary is never a target, yet relayed keys are injected into its OS.
  // Teardown must release them and give the platform a chance to sweep.
  EventQueue events;
  auto *platform = new FakePlatformScreen(&events, true);
  {
    deskflow::Screen screen(platform, &events);
    screen.enable();
    screen.keyDown(static_cast<KeyID>('x'), 0, 0x2D, "en");
    platform->calls.clear();

    screen.disable();

    QCOMPARE(platform->count(PlatformCall::Kind::AllKeysUp), 1);
    QCOMPARE(platform->count(PlatformCall::Kind::Sanitize), 1);
    // ~Screen deletes platform
  }
}

void KeyStateLedgerTests::updateKeyState_releasesSyntheticKeysBeforeZeroing()
{
  // updateKeyState() used to memset m_syntheticKeys with keys still down,
  // silently forgetting them. It must release first.
  EventQueue events;
  deskflow::KeyMap map;
  addOneKey(map, static_cast<KeyID>('a'), 0x1E);
  RecordingKeyState ks(&events, map);

  ks.fakeKeyDown(static_cast<KeyID>('a'), 0, 0x10, "en");
  QVERIFY(ks.isKeyDown(0x1E));
  QCOMPARE(ks.strokes.size(), 1u);
  QVERIFY(ks.strokes[0].second); // press
  ks.strokes.clear();

  ks.updateKeyState();

  QCOMPARE(ks.strokes.size(), 1u);
  QCOMPARE(ks.strokes[0].first, static_cast<KeyButton>(0x1E));
  QVERIFY(!ks.strokes[0].second); // release
  QVERIFY(!ks.isKeyDown(0x1E));

  // and with nothing down it stays quiet
  ks.strokes.clear();
  ks.updateKeyState();
  QVERIFY(ks.strokes.empty());
}

void KeyStateLedgerTests::fakeKeyDown_capsKeyRoutesMaskBitToSetToggleState()
{
  // The Caps KeyID stays ignored (never a toggle press on the target), but
  // the mask bit riding with it is the primary's lock STATE and is applied
  // absolutely when it disagrees with the OS.
  EventQueue events;
  deskflow::KeyMap map;
  addOneKey(map, static_cast<KeyID>('a'), 0x1E);
  RecordingKeyState ks(&events, map);
  ks.osModifiers = 0;

  ks.fakeKeyDown(kKeyCapsLock, KeyModifierCapsLock, 0x3A, "en");
  QCOMPARE(ks.toggles.size(), 1u);
  QCOMPARE(ks.toggles[0].first, KeyModifierCapsLock);
  QVERIFY(ks.toggles[0].second);
  QVERIFY(ks.strokes.empty()); // no key press synthesized

  // already in phase: nothing to apply
  ks.osModifiers = KeyModifierCapsLock;
  ks.fakeKeyDown(kKeyCapsLock, KeyModifierCapsLock, 0x3A, "en");
  QCOMPARE(ks.toggles.size(), 1u);

  // primary turned it off
  ks.fakeKeyDown(kKeyCapsLock, 0, 0x3A, "en");
  QCOMPARE(ks.toggles.size(), 2u);
  QVERIFY(!ks.toggles[1].second);
}

void KeyStateLedgerTests::fakeKeyDown_doesNotClickCapsAgainAfterSetToggleState()
{
  // The Caps double-toggle: enter applies Caps ON via setToggleState() (OS
  // now on). If the tracked mask is left at "off", the first letter whose
  // mask carries Caps makes mapKey() click the Caps key to "turn it on" --
  // inverting it for the rest of the epoch. setToggleState() must keep the
  // tracked mask in step, and fakeKeyDown() must judge by that mask.
  EventQueue events;
  deskflow::KeyMap map;
  addCapsLayout(map);
  RecordingKeyState ks(&events, map);
  ks.osModifiers = 0;
  ks.updateKeyState(); // tracked mask = OS truth = no caps

  // enter with caps on (Screen::applyToggleMask -> setToggleState)
  ks.setToggleState(KeyModifierCapsLock, true);
  QCOMPARE(ks.toggles.size(), 1u);
  QVERIFY((ks.getActiveModifiers() & KeyModifierCapsLock) != 0);

  // the relayed Caps key itself (mask says on) must not re-apply
  ks.fakeKeyDown(kKeyCapsLock, KeyModifierCapsLock, 0x3A, "en");
  QCOMPARE(ks.toggles.size(), 1u);

  // next letter: 'A' with caps in its mask -> exactly one press of 0x1E,
  // and NO click of the caps button (0x3A)
  ks.strokes.clear();
  ks.fakeKeyDown(static_cast<KeyID>('A'), KeyModifierCapsLock, 0x10, "en");
  for (const auto &[button, press] : ks.strokes) {
    QVERIFY2(button != 0x3A, "caps lock was clicked again");
  }
  QCOMPARE(ks.strokes.size(), 1u);
  QCOMPARE(ks.strokes[0].first, static_cast<KeyButton>(0x1E));
  QVERIFY(ks.strokes[0].second);
}

void KeyStateLedgerTests::enable_sanitizesOnlyWhenNotEntered()
{
  // K2 residual: a client (never entered at enable) sweeps stale OS-held
  // modifiers before it can be typed into; a primary (entered at enable,
  // the user's hands are on it) does not.
  EventQueue events;
  {
    auto *platform = new FakePlatformScreen(&events, false);
    deskflow::Screen client(platform, &events);
    client.enable();
    QCOMPARE(platform->count(PlatformCall::Kind::Sanitize), 1);
  }
  {
    auto *platform = new FakePlatformScreen(&events, true);
    deskflow::Screen primary(platform, &events);
    primary.enable();
    QCOMPARE(platform->count(PlatformCall::Kind::Sanitize), 0);
  }
}

void KeyStateLedgerTests::describeKey_printsCharacterWithItsCase()
{
  const std::string upper = IKeyState::describeKey(static_cast<KeyID>('A'));
  const std::string lower = IKeyState::describeKey(static_cast<KeyID>('a'));
  QVERIFY(upper.find("('A')") != std::string::npos);
  QVERIFY(lower.find("('a')") != std::string::npos);
  QVERIFY(upper.find("65") == 0);
  const std::string caps = IKeyState::describeKey(kKeyCapsLock);
  QVERIFY(caps.find("CapsLock") != std::string::npos);
  QVERIFY(caps.find('\'') == std::string::npos);
}

void KeyStateLedgerTests::primarySweep_neverReleasesPhysicallyCapturedKey()
{
  // K4 audit HIGH-1 (Pair D): on the primary, KeyState::onKey records what
  // the hardware tap saw -- the user's own Shift, held while dragging across
  // the edge. It is captured, not injected, so neither the leave-time
  // updateKeyState() (Screen::leavePrimary) nor the disable-time
  // fakeAllKeysUp() (Screen::disable, PrimaryClient::releaseForwardedKeys)
  // may post a release for it. A Shift WE injected still is.
  constexpr KeyButton kShift = 0x38;
  deskflow::KeyMap map;
  deskflow::KeyMap::KeyItem shift;
  shift.m_id = kKeyShift_L;
  shift.m_group = 0;
  shift.m_button = kShift;
  shift.m_generates = KeyModifierShift;
  map.addKeyEntry(shift);
  map.finish();
  EventQueue events;
  RecordingKeyState ks(&events, map);
  ks.osModifiers = KeyModifierShift; // the OS still reports the user's Shift down

  auto releasedShift = [&] {
    for (const auto &[button, press] : ks.strokes) {
      if (button == kShift && !press) {
        return true;
      }
    }
    return false;
  };

  // hardware capture on the primary: the user presses and HOLDS Shift
  ks.onKey(kShift, true, KeyModifierShift);
  QVERIFY(ks.isKeyDown(kShift));

  // Server::switchScreen -> PrimaryClient::leave -> Screen::leavePrimary -> updateKeyState()
  ks.updateKeyState();
  QVERIFY2(!releasedShift(), "updateKeyState released a physically held Shift");
  QVERIFY(ks.strokes.empty());

  // Screen::disable(primary) / releaseForwardedKeys -> fakeAllKeysUp()
  ks.onKey(kShift, true, KeyModifierShift);
  ks.fakeAllKeysUp();
  QVERIFY2(!releasedShift(), "fakeAllKeysUp released a physically held Shift");
  QVERIFY(ks.strokes.empty());

  // control: an injected Shift IS synthetic and IS released (press + release)
  ks.fakeKeyDown(kKeyShift_L, 0, 0x1F0, "en");
  ks.fakeAllKeysUp();
  QCOMPARE(ks.strokes.size(), size_t(2));
  QVERIFY(ks.strokes[0].first == kShift && ks.strokes[0].second);
  QVERIFY(ks.strokes[1].first == kShift && !ks.strokes[1].second);
}

void KeyStateLedgerTests::postSwitchVerifier_keepsReassertedModifiers()
{
  // K4 audit MED-3: the user crosses with Shift held (re-asserted on enter)
  // and some OTHER modifier reads stuck (the OS still holds Ctrl, nothing
  // typed). Pass 2 must close the ledger EXCEPT the re-asserted Shift --
  // it used to release everything, dropping the user's shift-drag.
  SecondaryFixture f;
  f.platform->osModifiers = 0;
  f.screen->enter(KeyModifierShift);
  const auto *down = f.platform->find(PlatformCall::Kind::KeyDown);
  QVERIFY(down != nullptr);
  QCOMPARE(down->id, KeyID(kKeyShift_L));
  f.platform->calls.clear();

  // now the OS reports Shift (ours) and a stale Ctrl
  f.platform->osModifiers = KeyModifierShift | KeyModifierControl;
  pumpUntil(f.events, deskflow::Screen::kPostSwitchFirstCheckS + deskflow::Screen::kPostSwitchSecondCheckS + 3.0, [&] {
    return f.platform->count(PlatformCall::Kind::ReleaseInjected) > 0;
  });
  QCOMPARE(f.platform->count(PlatformCall::Kind::ReleaseInjected), 1);
  const auto *release = f.platform->find(PlatformCall::Kind::ReleaseInjected);
  QVERIFY(release != nullptr);
  QCOMPARE(release->mask, KeyModifierMask(KeyModifierShift)); // kept: the re-asserted Shift
  QCOMPARE(f.platform->count(PlatformCall::Kind::KeyUp), 0);   // Shift itself untouched

  // ... and the re-asserted Shift still has its own release path
  f.platform->calls.clear();
  f.screen->keyUp(kKeyShift_L, 0, 0x2A);
  QCOMPARE(f.platform->count(PlatformCall::Kind::KeyUp), 1);
  QVERIFY(f.screen->leave());
}

// Reviewer (K4 item 2): shift-drag across, the user releases Shift on the
// server while on the client, then some other modifier reads stuck. Pass 2
// must release with keep=0: the stale Shift is no longer "kept".
void KeyStateLedgerTests::postSwitchVerifier_keepsNothingAfterShiftReleased()
{
  SecondaryFixture f;
  f.platform->osModifiers = 0;
  f.screen->enter(KeyModifierShift);
  QVERIFY(f.platform->find(PlatformCall::Kind::KeyDown) != nullptr);
  f.platform->calls.clear();

  f.screen->keyUp(kKeyShift_L, 0, 0x2A); // real release relayed from the server
  QCOMPARE(f.platform->count(PlatformCall::Kind::KeyUp), 1);
  f.platform->calls.clear();

  f.platform->osModifiers = KeyModifierControl; // stale Ctrl, nothing typed
  pumpUntil(f.events, deskflow::Screen::kPostSwitchFirstCheckS + deskflow::Screen::kPostSwitchSecondCheckS + 3.0, [&] {
    return f.platform->count(PlatformCall::Kind::ReleaseInjected) > 0;
  });
  QCOMPARE(f.platform->count(PlatformCall::Kind::ReleaseInjected), 1);
  const auto *release = f.platform->find(PlatformCall::Kind::ReleaseInjected);
  QVERIFY(release != nullptr);
  QCOMPARE(release->mask, KeyModifierMask(0));
  QVERIFY(f.screen->leave());
}

QTEST_MAIN(KeyStateLedgerTests)
