/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/LocalInputMonitor.h"

#include "base/Log.h"

#include <windows.h>

#include <atomic>
#include <set>
#include <thread>

namespace deskflow::coordination {

namespace {

//! Raw Input sink on a message-only window thread.
/*!
Genuine hardware events arrive with a non-null RAWINPUTHEADER.hDevice;
SendInput-synthesized events (deskflow client injection) carry a null
device handle and are ignored. Replaces the external KvmSwitch.cs
Raw Input window.
*/
class MSWindowsLocalInputMonitor : public ILocalInputMonitor
{
public:
  ~MSWindowsLocalInputMonitor() override
  {
    stop();
  }

  bool start(Callback onGenuineInput) override
  {
    if (m_thread.joinable()) {
      return true;
    }
    m_callback = std::move(onGenuineInput);
    m_running = true;
    m_thread = std::thread([this] { runLoop(); });
    return true;
  }

  void setKeyDownSink(KeyDownSink sink) override
  {
    m_keyDownSink = std::move(sink);
  }

  void stop() override
  {
    m_running = false;
    if (m_threadId != 0) {
      PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
    }
    if (m_thread.joinable()) {
      m_thread.join();
    }
    m_threadId = 0;
  }

private:
  static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
  {
    if (msg == WM_INPUT) {
      auto *self = reinterpret_cast<MSWindowsLocalInputMonitor *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
      if (self != nullptr) {
        self->handleRawInput(lParam);
      }
      return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
  }

  void handleRawInput(LPARAM lParam)
  {
    RAWINPUTHEADER header;
    UINT size = sizeof(header);
    const auto rc =
        GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_HEADER, &header, &size, sizeof(RAWINPUTHEADER));
    if (rc == static_cast<UINT>(-1)) {
      return;
    }
    // Null device == synthesized (SendInput); only real hardware counts.
    if (header.hDevice == nullptr) {
      return;
    }
    if (header.dwType == RIM_TYPEKEYBOARD) {
      // Keys feed ONLY the Esc rescue counter. They never count as genuine
      // input for the election: a client seat's keyboard is relayed to the
      // cursor host on purpose, and typing there must not promote that seat.
      if (m_keyDownSink) {
        handleRawKeyboard(lParam);
      }
      return;
    }
    if (m_callback) {
      logDeviceOnce(header.hDevice);
      m_callback();
    }
  }

  //! A genuine key transition: feed non-repeat downs to the sink (the
  //! Esc rescue counter). Raw Input carries no repeat flag; a down for a
  //! key already recorded down is a repeat.
  void handleRawKeyboard(LPARAM lParam)
  {
    RAWINPUT raw{};
    UINT size = sizeof(raw);
    const auto rc = GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER));
    if (rc == static_cast<UINT>(-1) || raw.header.dwType != RIM_TYPEKEYBOARD) {
      return;
    }
    const auto &keyboard = raw.data.keyboard;
    const bool keyUp = (keyboard.Flags & RI_KEY_BREAK) != 0;
    const bool isEscape = keyboard.VKey == VK_ESCAPE;
    if (keyUp) {
      if (isEscape) {
        m_escapeDown = false;
      }
      return;
    }
    if (isEscape && m_escapeDown) {
      return; // autorepeat
    }
    if (isEscape) {
      m_escapeDown = true;
    }
    m_keyDownSink(isEscape ? kKeyEscape : kKeyNone, heldModifiers());
  }

  //! Shift/Ctrl/Alt/Win currently held (OS view; delivered after the key
  //! state update, so a chord modifier is visible here), plus Caps Lock.
  static KeyModifierMask heldModifiers()
  {
    KeyModifierMask mask = 0;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
      mask |= KeyModifierShift;
    }
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
      mask |= KeyModifierControl;
    }
    if (GetAsyncKeyState(VK_MENU) & 0x8000) {
      mask |= KeyModifierAlt;
    }
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) {
      mask |= KeyModifierSuper;
    }
    if (GetKeyState(VK_CAPITAL) & 1) {
      mask |= KeyModifierCapsLock;
    }
    return mask;
  }

  //! Name each genuine input device the first time it is seen (bounded).
  //! When phantom input claims serverhood, this line convicts the exact
  //! device (drifting mouse, controller, ghost receiver).
  void logDeviceOnce(HANDLE device)
  {
    if (m_seenDevices.count(device) != 0 || m_seenDevices.size() >= 8) {
      return;
    }
    m_seenDevices.insert(device);
    wchar_t name[256]{};
    UINT size = 255;
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, name, &size) > 0) {
      LOG_INFO("coordination: genuine local input from device: %ls", name);
    } else {
      LOG_INFO("coordination: genuine local input from unnamed device handle=%p", device);
    }
  }

  void runLoop()
  {
    m_threadId = GetCurrentThreadId();

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"DeskflowCoordinationInput";
    RegisterClassExW(&windowClass);

    HWND hwnd = CreateWindowExW(
        0, windowClass.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, windowClass.hInstance, nullptr
    );
    if (hwnd == nullptr) {
      LOG_WARN("coordination: could not create raw input window");
      return;
    }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    RAWINPUTDEVICE devices[2]{};
    devices[0].usUsagePage = 0x01; // generic desktop
    devices[0].usUsage = 0x02;     // mouse
    devices[0].dwFlags = RIDEV_INPUTSINK;
    devices[0].hwndTarget = hwnd;
    devices[1].usUsagePage = 0x01; // generic desktop
    devices[1].usUsage = 0x06;     // keyboard (Esc rescue counter, every role)
    devices[1].dwFlags = RIDEV_INPUTSINK;
    devices[1].hwndTarget = hwnd;
    if (!RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE))) {
      LOG_WARN("coordination: raw input registration failed");
      DestroyWindow(hwnd);
      return;
    }
    LOG_DEBUG("coordination: local input monitor started");

    MSG message;
    while (m_running && GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }

    DestroyWindow(hwnd);
    LOG_DEBUG("coordination: local input monitor stopped");
  }

  Callback m_callback;
  KeyDownSink m_keyDownSink;
  bool m_escapeDown = false;      //!< raw-input thread only (repeat detection)
  std::set<HANDLE> m_seenDevices; //!< raw-input thread only
  std::thread m_thread;
  std::atomic<bool> m_running{false};
  DWORD m_threadId = 0;
};

} // namespace

std::unique_ptr<ILocalInputMonitor> createLocalInputMonitor()
{
  return std::make_unique<MSWindowsLocalInputMonitor>();
}

} // namespace deskflow::coordination
