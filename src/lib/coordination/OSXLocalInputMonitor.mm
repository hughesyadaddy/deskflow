/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/LocalInputMonitor.h"

#include "base/Log.h"

#include <ApplicationServices/ApplicationServices.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace deskflow::coordination {

namespace {

//! Listen-only CGEventTap on its own CFRunLoop thread.
/*!
Genuine hardware events carry an event-source unix PID of 0; anything
injected by deskflow (or any other process) carries the injector's PID
and is ignored. Replaces the external inputmon.swift helper.
*/
class OSXLocalInputMonitor : public ILocalInputMonitor
{
public:
  ~OSXLocalInputMonitor() override
  {
    stop();
  }

  bool start(Callback onGenuineInput) override
  {
    if (m_thread.joinable()) {
      if (m_active) {
        return true;
      }
      // Thread finished without a live tap (permission/transient failure):
      // reap it so the retry below can actually start a fresh one.
      stop();
    }
    m_callback = std::move(onGenuineInput);
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
  }

private:
  static CGEventRef tapCallback(CGEventTapProxy, CGEventType type, CGEventRef event, void *refcon)
  {
    auto *self = static_cast<OSXLocalInputMonitor *>(refcon);

    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
      // The OS disables stalled taps; listen-only taps still must re-arm.
      if (self->m_tap != nullptr) {
        CGEventTapEnable(self->m_tap, true);
      }
      return event;
    }

    const auto sourcePid = CGEventGetIntegerValueField(event, kCGEventSourceUnixProcessID);
    if (sourcePid == 0 && self->m_callback) {
      self->m_callback();
    }
    return event;
  }

  void runLoop()
  {
    const CGEventMask mask = CGEventMaskBit(kCGEventMouseMoved) | CGEventMaskBit(kCGEventLeftMouseDown) |
                             CGEventMaskBit(kCGEventRightMouseDown) | CGEventMaskBit(kCGEventOtherMouseDown) |
                             CGEventMaskBit(kCGEventScrollWheel);

    m_tap = CGEventTapCreate(
        kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionListenOnly, mask, tapCallback, this
    );
    if (m_tap == nullptr) {
      LOG_WARN("coordination: could not create input event tap (missing input monitoring permission?)");
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
    LOG_DEBUG("coordination: local input monitor started");

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
    LOG_DEBUG("coordination: local input monitor stopped");
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

  Callback m_callback;
  std::thread m_thread;
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_active{false}; //!< tap installed and pumping
  CFMachPortRef m_tap = nullptr;
  std::mutex m_runLoopMutex;         //!< guards m_runLoop between stop() and the monitor thread
  CFRunLoopRef m_runLoop = nullptr; //!< retained; released by stop() after join
};

} // namespace

std::unique_ptr<ILocalInputMonitor> createLocalInputMonitor()
{
  return std::make_unique<OSXLocalInputMonitor>();
}

} // namespace deskflow::coordination
