/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QString>

namespace deskflow::gui {

//! Windows background-service lifecycle tied to the GUI process.
class WindowsDaemonService
{
public:
  //! Start the Deskflow daemon service when background-service mode is enabled.
  static bool ensureRunning();

  //! Install (if needed) and start the background service. Used from Settings and CLI.
  static bool installAndStart();

  //! CLI entry for @c deskflow.exe --install-daemon-service (elevated helper process).
  static int installFromCli();

  //! Human-readable service state for the Settings dialog.
  static QString statusText();

  //! True when the Deskflow Windows service is registered and running.
  static bool isRunning();

  //! No-op in service mode — the daemon must stay running for login/UAC when the GUI exits.
  static void stop();

  //! Ensure the Windows service starts automatically at boot (login-screen / UAC watchdog).
  static void syncServiceStartTypeFromSettings();
};

} // namespace deskflow::gui
