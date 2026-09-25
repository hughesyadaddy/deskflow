/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/PlatformScreen.h"
#include "platform/OSXClipboard.h"
#include "platform/OSXPowerManager.h"

#include <Carbon/Carbon.h>
#include <IOKit/IOMessage.h>
#include <mach/mach_init.h>
#include <mach/mach_interface.h>
#include <mach/mach_port.h>

#include <atomic>
#include <bitset>
#include <map>
#include <memory>
#include <thread>
#include <vector>

extern "C"
{
  using CGSConnectionID = int;
  CGError CGSSetConnectionProperty(CGSConnectionID cid, CGSConnectionID targetCID, CFStringRef key, CFTypeRef value);
  int _CGSDefaultConnection();
}

template <class T> class CondVar;
class EventQueueTimer;
class Mutex;
class Thread;
class OSXKeyState;
class OSXScreenSaver;
class IEventQueue;
class Mutex;

//! Implementation of IPlatformScreen for OS X
class OSXScreen : public PlatformScreen
{
public:
  // clipboard polling: cheap NSPasteboard changeCount gate in front of the
  // heavy PasteboardSynchronize path. Pure helper so the gating rule is unit
  // testable without AppKit: returns true (and updates `last`) only when the
  // change count moved since the previous tick.
  // Lines to put on the wire for one non-continuous (wheel) scroll event.
  // macOS reports a slow notch as FixedPt 0.1 but DeltaAxis 1: the integer
  // is the line count apps scroll by, so it wins whenever it is set. FixedPt
  // only fills in the two cases it is better at -- a fast notch that carries
  // a fraction (>= 1 line) and a hi-res sub-notch tick the integer rounds
  // to zero.
  static double wheelLinesFromEvent(double fixedPtLines, int64_t wholeLines)
  {
    if (fixedPtLines >= 1.0 || fixedPtLines <= -1.0) {
      return fixedPtLines;
    }
    return wholeLines != 0 ? static_cast<double>(wholeLines) : fixedPtLines;
  }

  static bool clipboardChangeCountAdvanced(long &last, long current)
  {
    if (current == last) {
      return false;
    }
    last = current;
    return true;
  }

  //! K6: outcome of one mid-session secure-input poll tick.
  /*!
  Pure decision so the cadence/gating logic is unit testable without a real
  OSXScreen instance (no CGEventTap/CFRunLoop machinery involved).
  */
  enum class SecureInputTransition
  {
    None,     //!< no edge this tick (steady state, or not entered)
    TurnedOn, //!< false -> true: a password field just grabbed input
    TurnedOff //!< true -> false: the password field released input
  };

  //! Decide what a mid-session secure-input poll tick should do.
  /*!
  \p onScreen is the platform's \c m_isOnScreen (K6: this watcher only runs
  while entered -- the boundary sweep in \c Screen::enable() / its delayed
  timer already owns the not-entered case, so this always reports \c None
  then to avoid double-firing). \p wasSecureInputOn is the last observed
  state (mirrors \c m_secureInputLogged); \p isSecureInputOnNow is this
  tick's fresh read of \c IsSecureEventInputEnabled().
  */
  static SecureInputTransition
  secureInputWatchTick(bool onScreen, bool wasSecureInputOn, bool isSecureInputOnNow)
  {
    if (!onScreen) {
      return SecureInputTransition::None;
    }
    if (isSecureInputOnNow == wasSecureInputOn) {
      return SecureInputTransition::None;
    }
    return isSecureInputOnNow ? SecureInputTransition::TurnedOn : SecureInputTransition::TurnedOff;
  }

  //! Poll cadence (seconds) for the mid-session secure-input watcher.
  /*!
  K6: closes the gap left by the boundary-only sweep (Screen::enable() /
  Screen::kEnableSweepDelayS) -- a SecurityAgent/admin-auth dialog can pop
  up at any point while already entered and typing. Cheap: a single
  IsSecureEventInputEnabled() call per tick, never faster than ~1 Hz so it
  is not a CPU/battery cost.
  */
  static constexpr double kSecureInputPollSec = 1.0;

  OSXScreen(IEventQueue *events, bool isPrimary, bool enableLangSync = false);

  virtual ~OSXScreen();

  IEventQueue *getEvents() const
  {
    return m_events;
  }

  // IScreen overrides
  void *getEventTarget() const override;
  bool getClipboard(ClipboardID id, IClipboard *) const override;
  void getShape(int32_t &x, int32_t &y, int32_t &width, int32_t &height) const override;
  void getCursorPos(int32_t &x, int32_t &y) const override;

  // IPrimaryScreen overrides
  void reconfigure(uint32_t activeSides) override;
  uint32_t activeSides() override;
  void warpCursor(int32_t x, int32_t y) override;
  uint32_t registerHotKey(KeyID key, KeyModifierMask mask) override;
  void unregisterHotKey(uint32_t id) override;
  void fakeInputBegin() override;
  void fakeInputEnd() override;
  int32_t getJumpZoneSize() const override;
  bool isAnyMouseButtonDown(uint32_t &buttonID) const override;
  void getCursorCenter(int32_t &x, int32_t &y) const override;

