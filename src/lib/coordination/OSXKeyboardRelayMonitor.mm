/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/KeyboardRelayMonitor.h"

#include "coordination/KeyboardRelayHookPolicy.h"
#include "coordination/KeyboardRelayMap.h"

#include "base/Log.h"
#include "platform/OSXInjectedEvent.h"

#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#import <IOKit/hidsystem/ev_keymap.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace deskflow::coordination {

namespace {

bool relayEventIsInjected(CGEventRef event)
{
  return CGEventGetIntegerValueField(event, kCGEventSourceUnixProcessID) == getpid() ||
         deskflow::platform::isInjectedEvent(event);
}

CGEventRef relaySwallowDecision(CGEventRef event, bool passLocal, bool isInjected, bool mapped, bool forwarded)
{
  const KeyboardRelayHookContext ctx{passLocal, isInjected, mapped, forwarded};
  return keyboardRelayHookShouldPassThrough(ctx) ? event : nullptr;
}

class OSXKeyboardRelayMonitor : public IKeyboardRelayMonitor
{
public:
  ~OSXKeyboardRelayMonitor() override
  {
    stop();
  }

  bool start(RelayPassThroughQuery passThrough, KeyForwardSend send) override
  {
    if (m_thread.joinable()) {
      if (m_running) {
        return true;
      }
      // Thread finished without a live tap (permission/transient failure):
      // reap it so the retry below can actually start a fresh one.
      stop();
    }
    m_passThrough = std::move(passThrough);
    m_send = std::move(send);
    // Seed the caps toggle tracker from the live system state so the first
    // caps press relays as a change instead of being swallowed as a no-op.
    m_capsLockOn = (CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState) & kCGEventFlagMaskAlphaShift) != 0;
    m_running = true;
    m_thread = std::thread([this] { runLoop(); });
    return true;
  }

  void stop() override
  {
    m_running = false;
    {
      // m_runLoop is written by the monitor thread under the same mutex, so
      // this either sees nullptr (not captured yet, or tap creation failed;
      // the thread will observe m_running == false and exit on its own) or a
      // RETAINED loop that stays valid even if the thread has already exited.
      std::lock_guard<std::mutex> lock(m_runLoopMutex);
      if (m_runLoop != nullptr) {
        CFRunLoopStop(m_runLoop);
      }
    }
    if (m_thread.joinable()) {
      m_thread.join();
    }
    releaseRunLoop();
    flushLedger();
  }

  bool running() const override
  {
    return m_active;
  }

  void setForwardedReleaseSink(ForwardedReleaseSink sink) override
  {
    m_releaseSink = std::move(sink);
  }

  void releaseForwardedLocally() override
  {
    m_ledgerResyncPending = true;
  }

private:
  //! Boundary (tap thread joined): report every forwarded hold so the
  //! coordinator posts its Up, then forget everything. The ledger outlives
  //! epoch flips (the monitor object does), so without this a key held
  //! across a stop stayed pressed on the peer with nobody left to release it.
  void flushLedger()
  {
    const auto held = m_ledger.forwardedButtons();
    if (!held.empty() && m_releaseSink) {
      std::vector<KeyButton> buttons;
      buttons.reserve(held.size());
      for (const int button : held) {
        buttons.push_back(static_cast<KeyButton>(button));
      }
      LOG_DEBUG("coordination: relay stop releasing %zu forwarded key(s) on the peer", buttons.size());
      m_releaseSink(buttons);
    }
    m_ledger.clear();
    m_ledgerResyncPending = false;
  }

  //! Tap thread: apply a pending cross-thread resync before touching the ledger.
  void applyPendingResync()
  {
    if (m_ledgerResyncPending.exchange(false)) {
      m_ledger.releaseAllForwardedLocally();
    }
  }

  KeyForwardResult send(Message::KeyPhase phase, KeyID id, KeyModifierMask mask, KeyButton button)
  {
    return m_send ? m_send(phase, id, mask, button, {}) : KeyForwardResult::Local;
  }

  static CGEventRef tapCallback(CGEventTapProxy, CGEventType type, CGEventRef event, void *refcon)
  {
    auto *self = static_cast<OSXKeyboardRelayMonitor *>(refcon);
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
      if (self->m_tap != nullptr) {
        CGEventTapEnable(self->m_tap, true);
      }
      return event;
    }

    const bool isInjected = relayEventIsInjected(event);
    self->applyPendingResync();

