/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2003 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/Screen.h"
#include "base/EventTypes.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "deskflow/IPlatformScreen.h"

#include <QProcess>

#ifdef Q_OS_WIN
#include "arch/win32/ArchMiscWindows.h"
#include "platform/MSWindowsProcess.h"
#endif

namespace deskflow {

namespace {

bool runScreenCommand(const QString &commandLine)
{
#ifdef Q_OS_WIN
  using deskflow::platform::MSWindowsProcess;
  if (ArchMiscWindows::isProcessElevated()) {
    LOG_DEBUG("current process is elevated, starting detached process as session user");
    return MSWindowsProcess::startDetachedAsSessionUser(commandLine.toStdWString());
  }
#endif

  auto args = QProcess::splitCommand(commandLine);
  if (args.isEmpty()) {
    return false;
  }
  const auto program = args.takeFirst();
  return QProcess::startDetached(program, args);
}

//! Held (non-lock) modifiers the server reports on enter, with the key we
//! press to re-assert each one and a reserved synthetic server button.
/*!
The buttons sit at the top of the server-button space (kButtonMask is
0x1FF) where no real scan code lands, so a later DKeyUp from the server
cannot alias them; Screen::keyUp maps the real up onto them by KeyID.
*/
struct ReassertedModifier
{
  KeyModifierMask bit;
  KeyID key;
  KeyButton button;
};

constexpr ReassertedModifier kReassertedModifiers[] = {
    {KeyModifierShift, kKeyShift_L, 0x1F0},     //
    {KeyModifierControl, kKeyControl_L, 0x1F1}, //
    {KeyModifierAlt, kKeyAlt_L, 0x1F2},         //
    {KeyModifierMeta, kKeyMeta_L, 0x1F3},       //
    {KeyModifierSuper, kKeySuper_L, 0x1F4},     //
};

//! Modifier bit a real modifier KeyID drives, or 0.
KeyModifierMask modifierBitForKey(KeyID id)
{
  switch (id) {
  case kKeyShift_L:
  case kKeyShift_R:
    return KeyModifierShift;
  case kKeyControl_L:
  case kKeyControl_R:
    return KeyModifierControl;
  case kKeyAlt_L:
  case kKeyAlt_R:
    return KeyModifierAlt;
  case kKeyMeta_L:
  case kKeyMeta_R:
    return KeyModifierMeta;
  case kKeySuper_L:
  case kKeySuper_R:
    return KeyModifierSuper;
  default:
    return 0;
  }
}

} // namespace

//
// Screen
//

Screen::Screen(IPlatformScreen *platformScreen, IEventQueue *events)
    : m_screen(platformScreen),
      m_isPrimary(platformScreen->isPrimary()),
      m_entered(m_isPrimary),
      m_events(events)
{
  assert(m_screen != nullptr);

  // reset options
  resetOptions();

  LOG_DEBUG("opened display");
}

Screen::~Screen()
{

  if (m_enabled) {
    disable();
  }
  assert(!m_enabled);

  // Originally there was an assert here added before 2009 (history not in
  // tact). This condition seems to occur on a Windows client when the process
  // is shut down to make way for a new elevated process (e.g. at login screen).
  // The reason why this assert was originally added is unclear, and was causing
  // pain when using debug builds; you lose control of the client when it's at
  // the login screen. Therefore it has been converted to a warning so that we
  // can still see when it happens but it won't cause the process to pause. This
  // also gives us the added benefit of seeing when it happens in production.
  // Perhaps it indicates that the cursor is still being controlled on the
  // client while it's shutting down? i.e. the screen is entered and is not the
  // server, or the screen is not entered and is the server.
  if (m_entered == m_isPrimary) {
    LOG(
        (CLOG_DEBUG "current screen: entered=%s, primary=%s", //
         m_entered ? "yes" : "no", m_isPrimary ? "yes" : "no")
    );
    if (m_isPrimary) {
      LOG_WARN("current primary screen is not entered on shutdown");
    } else {
      LOG_WARN("current secondary screen is entered on shutdown");
    }
  }

  cancelPostSwitchVerifier();
  delete m_screen;
  LOG_DEBUG("closed display");
}

void Screen::enable()
{
  assert(!m_enabled);

  m_screen->updateKeyMap();
  m_screen->updateKeyState();
  m_screen->enable();
  if (!m_entered) {
    // K2 residual: a previous incarnation of this process may have died
    // with a modifier injected into the OS. Nobody is typing on a screen the
    // cursor is not on, so anything the OS still holds that no hardware
    // press backs is stale; let the platform sweep it before the first
    // relayed key lands on top of it. On macOS the freshness clock has no
    // observation yet at this instant (the tap was just created), so the
    // sweep is repeated once the window has elapsed (K5 review item 2).
    m_screen->sanitizeInjectedKeys();
    if (!m_isPrimary) {
      armEnableSweep();
    }
  }
  if (m_isPrimary) {
    enablePrimary();
  } else {
    enableSecondary();
  }

  // note activation
  m_enabled = true;
}

void Screen::disable()
{
  assert(m_enabled);

  cancelPostSwitchVerifier();
  cancelEnableSweep();
  if (!m_isPrimary && m_entered) {
    leave();
  } else if (m_isPrimary && !m_entered) {
    enter(0);
  }
  if (m_isPrimary) {
    // I1: a primary is never a target, yet relayed keys ARE injected into
    // its OS (PrimaryClient::injectForwardedKey). Tearing the primary down
    // with those still held (role flip, epoch teardown) used to strand them
    // with no one left to release them. Ledger only (K5): the user sits at
    // the primary and an epoch teardown lands at any moment -- including
    // mid-password with Shift held -- so the freshness sweep must not run.
    m_screen->fakeAllKeysUp();
    m_screen->releaseInjectedKeys();
  }
  m_screen->disable();
  if (m_isPrimary) {
    disablePrimary();
  } else {
    disableSecondary();
  }

  // note deactivation
  m_enabled = false;
}

void Screen::enter(KeyModifierMask toggleMask)
{
  LOG_INFO("entering screen");

  if (m_entered) {
    LOG_WARN("screen already entered");
  }

  // now on screen
  m_entered = true;

  m_screen->enter();
  if (m_isPrimary) {
    enterPrimary();
  } else {
    enterSecondary(toggleMask);
  }

  if (Settings::value(Settings::Core::EnableEnterCommand).toBool()) {
    const auto commandLine = Settings::value(Settings::Core::ScreenEnterCommand).toString();
    LOG_DEBUG("running screen enter command: %s", qPrintable(commandLine));
    if (!runScreenCommand(commandLine))
      LOG_ERR("failed to run screen enter command");
  }
}

bool Screen::leave()
{
  LOG_INFO("leaving screen");

  if (!m_entered) {
    LOG_WARN("screen already left");
  }

  if (!m_screen->canLeave()) {
    return false;
  }

  if (m_isPrimary) {
    leavePrimary();
  } else {
    leaveSecondary();
  }

  m_screen->leave();
  if (Settings::value(Settings::Core::EnableExitCommand).toBool()) {
    const auto commandLine = Settings::value(Settings::Core::ScreenExitCommand).toString();
    LOG_DEBUG("running screen exit command: %s", qPrintable(commandLine));
    if (!runScreenCommand(commandLine))
      LOG_ERR("failed to run screen exit command");
  }

  // make sure our idea of clipboard ownership is correct
  m_screen->checkClipboards();

  // now not on screen
  m_entered = false;

  return true;
}

void Screen::reconfigure(uint32_t activeSides)
{
  assert(m_isPrimary);
  m_screen->reconfigure(activeSides);
}

void Screen::warpCursor(int32_t x, int32_t y)
{
  assert(m_isPrimary);
  m_screen->warpCursor(x, y);
}

void Screen::setClipboard(ClipboardID id, const IClipboard *clipboard)
{
  m_screen->setClipboard(id, clipboard);
}

void Screen::grabClipboard(ClipboardID id)
{
  m_screen->setClipboard(id, nullptr);
}

void Screen::screensaver(bool) const
{
  // do nothing
}

void Screen::keyDown(KeyID id, KeyModifierMask mask, KeyButton button, const std::string &lang)
{
  m_lastKeyDownAt = std::chrono::steady_clock::now();
  // check for ctrl+alt+del emulation
  if (id == kKeyDelete && (mask & (KeyModifierControl | KeyModifierAlt)) == (KeyModifierControl | KeyModifierAlt)) {
    LOG_DEBUG("emulating ctrl+alt+del press");
    if (m_screen->fakeCtrlAltDel()) {
      return;
    }
  }
  m_screen->fakeKeyDown(id, mask, button, lang);
}

void Screen::keyRepeat(KeyID id, KeyModifierMask mask, int32_t count, KeyButton button, const std::string &lang)
{
  m_screen->fakeKeyRepeat(id, mask, count, button, lang);
}

void Screen::keyUp(KeyID id, KeyModifierMask, KeyButton button)
{
  if (m_screen->fakeKeyUp(button)) {
    return;
  }
  // The server never sent us the down for this button. If it is a modifier
  // we re-asserted on enter (held across the crossing), this is its real
  // release: map it onto the synthetic press so the key does not stay down
  // until leave.
  const KeyModifierMask bit = modifierBitForKey(id);
  if (bit == 0) {
    return;
  }
  const auto it = m_reassertedModifiers.find(bit);
  if (it == m_reassertedModifiers.end()) {
    return;
  }
  LOG_DEBUG("releasing modifier 0x%04x re-asserted on enter", bit);
  m_screen->fakeKeyUp(it->second);
  m_reassertedModifiers.erase(it);
}

void Screen::applyToggleMask(KeyModifierMask toggleMask)
{
  const KeyModifierMask osMods = m_screen->pollActiveModifiers();
  for (const KeyModifierMask lock : {KeyModifierCapsLock, KeyModifierNumLock, KeyModifierScrollLock}) {
    if (((toggleMask ^ osMods) & lock) == 0) {
      continue;
    }
    const bool on = (toggleMask & lock) != 0;
    LOG_DEBUG("lock 0x%04x: server says %s, OS says %s; applying", lock, on ? "on" : "off", on ? "off" : "on");
    m_screen->setToggleState(lock, on);
  }
}

void Screen::mouseDown(ButtonID button)
{
  m_screen->fakeMouseButton(button, true);
}

void Screen::mouseUp(ButtonID button)
{
  m_screen->fakeMouseButton(button, false);
}

void Screen::mouseMove(int32_t x, int32_t y)
{
  assert(!m_isPrimary);
  m_screen->fakeMouseMove(x, y);
}

void Screen::mouseRelativeMove(int32_t dx, int32_t dy) const
{
  assert(!m_isPrimary);
  m_screen->fakeMouseRelativeMove(dx, dy);
}

void Screen::mouseWheel(int32_t xDelta, int32_t yDelta) const
{
  assert(!m_isPrimary);
  m_screen->fakeMouseWheel({xDelta, yDelta});
}

void Screen::mouseWheelEx(const WheelEx &ex) const
{
  assert(!m_isPrimary);
  m_screen->fakeMouseWheelEx(ex);
}

void Screen::resetOptions()
{
  // reset options
  m_halfDuplex = 0;

  // let screen handle its own options
  m_screen->resetOptions();
}

void Screen::setOptions(const OptionsList &options)
{
  // update options
  for (uint32_t i = 0, n = (uint32_t)options.size(); i < n; i += 2) {
    if (options[i] == kOptionHalfDuplexCapsLock) {
      if (options[i + 1] != 0) {
        m_halfDuplex |= KeyModifierCapsLock;
      } else {
        m_halfDuplex &= ~KeyModifierCapsLock;
      }
      LOG_VERBOSE("half-duplex caps-lock %s", ((m_halfDuplex & KeyModifierCapsLock) != 0) ? "on" : "off");
    } else if (options[i] == kOptionHalfDuplexNumLock) {
      if (options[i + 1] != 0) {
        m_halfDuplex |= KeyModifierNumLock;
      } else {
        m_halfDuplex &= ~KeyModifierNumLock;
      }
      LOG_VERBOSE("half-duplex num-lock %s", ((m_halfDuplex & KeyModifierNumLock) != 0) ? "on" : "off");
    } else if (options[i] == kOptionHalfDuplexScrollLock) {
      if (options[i + 1] != 0) {
        m_halfDuplex |= KeyModifierScrollLock;
      } else {
        m_halfDuplex &= ~KeyModifierScrollLock;
      }
      LOG_VERBOSE("half-duplex scroll-lock %s", ((m_halfDuplex & KeyModifierScrollLock) != 0) ? "on" : "off");
    }
  }

  // update half-duplex options
  m_screen->setHalfDuplexMask(m_halfDuplex);

  // let screen handle its own options
  m_screen->setOptions(options);
}

void Screen::setSequenceNumber(uint32_t seqNum)
{
  m_screen->setSequenceNumber(seqNum);
}

uint32_t Screen::registerHotKey(KeyID key, KeyModifierMask mask)
{
  return m_screen->registerHotKey(key, mask);
}

void Screen::unregisterHotKey(uint32_t id)
{
  m_screen->unregisterHotKey(id);
}

void Screen::fakeInputBegin()
{
  assert(!m_fakeInput);

  m_fakeInput = true;
  m_screen->fakeInputBegin();
}

void Screen::fakeInputEnd()
{
  assert(m_fakeInput);

  m_fakeInput = false;
  m_screen->fakeInputEnd();
}

bool Screen::isOnScreen() const
{
  return m_entered;
}

bool Screen::isLockedToScreen() const
{
  if (uint32_t buttonID = 0; m_screen->isAnyMouseButtonDown(buttonID)) {
    LOG_DEBUG("locked by mouse buttonID: %d", buttonID);
    return true;
  }
  // not locked
  return false;
}

int32_t Screen::getJumpZoneSize() const
{
  if (!m_isPrimary) {
    return 0;
  } else {
    return m_screen->getJumpZoneSize();
  }
}

void Screen::getCursorCenter(int32_t &x, int32_t &y) const
{
  m_screen->getCursorCenter(x, y);
}

KeyModifierMask Screen::getActiveModifiers() const
{
  return m_screen->getActiveModifiers();
}

KeyModifierMask Screen::pollActiveModifiers() const
{
  return m_screen->pollActiveModifiers();
}

void *Screen::getEventTarget() const
{
  return m_screen;
}

bool Screen::getClipboard(ClipboardID id, IClipboard *clipboard) const
{
  return m_screen->getClipboard(id, clipboard);
}

void Screen::getShape(int32_t &x, int32_t &y, int32_t &w, int32_t &h) const
{
  m_screen->getShape(x, y, w, h);
}

void Screen::getCursorPos(int32_t &x, int32_t &y) const
{
  m_screen->getCursorPos(x, y);
}

void Screen::enablePrimary()
{
  // get notified of screen saver activation/deactivation
  m_screen->openScreensaver(true);

  // claim screen changed size
  m_events->addEvent(Event(EventTypes::ScreenShapeChanged, getEventTarget()));
}

void Screen::enableSecondary()
{
  // assume primary has all clipboards
  for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
    grabClipboard(id);
  }
}