  // ISecondaryScreen overrides
  void fakeMouseButton(ButtonID id, bool press) override;
  void fakeMouseMove(int32_t x, int32_t y) override;
  void fakeMouseRelativeMove(int32_t dx, int32_t dy) const override;
  void fakeMouseWheel(ScrollDelta delta) const override;
  void fakeMouseWheelEx(const WheelEx &ex) const override;

  // IPlatformScreen overrides
  void enable() override;
  void disable() override;
  void enter() override;
  bool canLeave() override;
  void leave() override;
  bool setClipboard(ClipboardID, const IClipboard *) override;
  void checkClipboards() override;
  void openScreensaver(bool notify) override;
  void closeScreensaver() override;
  void screensaver(bool activate) override;
  void resetOptions() override;
  void setOptions(const OptionsList &options) override;
  void setSequenceNumber(uint32_t) override;
  bool isPrimary() const override;
  std::string getSecureInputApp() const override;
  //! Log a "[keys] secure-input=on/off" line when the state changed since last look
  void logSecureInputState();

  void waitForCarbonLoop() const;

protected:
  // IPlatformScreen overrides
  void handleSystemEvent(const Event &e) override;
  void updateButtons() override;
  IKeyState *getKeyState() const override;

private:
  bool updateScreenShape();
  bool updateScreenShape(const CGDirectDisplayID, const CGDisplayChangeSummaryFlags);
  void postMouseEvent(CGPoint &) const;

  // convenience function to send events
  void sendEvent(EventTypes type, void * = nullptr) const;
  void sendClipboardEvent(EventTypes type, ClipboardID id) const;

  // message handlers
  bool onMouseMove();
  // mouse button handler.  pressed is true if this is a mousedown
  // event, false if it is a mouseup event.  macButton is the index
  // of the button pressed using the mac button mapping.
  bool onMouseButton(bool pressed, uint16_t macButton);
  bool onMouseWheel(int32_t xDelta, int32_t yDelta) const;
  bool onMouseWheelEx(const WheelEx &ex) const;

  void constructMouseButtonEventMap();

  bool onKey(CGEventRef event);

  void onMediaKey(CGEventRef event);

  bool onHotKey(EventRef event) const;

  // Added here to allow the carbon cursor hack to be called.
  void showCursor();
  void hideCursor();

  // map deskflow mouse button to mac buttons
  ButtonID mapDeskflowButtonToMac(uint16_t) const;

  // map mac mouse button to deskflow buttons
  ButtonID mapMacButtonToDeskflow(uint16_t) const;

  // map mac scroll wheel value to a deskflow scroll wheel value
  int32_t mapScrollWheelToDeskflow(int32_t) const;

  // decode a Quartz scroll event into the DMWX payload (device lines via
  // wheelLinesFromEvent, or pixels; no extra sender-side acceleration)
  WheelEx decodeScrollEvent(CGEventRef event) const;

  // Resolution switch callback
  static void displayReconfigurationCallback(CGDirectDisplayID, CGDisplayChangeSummaryFlags, void *);

  // fast user switch callback
  static pascal OSStatus userSwitchCallback(EventHandlerCallRef nextHandler, EventRef theEvent, void *inUserData);

  // sleep / wakeup support
  void watchSystemPowerThread(const void *);
  static void testCanceled(CFRunLoopTimerRef timer, void *info);
  static void powerChangeCallback(void *refcon, io_service_t service, natural_t messageType, void *messageArgument);
  void handlePowerChangeRequest(natural_t messageType, void *messageArgument);

  void handleConfirmSleep(const Event &event);

  bool checkAXPermissions();

  void clipboardPollTick();
  void armClipboardTimer();
  double clipboardPollInterval() const;

  // K6: mid-session secure-input watcher (see SecureInputTransition / kSecureInputPollSec).
  void secureInputPollTick();
  void armSecureInputTimer();
  void cancelSecureInputTimer();

  // global hotkey operating mode
  static bool isGlobalHotKeyOperatingModeAvailable();
  static void setGlobalHotKeysEnabled(bool enabled);
  static bool getGlobalHotKeysEnabled();

  // Quartz event tap support
  static CGEventRef handleCGInputEvent(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *refcon);
  static CGEventRef
  handleCGInputEventSecondary(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *refcon);

  // convert CFString to char*
  static char *CFStringRefToUTF8String(CFStringRef aString);

private:
  struct HotKeyItem
  {
  public:
    HotKeyItem(uint32_t, uint32_t);
    HotKeyItem(EventHotKeyRef, uint32_t, uint32_t);

    EventHotKeyRef getRef() const;

    bool operator<(const HotKeyItem &) const;

  private:
    EventHotKeyRef m_ref;
    uint32_t m_keycode;
    uint32_t m_mask;
  };

  enum EMouseButtonState
  {
    kMouseButtonUp = 0,
    kMouseButtonDragged,
    kMouseButtonDown,
    kMouseButtonStateMax
  };

