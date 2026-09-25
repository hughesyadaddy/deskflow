/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <string>
#include <vector>

namespace deskflow::coordination {

//! launchd labels of the fleet user agents (scripts/deskflow-ctl).
inline constexpr const char *kMacCoreLabel = "io.github.hughesyadaddy.deskflow-core";
inline constexpr const char *kMacGuiLabel = "io.github.hughesyadaddy.deskflow";
inline constexpr const char *kMacConvergeLabel = "io.github.hughesyadaddy.deskflow-converge";

//! Seconds a stray GUI/core gets to leave on SIGTERM before SIGKILL.
inline constexpr int kStrayTermGraceMs = 2000;

//! The commands the macOS stop-all sequence issues (seam for tests).
/*!
The real implementation (OSXRescueStopAll.mm) writes the sentinel file,
spawns launchctl / the launchd-safe deskflow-ctl copy, enumerates the
bundle's processes with libproc and signals them. The recorder used by the
unit tests only logs the calls, so the ORDER of the sequence is what is
verified: quit-intent → converge → GUI → strays → core (self) last.
*/
class IStopAllCommands
{
public:
  virtual ~IStopAllCommands() = default;

  //! Write ~/Library/Application Support/Deskflow/quit-intent so converge
  //! and KeepAlive leave everything down until `deskflow-ctl start`.
  virtual bool writeQuitIntent() = 0;

  //! True when a launchd-safe `deskflow-ctl` exists (~/Library/Deskflow/bin).
  virtual bool canonicalStopAvailable() = 0;
  //! Spawn `deskflow-ctl stop` (own session) and wait a bounded time.
  //! True when it exited 0 -- the caller is then most likely already gone.
  virtual bool runCanonicalStop() = 0;

  //! `launchctl bootout gui/<uid>/<label>`; false when it failed or the
  //! label was not loaded. \p wait = block until launchctl returns (never
  //! for the core: that is us, and launchd waits for us to exit).
  virtual bool bootout(const std::string &label, bool wait) = 0;

  //! PIDs of this user's bundle GUI/core processes, excluding self.
  virtual std::vector<int> strayPids() = 0;
  virtual void terminate(int pid, bool force) = 0;
  virtual void sleepMs(int ms) = 0;

  //! Graceful exit of this core (requestLocalCoreQuit()).
  virtual void quitSelf() = 0;
};

//! The canonical macOS stop-all order, on top of \p commands. Blocking.
void runMacStopAllSequence(IStopAllCommands &commands);

#if defined(__APPLE__)
//! Real executor for this seat (registered by deskflow-core on macOS).
void runMacStopAll(const std::string &seat);
#endif

} // namespace deskflow::coordination
