/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsVhidPipeClient.h"

#include "arch/win32/XArchWindows.h"
#include "base/Log.h"
#include "common/Constants.h"
#include "common/Settings.h"

#include "../../driver/deskflow-vhid/public/deskflow_vhid_ioctl.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace deskflow::platform::mswindows {

namespace {

constexpr wchar_t kPipeName[] = DESKFLOW_VHID_BRIDGE_PIPE_NAME;

HANDLE pipeHandle(void *opaque)
{
  return static_cast<HANDLE>(opaque);
}

int32_t clampAxis(int32_t value)
{
  return std::clamp(value, static_cast<int32_t>(-127), static_cast<int32_t>(127));
}

} // namespace

bool MSWindowsVhidPipeClient::connect()
{
  if (isConnected()) {
    return true;
  }

  for (int attempt = 0; attempt < 10; ++attempt) {
    HANDLE handle = CreateFileW(
        kPipeName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr
    );
    if (handle != INVALID_HANDLE_VALUE) {
      m_pipe = handle;
      LOG_INFO("connected to vhid bridge pipe");
      return true;
    }

    const DWORD error = GetLastError();
    if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) {
      LOG_WARN("vhid pipe connect failed: %s", windowsErrorToString(error).c_str());
      return false;
    }
    Sleep(100);
  }

  LOG_WARN("vhid pipe not available after retries");
  return false;
}

void MSWindowsVhidPipeClient::disconnect()
{
  if (m_pipe != nullptr) {
    CloseHandle(pipeHandle(m_pipe));
    m_pipe = nullptr;
    LOG_DEBUG("disconnected from vhid bridge pipe");
  }
}

bool MSWindowsVhidPipeClient::isConnected() const
{
  return m_pipe != nullptr && m_pipe != INVALID_HANDLE_VALUE;
}

bool MSWindowsVhidPipeClient::writeCommand(const char *command)
{
  if (!isConnected()) {
    return false;
  }

  char line[128];
  const int length = std::snprintf(line, sizeof(line), "%s\n", command);
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(line)) {
    return false;
  }

  DWORD written = 0;
  if (!WriteFile(pipeHandle(m_pipe), line, static_cast<DWORD>(length), &written, nullptr) ||
      written != static_cast<DWORD>(length)) {
    LOG_WARN("vhid pipe write failed");
    disconnect();
    return false;
  }
  return true;
}

bool MSWindowsVhidPipeClient::sendMove(int32_t dx, int32_t dy)
{
  while (dx != 0 || dy != 0) {
    const int32_t stepX = clampAxis(dx);
    const int32_t stepY = clampAxis(dy);
    char command[64];
    std::snprintf(command, sizeof(command), "move %d %d", stepX, stepY);
    if (!writeCommand(command)) {
      return false;
    }
    dx -= stepX;
    dy -= stepY;
  }
  return true;
}

bool MSWindowsVhidPipeClient::sendMouseButtons(uint8_t buttons)
{
  char command[64];
  std::snprintf(command, sizeof(command), "buttons %u", buttons);
  return writeCommand(command);
}

bool MSWindowsVhidPipeClient::sendKeyboardState(uint8_t modifiers, const unsigned char keys[6])
{
  char command[128];
  if (keys == nullptr || keys[0] == 0) {
    std::snprintf(command, sizeof(command), "modifiers %u", modifiers);
  } else {
    std::snprintf(command, sizeof(command), "key %u %u down", modifiers, keys[0]);
  }
  return writeCommand(command);
}

bool MSWindowsVhidPipeClient::sendKeyUsage(uint8_t usage, bool down, uint8_t modifiers)
{
  char command[64];
  std::snprintf(command, sizeof(command), "key %u %u %s", modifiers, usage, down ? "down" : "up");
  return writeCommand(command);
}

std::optional<uint8_t> MSWindowsVhidPipeClient::virtualKeyToModifierMask(unsigned virtualKey)
{
  switch (virtualKey) {
  case VK_LCONTROL:
  case VK_CONTROL:
    return 0x01;
  case VK_LSHIFT:
  case VK_SHIFT:
    return 0x02;
  case VK_LMENU:
  case VK_MENU:
    return 0x04;
  case VK_LWIN:
    return 0x08;
  case VK_RCONTROL:
    return 0x10;
  case VK_RSHIFT:
    return 0x20;
  case VK_RMENU:
    return 0x40;
  case VK_RWIN:
    return 0x80;
  default:
    return std::nullopt;
  }
}

std::optional<uint8_t> MSWindowsVhidPipeClient::virtualKeyToHidUsage(unsigned virtualKey)
{
  if (virtualKey >= 'A' && virtualKey <= 'Z') {
    return static_cast<uint8_t>(4 + (virtualKey - 'A'));
  }
  if (virtualKey >= '0' && virtualKey <= '9') {
    return static_cast<uint8_t>((virtualKey == '0') ? 39 : (29 + (virtualKey - '1')));
  }
  if (virtualKey >= 'a' && virtualKey <= 'z') {
    return static_cast<uint8_t>(4 + (virtualKey - 'a'));
  }

  switch (virtualKey) {
  case VK_RETURN:
    return 40;
  case VK_TAB:
    return 43;
  case VK_BACK:
    return 42;
  case VK_SPACE:
    return 44;
  case VK_ESCAPE:
    return 41;
  case VK_LEFT:
    return 80;
  case VK_RIGHT:
    return 79;
  case VK_UP:
    return 82;
  case VK_DOWN:
    return 81;
  default:
    return std::nullopt;
  }
}