    // Consumer/media keys (volume, brightness, play/pause, ...) arrive as
    // system-defined events, not key down/up. Forward a single Down per
    // press; the injector's fakeMediaKey emits the full down+up on the target.
    if (type == static_cast<CGEventType>(NX_SYSDEFINED)) {
      // Only ignore OUR OWN injected media events (fakeNativeMediaKey posts
      // from this process). Genuine hardware media keys are re-posted by
      // macOS system processes and arrive with a NON-zero source pid, so a
      // pid != 0 guard silently ate every real brightness/volume press.
      if (isInjected) {
        return event;
      }
      KeyID mediaId = kKeyNone;
      bool down = false;
      if (!mapRelayMediaKeyFromCgEvent(event, mediaId, down)) {
        return event;
      }
      const bool passLocal = self->m_passThrough ? self->m_passThrough() : true;
      if (passLocal) {
        return event;
      }
      // Forward a matched Down/Up pair. A macOS target's fakeMediaKey emits a
      // full tap on the Down (the Up is a no-op there); a Windows target maps
      // the media KeyID like a normal key and needs the Up to release the VK,
      // otherwise it stays logically held. Both halves are swallowed locally.
      LOG_DEBUG("coordination: relaying media key 0x%04x %s", mediaId, down ? "down" : "up");
      const auto result = self->send(down ? Message::KeyPhase::Down : Message::KeyPhase::Up, mediaId, 0, 0);
      return relaySwallowDecision(event, false, false, true, result != KeyForwardResult::Local);
    }

    if (type == kCGEventFlagsChanged) {
      return self->onFlagsChanged(event, isInjected);
    }

    if (type != kCGEventKeyDown && type != kCGEventKeyUp) {
      return event;
    }

    // Feedback-loop guard: never re-relay keys this process injected (the
    // server typing into this screen posts from deskflow-core itself).
    // Other processes' synthetic keys (dictation tools, Karabiner) are
    // user input and must relay like hardware.
    if (isInjected) {
      return event;
    }

