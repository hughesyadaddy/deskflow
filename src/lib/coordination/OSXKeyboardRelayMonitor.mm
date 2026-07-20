/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/KeyboardRelayMonitor.h"

#include "coordination/KeyboardRelayHookPolicy.h"
#include "coordination/KeyboardRelayMap.h"

#include "base/Log.h"

#include <ApplicationServices/ApplicationServices.h>
#import <IOKit/hidsystem/ev_keymap.h>

#include <atomic>
#include <thread>

namespace deskflow::coordination {

namespace {

bool relayEventIsInjected(CGEventRef event)
{
  return CGEventGetIntegerValueField(event, kCGEventSourceUnixProcessID) == getpid();
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
      if (m_active) {
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
    if (m_runLoop != nullptr) {
      CFRunLoopStop(m_runLoop);
    }
    if (m_thread.joinable()) {
      m_thread.join();
    }
    m_runLoop = nullptr;
  }

  bool running() const override
  {
    return m_active;
  }

private:
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
      bool forwarded = false;
      if (self->m_send) {
        forwarded = self->m_send(down ? Message::KeyPhase::Down : Message::KeyPhase::Up, mediaId, 0, 0, {});
      }
      return relaySwallowDecision(event, false, false, true, forwarded);
    }

    if (type == kCGEventFlagsChanged) {
      if (isInjected) {
        return event;
      }

      const bool passLocal = self->m_passThrough ? self->m_passThrough() : true;
      if (passLocal) {
        return event;
      }

      Message::KeyPhase phase = Message::KeyPhase::Down;
      KeyID id = 0;
      KeyModifierMask mask = 0;
      KeyButton button = 0;
      if (!mapRelayModifierFromCgEvent(event, phase, id, mask, button, self->m_capsLockOn)) {
        return event;
      }
      bool forwarded = false;
      if (self->m_send) {
        forwarded = self->m_send(phase, id, mask, button, {});
      }
      return relaySwallowDecision(event, false, false, true, forwarded);
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

    Message::KeyPhase phase = Message::KeyPhase::Down;
    KeyID id = 0;
    KeyModifierMask mask = 0;
    KeyButton button = 0;
    if (!mapRelayKeyFromCgEvent(event, phase, id, mask, button)) {
      return event;
    }

    const bool passLocal = self->m_passThrough ? self->m_passThrough() : true;
    // When keys stay local, still deliver Downs to sendKeyForward so 5× Esc
    // restart can observe taps (return true = swallow this key).
    if (passLocal) {
      if (type == kCGEventKeyDown && phase == Message::KeyPhase::Down && self->m_send &&
          self->m_send(phase, id, mask, button, {})) {
        return nullptr;
      }
      return event;
    }

    bool forwarded = false;
    if (self->m_send) {
      forwarded = self->m_send(phase, id, mask, button, {});
    }
    return relaySwallowDecision(event, false, false, true, forwarded);
  }

  void runLoop()
  {
    const CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp) |
                             CGEventMaskBit(kCGEventFlagsChanged) | CGEventMaskBit(NX_SYSDEFINED);

    m_tap =
        CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault, mask, tapCallback, this);
    if (m_tap == nullptr) {
      LOG_WARN("coordination: keyboard relay tap unavailable (input monitoring permission?)");
      return;
    }

    m_runLoop = CFRunLoopGetCurrent();
    CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, m_tap, 0);
    CFRunLoopAddSource(m_runLoop, source, kCFRunLoopCommonModes);
    CGEventTapEnable(m_tap, true);
    m_active = true;
    LOG_DEBUG("coordination: keyboard relay monitor started");

    while (m_running) {
      CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, true);
    }

    m_active = false;
    CGEventTapEnable(m_tap, false);
    CFRunLoopRemoveSource(m_runLoop, source, kCFRunLoopCommonModes);
    CFRelease(source);
    CFRelease(m_tap);
    m_tap = nullptr;
    LOG_DEBUG("coordination: keyboard relay monitor stopped");
  }

  RelayPassThroughQuery m_passThrough;
  KeyForwardSend m_send;
  std::thread m_thread;
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_active{false}; //!< tap installed and pumping
  CFMachPortRef m_tap = nullptr;
  bool m_capsLockOn = false;
  CFRunLoopRef m_runLoop = nullptr;
};

} // namespace

std::unique_ptr<IKeyboardRelayMonitor> createKeyboardRelayMonitor()
{
  return std::make_unique<OSXKeyboardRelayMonitor>();
}

} // namespace deskflow::coordination