MSWindowsVhidRouting &MSWindowsVhidRouting::instance()
{
  static MSWindowsVhidRouting routing;
  return routing;
}

MSWindowsVhidRouting::MSWindowsVhidRouting()
{
  m_secureEvent = OpenEventW(SYNCHRONIZE, FALSE, kSecureDesktopEventName);
  if (m_secureEvent == nullptr) {
    LOG_DEBUG("secure desktop event not yet available: %s", windowsErrorToString(GetLastError()).c_str());
  }
}

void MSWindowsVhidRouting::refreshState()
{
  m_enabledInSettings = Settings::value(Settings::Daemon::VhidBridgeEnabled).toBool();

  if (m_secureEvent == nullptr) {
    m_secureEvent = OpenEventW(SYNCHRONIZE, FALSE, kSecureDesktopEventName);
  }

  const bool wasSecure = m_secureDesktop;
  m_secureDesktop = false;
  if (m_secureEvent != nullptr) {
    m_secureDesktop = WaitForSingleObject(static_cast<HANDLE>(m_secureEvent), 0) == WAIT_OBJECT_0;
  }

  const bool shouldRoute = m_enabledInSettings && m_secureDesktop;
  if (shouldRoute && !m_client.isConnected()) {
    m_client.connect();
  } else if (!shouldRoute) {
    if (m_client.isConnected()) {
      m_client.disconnect();
    }
    m_modifiers = 0;
    m_hasLastPosition = false;
  }

  if (wasSecure != m_secureDesktop) {
    if (m_secureDesktop) {
      // Keep m_lastX/m_lastY from the normal desktop so the first UAC move is a delta, not a warp from (0,0).
      LOG_INFO(
          "vhid routing %s (bridge %s)", m_secureDesktop ? "active" : "inactive",
          m_client.isConnected() ? "connected" : "disconnected"
      );
    } else {
      m_hasLastPosition = false;
      LOG_INFO("vhid routing inactive (bridge disconnected)");
    }
  }
}

bool MSWindowsVhidRouting::isActive() const
{
  return m_enabledInSettings && m_secureDesktop && m_client.isConnected();
}

bool MSWindowsVhidRouting::routeRelativeMove(int32_t dx, int32_t dy)
{
  if (!isActive()) {
    return false;
  }
  return m_client.sendMove(dx, dy);
}

bool MSWindowsVhidRouting::routeAbsoluteMove(int32_t x, int32_t y)
{
  if (!isActive()) {
    return false;
  }

  if (!m_hasLastPosition) {
    m_lastX = 0;
    m_lastY = 0;
    m_hasLastPosition = true;
  }

  const int32_t dx = x - m_lastX;
  const int32_t dy = y - m_lastY;
  m_lastX = x;
  m_lastY = y;

  if (dx == 0 && dy == 0) {
    return true;
  }
  return m_client.sendMove(dx, dy);
}

bool MSWindowsVhidRouting::routeMouseButton(unsigned mouseEventFlags, unsigned /*mouseData*/)
{
  if (!isActive()) {
    return false;
  }

  if (mouseEventFlags == MOUSEEVENTF_LEFTDOWN) {
    return m_client.sendMouseButtons(1);
  }
  if (mouseEventFlags == MOUSEEVENTF_LEFTUP) {
    return m_client.sendMouseButtons(0);
  }
  if (mouseEventFlags == MOUSEEVENTF_RIGHTDOWN) {
    return m_client.sendMouseButtons(2);
  }
  if (mouseEventFlags == MOUSEEVENTF_RIGHTUP) {
    return m_client.sendMouseButtons(0);
  }
  if (mouseEventFlags == MOUSEEVENTF_MIDDLEDOWN) {
    return m_client.sendMouseButtons(4);
  }
  if (mouseEventFlags == MOUSEEVENTF_MIDDLEUP) {
    return m_client.sendMouseButtons(0);
  }
  return false;
}

bool MSWindowsVhidRouting::routeKeyboard(unsigned virtualKey, unsigned /*scanCode*/, unsigned flags)
{
  if (!isActive()) {
    return false;
  }

  if ((flags & KEYEVENTF_UNICODE) != 0) {
    return false;
  }

  const bool down = (flags & KEYEVENTF_KEYUP) == 0;

  if (const auto modifierMask = MSWindowsVhidPipeClient::virtualKeyToModifierMask(virtualKey); modifierMask.has_value()) {
    if (down) {
      m_modifiers = static_cast<uint8_t>(m_modifiers | modifierMask.value());
    } else {
      m_modifiers = static_cast<uint8_t>(m_modifiers & ~modifierMask.value());
    }
    return m_client.sendKeyboardState(m_modifiers, nullptr);
  }

  const auto usage = MSWindowsVhidPipeClient::virtualKeyToHidUsage(virtualKey);
  if (!usage.has_value()) {
    LOG_DEBUG("vhid routing: no HID usage for VK %u", virtualKey);
    return false;
  }

  return m_client.sendKeyUsage(usage.value(), down, m_modifiers);
}

} // namespace deskflow::platform::mswindows
