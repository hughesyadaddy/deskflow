/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <ostream>
#include <string>

/**
 * @brief `deskflow-core --check-permissions [--json]`
 *
 * Fleet-health probe for macOS TCC state. Deliberately free of Qt and of any
 * app/platform library so it can run before QApplication exists (e.g. from a
 * headless SSH session) and be unit-tested without TCC.
 *
 * Exit codes: 0 = accessibility and input-monitoring granted, 1 = otherwise,
 * 2 = unsupported platform.
 */
namespace deskflow::core::permissions {

struct Status
{
  bool supported = false;      //!< false on non-macOS platforms
  bool accessibility = false;  //!< AXIsProcessTrusted()
  bool inputMonitoring = false; //!< IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) == granted
  bool postEvent = false;      //!< IOHIDCheckAccess(kIOHIDRequestTypePostEvent) == granted
};

using Probe = Status (*)();

inline constexpr int kExitGranted = 0;
inline constexpr int kExitDenied = 1;
inline constexpr int kExitUnsupported = 2;

/// Real platform probe. Never prompts the user.
Status probe();

/// True if argv contains `--check-permissions`.
bool requested(int argc, char **argv);

/// True if argv contains `--json`.
bool wantsJson(int argc, char **argv);

/// Text or JSON rendering of a status (always newline-terminated).
std::string render(const Status &status, bool json);

/// Exit-code mapping for a status.
int exitCodeFor(const Status &status);

/**
 * @brief Early-argv hook. Returns -1 if `--check-permissions` was not
 * requested (caller continues normal startup); otherwise writes the report
 * to @p out and returns the process exit code.
 */
int run(int argc, char **argv, Probe probeFn, std::ostream &out);

} // namespace deskflow::core::permissions