    return self->onKey(event, type);
  }

  CGEventRef onFlagsChanged(CGEventRef event, bool isInjected)
  {
    // Keep the caps toggle tracker true to the OS even when this event
    // will not relay (local mode or injected): a stale tracker would make
    // the next genuine remote caps press look like a no-op state and be
    // swallowed (KeyboardRelayMap.mm relays caps per state CHANGE).
    const auto vk = static_cast<CGKeyCode>(CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
    const bool isCapsEvent = vk == kVK_CapsLock;
    const bool capsNowOn = (CGEventGetFlags(event) & kCGEventFlagMaskAlphaShift) != 0;
    const auto button = static_cast<KeyButton>(vk);

    if (isInjected) {
      if (isCapsEvent) {
        m_capsLockOn = capsNowOn;
      }
      return event;
    }

    // Modifiers arrive here as flagsChanged, so they need the SAME
    // destination ledger as ordinary keys, consulted FIRST -- before the
    // cursor's current screen. A modifier pressed while the cursor was
    // remote (Down forwarded) and released after it came home must still
    // release on the PEER, or Shift/Cmd stays held there (every keystroke
    // typed there afterwards is uppercase / a shortcut). Likewise a
    // modifier pressed while local must release locally even if the cursor
    // is remote by now. Only a fresh Down consults the cursor.
    const auto destination = m_ledger.destination(button);
    const bool passLocal = m_passThrough ? m_passThrough() : true;
    if (keyboardRelayRouteUp(destination, passLocal) == KeyboardRelayUpRoute::Forward) {
      Message::KeyPhase phase = Message::KeyPhase::Down;
      KeyID id = 0;
      KeyModifierMask mask = 0;
      KeyButton mappedButton = 0;
      bool isUp = false;
      if (isCapsEvent) {
        // Caps: the state-changing edge is the press; the state-preserving
        // edge (tracker unchanged) is the release, which the tracker
        // cannot express -- so decide it here from the tracker outcome.
        isUp = !mapRelayModifierFromCgEvent(event, phase, id, mask, mappedButton, m_capsLockOn);
        mask = 0;
      } else {
        isUp = mapRelayModifierFromCgEvent(event, phase, id, mask, mappedButton, m_capsLockOn) &&
               phase == Message::KeyPhase::Up;
      }
      if (!isUp) {
        // A second press edge with no release seen in between (the hook
        // missed it): release the peer's hold and press again so the peer
        // sees the same edges the user produced.
        (void)send(Message::KeyPhase::Up, kKeyNone, 0, button);
        const auto again = send(phase, id, mask, button);
        if (again != KeyForwardResult::Forwarded) {
          m_ledger.downLocal(button);
        }
        return again != KeyForwardResult::Local ? nullptr : event;
      }
      m_ledger.release(button);
      const auto result = send(Message::KeyPhase::Up, kKeyNone, mask, button);
      return result != KeyForwardResult::Local ? nullptr : event;
    }

    if (passLocal || destination == KeyboardRelayForwardLedger::Destination::Local) {
      if (isCapsEvent) {
        m_capsLockOn = capsNowOn;
        m_ledger.release(button);
      } else {
        // Record the local destination so a later Up (possibly after the
        // cursor moved remote) is not swallowed away from this machine.
        if ((CGEventGetFlags(event) & (kCGEventFlagMaskShift | kCGEventFlagMaskControl | kCGEventFlagMaskAlternate |
                                       kCGEventFlagMaskCommand)) != 0) {
          m_ledger.downLocal(button);
        } else {
          m_ledger.release(button);
        }
      }
      return event;
    }

    Message::KeyPhase phase = Message::KeyPhase::Down;
    KeyID id = 0;
    KeyModifierMask mask = 0;
    KeyButton mappedButton = 0;
    if (!mapRelayModifierFromCgEvent(event, phase, id, mask, mappedButton, m_capsLockOn)) {
      // Caps release edge with no forwarded hold, or an unknown key.
      return event;
    }
    if (phase == Message::KeyPhase::Up) {
      return event; // its Down went local (or was never seen)
    }

    const auto result = send(phase, id, mask, button);
    switch (result) {
    case KeyForwardResult::Forwarded:
      m_ledger.downForwarded(button);
      break;
    default:
      m_ledger.downLocal(button);
      break;
    }
    return relaySwallowDecision(event, false, false, true, result != KeyForwardResult::Local);
  }

  CGEventRef onKey(CGEventRef event, CGEventType type)
  {
    // Phase and button are cheap (event fields); the KeyID needs the
    // keyboard layout, which lives on the main queue (dispatch_sync from
    // this tap thread). Only a Down that is actually leaving the machine
    // pays for it: in local mode a stalled main thread would otherwise
    // stall the tap on every keystroke, and past the tap timeout macOS
    // delivers the key locally while the callback still forwards it.
    const auto vk = static_cast<CGKeyCode>(CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
    const auto button = static_cast<KeyButton>(vk);
    const bool isUp = type == kCGEventKeyUp;
    const bool isRepeat = !isUp && CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat) != 0;

    // Repeat/Up of a held key follow the DOWN's destination (ledger), not the
    // cursor's current screen: a mid-hold screen switch must not strand the
    // key held on one side with its release delivered to the other.
    if (isUp) {
      const auto destination = m_ledger.destination(button);
      if (destination == KeyboardRelayForwardLedger::Destination::Local) {
        // Its Down was delivered locally: the Up must be too, or the key
        // never releases on this machine.
        m_ledger.release(button);
        return event;
      }
      Message::KeyPhase phase = Message::KeyPhase::Up;
      KeyID id = kKeyNone;
      KeyModifierMask mask = 0;
      KeyButton mappedButton = 0;
      mapRelayKeyFromCgEvent(event, phase, id, mask, mappedButton); // Up: no layout lookup
      if (destination == KeyboardRelayForwardLedger::Destination::Unknown) {
        // Genuinely unseen Down (ledger lost mid-hold): if the cursor is
        // remote, forward-and-swallow so the target never keeps it held.
        const bool passLocalNow = m_passThrough ? m_passThrough() : true;
        if (!passLocalNow && send(phase, id, mask, button) != KeyForwardResult::Local) {
          return nullptr;
        }
        return event;
      }
      m_ledger.release(button);
      return send(phase, id, mask, button) != KeyForwardResult::Local ? nullptr : event;
    }

    if (isRepeat && !m_ledger.follow(button)) {
      return event; // Down stayed local
    }

    // Fresh Down: destination is decided by where the cursor is NOW.
    const bool passLocal = !isRepeat && (m_passThrough ? m_passThrough() : true);
    if (passLocal) {
      m_ledger.downLocal(button);
      // Still hand the Down to the coordinator so 5x Esc rescue can observe
      // taps while the cursor is local -- but WITHOUT the layout lookup:
      // Esc (and every other rescue-relevant key) maps from the fixed
      // table, and a glyph key only has to break the Esc streak, which a
      // kKeyNone id does just as well.
      Message::KeyPhase phase = Message::KeyPhase::Down;
      KeyID id = kKeyNone;
      KeyModifierMask mask = 0;
      KeyButton mappedButton = 0;
      mapRelayKeyFromCgEvent(event, phase, id, mask, mappedButton, false);
      if (send(phase, id, mask, button) == KeyForwardResult::Swallowed) {
        return nullptr;
      }
      return event;
    }

    Message::KeyPhase phase = Message::KeyPhase::Down;
    KeyID id = 0;
    KeyModifierMask mask = 0;
    KeyButton mappedButton = 0;
    const bool mapped = mapRelayKeyFromCgEvent(event, phase, id, mask, mappedButton);
    if (isRepeat) {
      if (!mapped) {
        return event;
      }
      return send(phase, id, mask, button) != KeyForwardResult::Local ? nullptr : event;
    }

    KeyForwardResult result = KeyForwardResult::Local;
    if (mapped) {
      result = send(phase, id, mask, button);
    }
    switch (result) {
    case KeyForwardResult::Forwarded:
      m_ledger.downForwarded(button);
      break;
    case KeyForwardResult::Swallowed:
      // Consumed by the rescue gesture: never seen by either OS, so its Up
      // must not chase a hold on the peer. Local = the Up passes through
      // here as a harmless no-op.
      m_ledger.downLocal(button);
      return nullptr;
    default:
      m_ledger.downLocal(button);
      break;
    }
    return relaySwallowDecision(event, false, false, mapped, result == KeyForwardResult::Forwarded);
  }

  void runLoop()
  {
    const CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp) |
                             CGEventMaskBit(kCGEventFlagsChanged) | CGEventMaskBit(NX_SYSDEFINED);

    m_tap =
        CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault, mask, tapCallback, this);
    if (m_tap == nullptr) {
      LOG_WARN("coordination: keyboard relay tap unavailable (input monitoring permission?)");
      m_running = false;
      return;
    }

    // Retain the run loop (same fix as OSXScreen, 843d0743b): the loop is
    // owned by this thread and freed on thread exit. stop() sets m_running
    // first, so this thread can finish its 250ms slice and exit BEFORE stop()
    // reaches CFRunLoopStop -- without the retain that would trap on a freed
    // loop (SIGTRAP in __CFCheckCFInfoPACSignature). stop() releases it after
    // join; the destructor covers the never-started/failed-tap paths.
    CFRunLoopRef runLoop = CFRunLoopGetCurrent();
    {
      std::lock_guard<std::mutex> lock(m_runLoopMutex);
      m_runLoop = (CFRunLoopRef)CFRetain(runLoop);
    }
    CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, m_tap, 0);
    CFRunLoopAddSource(runLoop, source, kCFRunLoopCommonModes);
    CGEventTapEnable(m_tap, true);
    m_active = true;
    LOG_DEBUG("coordination: keyboard relay monitor started");

    while (m_running) {
      CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, true);
    }

    m_active = false;
    // Every CF object created per start (tap + source) is released here so
    // repeated start/stop (auto-mode epoch flips) cannot accumulate them.
    CGEventTapEnable(m_tap, false);
    CFRunLoopRemoveSource(runLoop, source, kCFRunLoopCommonModes);
    CFRelease(source);
    CFRelease(m_tap);
    m_tap = nullptr;
    LOG_DEBUG("coordination: keyboard relay monitor stopped");
  }

  //! Drop the retained run loop. Only call with the monitor thread joined.
  void releaseRunLoop()
  {
    std::lock_guard<std::mutex> lock(m_runLoopMutex);
    if (m_runLoop != nullptr) {
      CFRelease(m_runLoop);
      m_runLoop = nullptr;
    }
  }

  RelayPassThroughQuery m_passThrough;
  KeyForwardSend m_send;
  ForwardedReleaseSink m_releaseSink;
  std::thread m_thread;
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_active{false}; //!< tap installed and pumping
  CFMachPortRef m_tap = nullptr;
  KeyboardRelayForwardLedger m_ledger;             //!< tap thread only (stop() after join)
  std::atomic<bool> m_ledgerResyncPending{false}; //!< releaseForwardedLocally() -> tap thread
  bool m_capsLockOn = false;
  std::mutex m_runLoopMutex;         //!< guards m_runLoop between stop() and the monitor thread
  CFRunLoopRef m_runLoop = nullptr; //!< retained; released by stop() after join
};

} // namespace

std::unique_ptr<IKeyboardRelayMonitor> createKeyboardRelayMonitor()
{
  return std::make_unique<OSXKeyboardRelayMonitor>();
}

} // namespace deskflow::coordination
