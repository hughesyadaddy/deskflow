/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "HealthReport.h"

#include <cstdio>

namespace deskflow::core::health {

std::string formatLine(const Snapshot &snapshot)
{
  char buffer[512];
  std::snprintf(
      buffer, sizeof(buffer),
      "health: seat=%s role=%s epoch=%d up=%ld peers=%d/%d links=%d mesh_rx=%llu mesh_dup=%llu tap=%s ax=%s "
      "gui_ipc=%s flips_1h=%d rescue_1h=%d mouser_bridge=%s",
      snapshot.seat.c_str(), snapshot.role.c_str(), snapshot.epoch, snapshot.upSeconds, snapshot.stats.peersReachable,
      snapshot.stats.peersTotal, snapshot.stats.links, static_cast<unsigned long long>(snapshot.stats.meshRx),
      static_cast<unsigned long long>(snapshot.stats.meshDup), snapshot.tap.c_str(), snapshot.ax.c_str(),
      snapshot.guiIpc ? "connected" : "none", snapshot.stats.flipsLastHour, snapshot.stats.rescuesLastHour,
      snapshot.mouserBridge.c_str()
  );
  return buffer;
}

std::string tapState(coordination::Role role, bool relayRunning)
{
  if (role != coordination::Role::Client) {
    return "idle";
  }
  return relayRunning ? "ok" : "missing";
}

std::string mouserBridgeState(bool connected, bool legacy)
{
  if (!connected) {
    return "none";
  }
  return legacy ? "linked" : "attached";
}

} // namespace deskflow::core::health