  class MouseButtonState
  {
  public:
    void set(uint32_t button, EMouseButtonState state);
    bool any();
    void reset();
    void overwrite(uint32_t buttons);

    bool test(uint32_t button) const;
    int8_t getFirstButtonDown() const;

  private:
    std::bitset<NumButtonIDs> m_buttons;
  };

  using HotKeyMap = std::map<uint32_t, HotKeyItem>;
  using HotKeyIDList = std::vector<uint32_t>;
  using ModifierHotKeyMap = std::map<KeyModifierMask, uint32_t>;
  using HotKeyToIDMap = std::map<HotKeyItem, uint32_t>;

  // true if screen is being used as a primary screen, false otherwise
  bool m_isPrimary;

  // true if mouse has entered the screen
  bool m_isOnScreen;

  // the display
  CGDirectDisplayID m_displayID;

  uint32_t m_activeSides = 0;
  // screen shape stuff
  int32_t m_x, m_y;
  int32_t m_w, m_h;
  int32_t m_xCenter, m_yCenter;

  // mouse state
  mutable int32_t m_xCursor, m_yCursor;
  mutable bool m_cursorPosValid;
  //! Sub-line wheel remainder (client event thread only; fakeMouseWheel is const).
  mutable double m_wheelCarryX = 0.0;
  mutable double m_wheelCarryY = 0.0;

  /* FIXME: this data structure is explicitly marked mutable due
     to a need to track the state of buttons since the remote
     side only lets us know of change events, and because the
     fakeMouseButton button method is marked 'const'. This is
     Evil, and this should be moved to a place where it need not
     be mutable as soon as possible. */
  mutable MouseButtonState m_buttonState;
  using MouseButtonEventMapType = std::map<uint16_t, CGEventType>;
  std::vector<MouseButtonEventMapType> MouseButtonEventMap;

  bool m_cursorHidden;

  // keyboard stuff
  OSXKeyState *m_keyState;

  // clipboards
  OSXClipboard m_pasteboard;
  uint32_t m_sequenceNumber;

  // screen saver stuff
  OSXScreenSaver *m_screensaver;
  bool m_screensaverNotify;
  // last secure-input state written to the log as "[keys] secure-input=..."
  bool m_secureInputLogged = false;

  // clipboard stuff
  bool m_ownClipboard;
  EventQueueTimer *m_clipboardTimer;
  // interval the live m_clipboardTimer was armed with (0 = none)
  double m_clipboardTimerInterval = 0.0;
  // last NSPasteboard changeCount observed; -1 forces a sync on first tick
  long m_lastPasteboardChangeCount = -1;

  // low-rate watchdog: quits if accessibility trust is revoked at runtime
  EventQueueTimer *m_axTimer;

  // K6: mid-session secure-input watcher. Armed in enable(), cancelled in
  // disable(); the tick itself only acts while m_isOnScreen (see
  // secureInputWatchTick()).
  EventQueueTimer *m_secureInputTimer = nullptr;

  // window object that gets user input events when the server
  // has focus.
  WindowRef m_hiddenWindow;
  // window object that gets user input events when the server
  // does not have focus.
  WindowRef m_userInputWindow;

  // fast user switching
  EventHandlerRef m_switchEventHandlerRef;

  // sleep / wakeup
  Mutex *m_pmMutex;
  Thread *m_pmWatchThread;
  CondVar<bool> *m_pmThreadReady;
  CFRunLoopRef m_pmRunloop;
  //! Set by the destructor before it stops the power run loop, so a thread
  //! that has not entered CFRunLoopRun yet skips it instead of running a
  //! loop nobody will stop.
  std::atomic<bool> m_pmStopRequested{false};
  io_connect_t m_pmRootPort;

  // hot key stuff
  HotKeyMap m_hotKeys;
  HotKeyIDList m_oldHotKeyIDs;
  ModifierHotKeyMap m_modifierHotKeys;
  uint32_t m_activeModifierHotKey;
  KeyModifierMask m_activeModifierHotKeyMask;
  HotKeyToIDMap m_hotKeyToIDMap;

  // global hotkey operating mode
  static bool s_testedForGHOM;
  static bool s_hasGHOM;

  // Quartz input event support
  CFMachPortRef m_eventTapPort;
  CFRunLoopSourceRef m_eventTapRLSR;
  std::thread m_eventTapThread;
  CFRunLoopRef m_eventTapRunLoop = nullptr;

  // for double click coalescing.
  double m_lastClickTime;
  int m_clickState;
  int32_t m_lastSingleClickXCursor;
  int32_t m_lastSingleClickYCursor;

  IEventQueue *m_events;

  std::unique_ptr<Thread> m_getDropTargetThread;
  std::string m_dropTarget;

  Mutex *m_carbonLoopMutex;
  CondVar<bool> *m_carbonLoopReady;

  OSXPowerManager m_powerManager;

  class OSXScreenImpl *m_impl;
};
