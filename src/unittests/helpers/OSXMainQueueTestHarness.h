/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>

namespace deskflow::test::osx {

/// Qt Test must run on the process main thread (QCoreApplication::exec). Off-main-thread code
/// that calls `runOnMainQueue` needs the main CFRunLoop pumped while waiting.
inline void pumpMainQueueUntil(
    const std::atomic<bool> &finished, std::chrono::milliseconds timeout = std::chrono::seconds(5)
)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
  }
}

template <typename Fn>
auto runOffMainThreadWithMainQueuePump(Fn &&fn, std::chrono::milliseconds timeout = std::chrono::seconds(5))
    -> std::optional<decltype(fn())>
{
  using Result = decltype(fn());
  auto finished = std::make_shared<std::atomic<bool>>(false);
  auto result = std::make_shared<Result>();
  auto task = std::make_shared<std::decay_t<Fn>>(std::forward<Fn>(fn));

  std::thread worker([finished, result, task]() {
    *result = (*task)();
    finished->store(true, std::memory_order_release);
  });

  pumpMainQueueUntil(*finished, timeout);
  if (!finished->load(std::memory_order_acquire)) {
    worker.detach();
    return std::nullopt;
  }
  worker.join();
  return *result;
}

struct ScopedCGEvent
{
  CGEventRef ref = nullptr;

  explicit ScopedCGEvent(CGEventRef event) : ref(event) {}
  ScopedCGEvent(const ScopedCGEvent &) = delete;
  ScopedCGEvent &operator=(const ScopedCGEvent &) = delete;

  ~ScopedCGEvent()
  {
    if (ref != nullptr) {
      CFRelease(ref);
    }
  }

  CGEventRef get() const
  {
    return ref;
  }
};

} // namespace deskflow::test::osx