void Screen::disablePrimary()
{
  // done with screen saver
  m_screen->closeScreensaver();
}

void Screen::disableSecondary()
{
  // done with screen saver
  m_screen->closeScreensaver();
}

void Screen::enterPrimary() const
{
  // Coming back to the primary means the server holds nothing here any
  // more, yet relayed keys ARE injected into its OS (PrimaryClient::
  // injectForwardedKey). Close what WE still hold before the user's first
  // real key lands on top. Ledger-only on purpose: the user may well be
  // mid-gesture at this keyboard (shift-dragging back onto the primary),
  // and the full sanitize sweep judges an OS-held modifier by freshness --
  // a hold emits one flagsChanged, so a >2 s shift-drag would be swept.
  m_screen->releaseInjectedKeys();
}

void Screen::enterSecondary(KeyModifierMask mask)
{
  // the server is about to type here: the delayed K2 sweep must not land
  // on top of a chord it is holding
  cancelEnableSweep();
  // The enter mask is the primary's OS truth at the crossing. Two things
  // ride in it that a fresh target must honour (I4: every boundary resyncs):
  //
  // 1. Lock state. Caps/Num/Scroll are STATE, never toggles (I2); apply the
  //    bits absolutely so a half-duplex Caps key cannot drift out of phase.
  applyToggleMask(mask);

  // 2. Physically held modifiers. The user crossed with Shift (or Ctrl...)
  //    held down; our OS has no such key down, so shift-click and
  //    modifier-only gestures would silently lose it. Press it here and
  //    track it as synthetic so leave (or the server's real key up) releases
  //    it. Only when the OS truth disagrees -- never double-press.
  //
  //    The mask handed to each press is the modifier state we want AFTER
  //    that press, built up one modifier at a time from what the OS already
  //    holds. Passing the whole enter mask (Win|Shift) as the desired state
  //    of the Shift press made mapKey() tap Super around it to "match" the
  //    mask -- a Start-menu flash on every Win+Shift crossing.
  const KeyModifierMask osMods = m_screen->pollActiveModifiers();
  KeyModifierMask desired = osMods & ~IKeyState::s_lockModifierMask;
  for (const auto &mod : kReassertedModifiers) {
    if ((mask & mod.bit) == 0 || (osMods & mod.bit) != 0) {
      continue;
    }
    if (m_reassertedModifiers.contains(mod.bit)) {
      continue;
    }
    desired |= mod.bit;
    LOG_DEBUG("re-asserting modifier 0x%04x held across enter (state 0x%04x)", mod.bit, desired);
    m_screen->fakeKeyDown(mod.key, desired, mod.button, std::string{});
    m_reassertedModifiers[mod.bit] = mod.button;
  }

  // 3. Verify. Whatever leave() and the server's release batch missed (a
  //    CLeave that raced the key-ups) shows up as a modifier the OS still
  //    holds while nothing has been typed here. Look once the dust settles,
  //    then once more before closing what our ledger still holds. The OS
  //    view is logged for the fleet; only the ledger is ever released, so
  //    a modifier held on this machine's own keyboard is never touched.
  m_enteredAt = std::chrono::steady_clock::now();
  m_postSwitchPass = 0;
  armPostSwitchVerifier(kPostSwitchFirstCheckS);
}

