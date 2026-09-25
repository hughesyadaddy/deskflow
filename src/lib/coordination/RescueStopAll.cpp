/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/RescueStopAll.h"

#include "base/Log.h"

namespace deskflow::coordination {

namespace {

void terminateStrays(IStopAllCommands &commands)
{
  auto strays = commands.strayPids();
  if (strays.empty()) {
    return;
  }
  for (const int pid : strays) {
    LOG_INFO("[rescue] stray Deskflow process pid %d: SIGTERM", pid);
    commands.terminate(pid, false);
  }
  commands.sleepMs(kStrayTermGraceMs);
  strays = commands.strayPids();
  for (const int pid : strays) {
    LOG_WARN("[rescue] stray Deskflow process pid %d ignored SIGTERM: SIGKILL", pid);
    commands.terminate(pid, true);
  }
}

//! Leave through the real quit path, and hard-exit if that never lands
//! (SIGTERM alone only ends an epoch; a wedged loop swallows even the
//! runner's quit).
void quitForReal(IStopAllCommands &commands)
{
  commands.quitSelf();
  commands.armExitFallback(kQuitExitFallbackMs);
}

} // namespace

void runMacStopAllSequence(IStopAllCommands &commands)
{
  // 1. The sentinel goes first: a converge tick racing this stop must not
  //    bootstrap the agents straight back (same rule as deskflow-ctl stop).
  if (!commands.writeQuitIntent()) {
    LOG_WARN("[rescue] could not write the quit-intent sentinel; converge may relaunch Deskflow");
  }
  LOG_INFO("[rescue] the login-window bridge (org.deskflow.vhid-bridge) needs root and is left running");

  // 2. Prefer the canonical script: it is the one owner of the launchd
  //    sequence and escalates by PID exactly like an operator would. It is
  //    spawned detached and NOT waited for: it boots this core out as part
  //    of its job, so we leave right away through the real quit path
  //    instead of sitting in its TERM→KILL escalation.
  if (commands.canonicalStopAvailable()) {
    LOG_INFO("[rescue] spawning the launchd-safe deskflow-ctl stop");
    if (commands.spawnCanonicalStop()) {
      quitForReal(commands);
      return;
    }
    LOG_WARN("[rescue] deskflow-ctl stop could not be spawned; falling back to the in-process sequence");
  } else {
    LOG_INFO("[rescue] no launchd-safe deskflow-ctl copy; using the in-process sequence");
  }

  // 3. In-process fallback, canonical order: converge, GUI, strays, core.
  commands.bootout(kMacConvergeLabel, true);
  commands.bootout(kMacGuiLabel, true);
  terminateStrays(commands);
  // Self last. launchd SIGTERMs us for this one, so never wait on it; the
  // explicit quit covers a core that launchd does not own (dev run).
  commands.bootout(kMacCoreLabel, false);
  quitForReal(commands);
}

} // namespace deskflow::coordination
