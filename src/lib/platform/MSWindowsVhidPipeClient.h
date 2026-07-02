/*

 * Deskflow -- mouse and keyboard sharing utility

 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers

 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception

 */



#pragma once



#include <cstdint>

#include <optional>



namespace deskflow::platform::mswindows {



//! User-mode client for deskflow-vhid-bridge.exe pipe mode.

class MSWindowsVhidPipeClient

{

public:

  bool connect();

  void disconnect();

  bool isConnected() const;



  bool sendMove(int32_t dx, int32_t dy);

  bool sendMouseButtons(uint8_t buttons);

  bool sendKeyboardState(uint8_t modifiers, const unsigned char keys[6]);

  bool sendKeyUsage(uint8_t usage, bool down, uint8_t modifiers);



  static std::optional<uint8_t> virtualKeyToHidUsage(unsigned virtualKey);

  static std::optional<uint8_t> virtualKeyToModifierMask(unsigned virtualKey);



private:

  bool writeCommand(const char *command);



  void *m_pipe = nullptr; // HANDLE stored opaquely to avoid Windows.h in header

};



//! Routes desk injection to the VHID pipe when secure desktop + bridge are active.

class MSWindowsVhidRouting

{

public:

  static MSWindowsVhidRouting &instance();



  void refreshState();

  bool isActive() const;



  bool routeRelativeMove(int32_t dx, int32_t dy);

  bool routeAbsoluteMove(int32_t x, int32_t y);

  bool routeMouseButton(unsigned mouseEventFlags, unsigned mouseData);

  bool routeKeyboard(unsigned virtualKey, unsigned scanCode, unsigned flags);



private:

  MSWindowsVhidRouting();



  MSWindowsVhidPipeClient m_client;

  bool m_enabledInSettings = false;

  bool m_secureDesktop = false;

  uint8_t m_modifiers = 0;

  int32_t m_lastX = 0;

  int32_t m_lastY = 0;

  bool m_hasLastPosition = false;

  void *m_secureEvent = nullptr; // HANDLE

};



} // namespace deskflow::platform::mswindows

