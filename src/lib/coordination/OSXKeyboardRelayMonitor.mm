/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/KeyboardRelayMonitor.h"

#include "coordination/KeyboardRelayMap.h"

#include "base/Log.h"

#include <ApplicationServices/ApplicationServices.h>
#import <IOKit/hidsystem/ev_keymap.h>

#include <atomic>
#include <thread>

namespace deskflow::coordination {

namespace {

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
      return true;
    }
    m_passThrough = std::move(passThrough);
    m_send = std::move(send);
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

    // Consumer/media keys (volume, brightness, play/pause, ...) arrive as
    // system-defined events, not key down/up. Forward a single Down per
    // press; the injector's fakeMediaKey emits the full down+up on the target.
    if (type == static_cast<CGEventType>(NX_SYSDEFINED)) {
      // Ignore our own injected media events (genuine hardware has pid 0),
      // matching the standard-key path's guard against feedback loops.
      if (CGEventGetIntegerValueField(event, kCGEventSourceUnixProcessID) != 0) {
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
      if (self->m_send) {
        self->m_send(down ? Message::KeyPhase::Down : Message::KeyPhase::Up, mediaId, 0, 0, {});
      }
      return nullptr; // swallow so the key does not also act locally
    }

    if (type != kCGEventKeyDown && type != kCGEventKeyUp) {
      return event;
    }

    const auto sourcePid = CGEventGetIntegerValueField(event, kCGEventSourceUnixProcessID);
    if (sourcePid != 0) {
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
    if (!mapRelayKeyFromCgEvent(event, phase, id, mask, button)) {
      return event;
    }
    if (self->m_send) {
      self->m_send(phase, id, mask, button, {});
    }
    return nullptr;
  }

  void runLoop()
  {
    const CGEventMask mask =
        CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp) | CGEventMaskBit(NX_SYSDEFINED);

    m_tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault, mask, tapCallback, this);
    if (m_tap == nullptr) {
      LOG_WARN("coordination: keyboard relay tap unavailable (input monitoring permission?)");
      return;
    }

    m_runLoop = CFRunLoopGetCurrent();
    CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, m_tap, 0);
    CFRunLoopAddSource(m_runLoop, source, kCFRunLoopCommonModes);
    CGEventTapEnable(m_tap, true);
    LOG_DEBUG("coordination: keyboard relay monitor started");

    while (m_running) {
      CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, true);
    }

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
  CFMachPortRef m_tap = nullptr;
  CFRunLoopRef m_runLoop = nullptr;
};

} // namespace

std::unique_ptr<IKeyboardRelayMonitor> createKeyboardRelayMonitor()
{
  return std::make_unique<OSXKeyboardRelayMonitor>();
}

} // namespace deskflow::coordination
