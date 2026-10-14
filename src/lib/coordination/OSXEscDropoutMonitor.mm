/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/EscDropout.h"

#include "base/Log.h"
#include "platform/OSXInjectedEvent.h"

#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace deskflow::coordination {

namespace {

//! Listen-only tap at kCGHIDEventTap: the earliest point user space can
//! observe keys -- before the primary tap and the keyboard relay (session
//! taps) can swallow them, and independent of the network and the main loop.
class OSXEscDropoutMonitor : public IEscDropoutMonitor
{
public:
  ~OSXEscDropoutMonitor() override
  {
    stop();
  }

  bool start(std::function<void()> onBurst) override
  {
    if (m_thread.joinable()) {
      return true;
    }
    m_onBurst = std::move(onBurst);
    m_running = true;
    m_thread = std::thread([this] { runLoop(); });
    return true;
  }

  void stop() override
  {
    m_running = false;
    {
      std::scoped_lock lock(m_mutex);
      if (m_runLoop != nullptr) {
        CFRunLoopStop(m_runLoop);
      }
    }
    if (m_thread.joinable()) {
      m_thread.join();
    }
    std::scoped_lock lock(m_mutex);
    if (m_runLoop != nullptr) {
      CFRelease(m_runLoop);
      m_runLoop = nullptr;
    }
  }

private:
  static CGEventRef tapCallback(CGEventTapProxy, CGEventType type, CGEventRef event, void *refcon)
  {
    auto *self = static_cast<OSXEscDropoutMonitor *>(refcon);
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
      if (self->m_tap != nullptr) {
        CGEventTapEnable(self->m_tap, true);
      }
      return event;
    }
    if (type != kCGEventKeyDown) {
      return event;
    }
    // physical keys only: nothing this process (or any process) posted
    if (deskflow::platform::isInjectedEvent(event) ||
        CGEventGetIntegerValueField(event, kCGEventSourceUnixProcessID) == getpid()) {
      return event;
    }
    const auto vk = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
    if (deskflow::platform::wasJustInjected(static_cast<int>(vk))) {
      return event; // the server typing here (relayed/forwarded Esc), not this keyboard
    }
    const bool repeat = CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat) != 0;
    if (self->m_burst.noteKeyDown(vk == kVK_Escape, repeat, EscTapBurst::Clock::now()) && self->m_onBurst) {
      self->m_onBurst();
    }
    return event; // listen-only: never swallowed
  }

  void runLoop()
  {
    m_tap = CGEventTapCreate(
        kCGHIDEventTap, kCGHeadInsertEventTap, kCGEventTapOptionListenOnly, CGEventMaskBit(kCGEventKeyDown),
        tapCallback, this
    );
    if (m_tap == nullptr) {
      LOG_WARN("[esc-dropout] local Esc x5 monitor unavailable (input monitoring permission?)");
      m_running = false;
      return;
    }
    CFRunLoopRef loop = CFRunLoopGetCurrent();
    {
      std::scoped_lock lock(m_mutex);
      m_runLoop = (CFRunLoopRef)CFRetain(loop);
    }
    CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, m_tap, 0);
    CFRunLoopAddSource(loop, source, kCFRunLoopCommonModes);
    CGEventTapEnable(m_tap, true);
    LOG_INFO("[esc-dropout] local Esc x5 monitor armed (HID level, listen-only)");
    while (m_running) {
      CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, true);
    }
    CGEventTapEnable(m_tap, false);
    CFRunLoopRemoveSource(loop, source, kCFRunLoopCommonModes);
    CFRelease(source);
    CFRelease(m_tap);
    m_tap = nullptr;
  }

  std::function<void()> m_onBurst;
  std::atomic<bool> m_running{false};
  std::thread m_thread;
  std::mutex m_mutex;
  CFRunLoopRef m_runLoop = nullptr;
  CFMachPortRef m_tap = nullptr;
  EscTapBurst m_burst; // tap thread only
};

} // namespace

std::unique_ptr<IEscDropoutMonitor> createEscDropoutMonitor()
{
  return std::make_unique<OSXEscDropoutMonitor>();
}

} // namespace deskflow::coordination