void Screen::leavePrimary()
{
  // we don't track keys while on the primary screen so update our
  // idea of them now.  this is particularly to update the state of
  // the toggle modifiers.
  m_screen->updateKeyState();
}

void Screen::leaveSecondary()
{
  cancelPostSwitchVerifier();
  // release any keys we think are still down (including modifiers
  // re-asserted on enter; fakeAllKeysUp covers every synthetic key AND,
  // per platform, whatever its injected-modifier ledger still holds beyond
  // the synthetic set -- K2 gap b1: OSXKeyState used to clear that ledger
  // here without releasing it). Deliberately no sanitize sweep: a modifier
  // held on this machine's own keyboard must survive the crossing.
  m_reassertedModifiers.clear();
  m_screen->fakeAllKeysUp();
}

void Screen::armEnableSweep()
{
  cancelEnableSweep();
  m_enableSweepTimer = m_events->newOneShotTimer(kEnableSweepDelayS, nullptr);
  if (m_enableSweepTimer == nullptr) {
    return; // event queues without timers (test doubles)
  }
  m_events->addHandler(EventTypes::Timer, m_enableSweepTimer, [this](const auto &) {
    cancelEnableSweep();
    if (m_entered) {
      return;
    }
    LOG_DEBUG("[keys] delayed enable sweep");
    m_screen->sanitizeInjectedKeys();
  });
}

