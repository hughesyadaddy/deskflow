/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/EscDropout.h"

#include "base/Log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace deskflow::coordination {

namespace {

class MSWindowsEscDropoutMonitor;
MSWindowsEscDropoutMonitor *g_instance = nullptr;

//! WH_KEYBOARD_LL observer: sees physical keys before any hook installed
//! later (the keyboard relay) can swallow them; injected keys are skipped.
class MSWindowsEscDropoutMonitor : public IEscDropoutMonitor
{
public:
  ~MSWindowsEscDropoutMonitor() override
  {
    stop();
  }

  bool start(std::function<void()> onBurst) override
  {
    if (m_thread.joinable()) {
      return true;
    }
    m_onBurst = std::move(onBurst);
    m_thread = std::thread([this] { run(); });
    return true;
  }

  void stop() override
  {
    if (m_threadId != 0) {
      PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
    }
    if (m_thread.joinable()) {
      m_thread.join();
    }
    m_threadId = 0;
  }

private:
  static LRESULT CALLBACK hookProc(int code, WPARAM wParam, LPARAM lParam)
  {
    if (code == HC_ACTION) {
      const auto *info = reinterpret_cast<KBDLLHOOKSTRUCT *>(lParam);
      const bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
      escDropoutObserveWindowsKey(info->vkCode, down, (info->flags & LLKHF_INJECTED) != 0, info->time);
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
  }

public:
  void observe(unsigned vk, bool down, bool injected, unsigned long time)
  {
    if (injected) {
      return;
    }
    std::scoped_lock lock(m_observeMutex);
    if (time == m_lastTime && vk == m_lastVk && down == m_lastDown) {
      return; // same OS event seen by the relay hook and by ours
    }
    m_lastTime = time;
    m_lastVk = vk;
    m_lastDown = down;
    const bool isEsc = vk == VK_ESCAPE;
    if (!down) {
      if (isEsc) {
        m_escDown = false;
      }
      return;
    }
    const bool repeat = isEsc && m_escDown;
    if (isEsc) {
      m_escDown = true;
    }
    if (m_burst.noteKeyDown(isEsc, repeat, EscTapBurst::Clock::now()) && m_onBurst) {
      m_onBurst();
    }
  }

private:
  void run()
  {
    m_threadId = GetCurrentThreadId();
    g_instance = this;
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, hookProc, GetModuleHandleW(nullptr), 0);
    if (hook == nullptr) {
      LOG_WARN("[esc-dropout] local Esc x5 monitor unavailable: %lu", GetLastError());
      g_instance = nullptr;
      return;
    }
    LOG_INFO("[esc-dropout] local Esc x5 monitor armed (low-level hook, observe-only)");
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    }
    UnhookWindowsHookEx(hook);
    g_instance = nullptr;
  }

  std::function<void()> m_onBurst;
  std::thread m_thread;
  std::atomic<DWORD> m_threadId{0};
  EscTapBurst m_burst;
  bool m_escDown = false;
  std::mutex m_observeMutex;
  unsigned long m_lastTime = 0;
  unsigned m_lastVk = 0;
  bool m_lastDown = false;
};

} // namespace

void escDropoutObserveWindowsKey(unsigned vk, bool down, bool injected, unsigned long eventTime)
{
  // all LL hooks run on threads with their own message loops, but Windows
  // calls LL hooks serially for one input event; g_instance is set/cleared
  // by the monitor thread around its hook lifetime
  if (auto *self = g_instance; self != nullptr) {
    self->observe(vk, down, injected, eventTime);
  }
}

std::unique_ptr<IEscDropoutMonitor> createEscDropoutMonitor()
{
  return std::make_unique<MSWindowsEscDropoutMonitor>();
}

} // namespace deskflow::coordination
