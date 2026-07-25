/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/KeyboardRescue.h"

#include "base/Log.h"

namespace deskflow::coordination {

namespace {

LocalCoreRestartFn g_localCoreRestartHandler = nullptr;
FleetRescueFn g_fleetRescueHandler = nullptr;

} // namespace

void setLocalCoreRestartHandler(LocalCoreRestartFn fn)
{
  g_localCoreRestartHandler = fn;
}

void requestLocalCoreRestart()
{
  if (g_localCoreRestartHandler != nullptr) {
    g_localCoreRestartHandler();
    return;
  }
  LOG_WARN("keyboard rescue: 5x Esc — no local core restart handler registered");
}

void setFleetRescueHandler(FleetRescueFn fn)
{
  g_fleetRescueHandler = fn;
}

void requestFleetRescue()
{
  if (g_fleetRescueHandler != nullptr) {
    g_fleetRescueHandler();
    return;
  }
  // No mesh (server/client mode, or coordinator not started): the local
  // restart is still the best available rescue.
  requestLocalCoreRestart();
}

} // namespace deskflow::coordination