void Screen::cancelEnableSweep()
{
  if (m_enableSweepTimer == nullptr) {
    return;
  }
  m_events->removeHandler(EventTypes::Timer, m_enableSweepTimer);
  m_events->deleteTimer(m_enableSweepTimer);
  m_enableSweepTimer = nullptr;
}

void Screen::armPostSwitchVerifier(double delayS)
{
  cancelPostSwitchVerifier();
  m_postSwitchTimer = m_events->newOneShotTimer(delayS, nullptr);
  if (m_postSwitchTimer == nullptr) {
    // event queues without timers (test doubles): nothing to verify with
    return;
  }
  m_events->addHandler(EventTypes::Timer, m_postSwitchTimer, [this](const auto &) { handlePostSwitchVerifier(); });
}

void Screen::cancelPostSwitchVerifier()
{
  if (m_postSwitchTimer == nullptr) {
    return;
  }
  m_events->removeHandler(EventTypes::Timer, m_postSwitchTimer);
  m_events->deleteTimer(m_postSwitchTimer);
  m_postSwitchTimer = nullptr;
}

void Screen::handlePostSwitchVerifier()
{
  // one-shot: it has fired, drop it before anything else
  cancelPostSwitchVerifier();
  if (m_isPrimary || !m_entered) {
    return;
  }
  ++m_postSwitchPass;

  // Lock bits are state, not held keys. Modifiers we re-asserted on enter
  // are tracked and have their own release path (the server's real key up
  // or leave), so they are not "stuck" however long the user holds them --
  // a shift-drag across the crossing must survive this.
  KeyModifierMask held = m_screen->pollActiveModifiers() & ~IKeyState::s_lockModifierMask;
  for (const auto &[bit, button] : m_reassertedModifiers) {
    held &= ~bit;
  }
  if (held == 0) {
    LOG_DEBUG("[keys] post-switch clear (pass %d)", m_postSwitchPass);
    return;
  }
  if (m_lastKeyDownAt > m_enteredAt) {
    // the server has typed here since we entered: whatever is down is the
    // user's own chord in progress, not a leftover
    LOG_DEBUG("[keys] post-switch user-held 0x%04x (typed since enter)", held);
    return;
  }
  if (m_postSwitchPass == 1) {
    LOG_INFO("[keys] post-switch held=0x%04x", held);
    armPostSwitchVerifier(kPostSwitchSecondCheckS);
    return;
  }
  LOG_WARN("[keys] stuck-release 0x%04x", held);
  // Close the ledger EXCEPT the modifiers we re-asserted on enter: those
  // are the user's ongoing chord (a shift-drag across the crossing) with
  // their own release path (keyUp / leaveSecondary), and m_reassertedModifiers
  // keeps describing exactly what is still down. Releasing the whole ledger
  // here dropped that Shift whenever some OTHER modifier read stuck.
  KeyModifierMask keep = 0;
  for (const auto &[bit, button] : m_reassertedModifiers) {
    keep |= bit;
  }
  m_screen->releaseInjectedKeys(keep);
}

std::string Screen::getSecureInputApp() const
{
  return m_screen->getSecureInputApp();
}

} // namespace deskflow
