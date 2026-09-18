/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "coordination/Coordinator.h"

#include <chrono>
#include <string>

namespace deskflow::core::health {

//! Period of the `health:` line.
inline constexpr std::chrono::seconds kInterval{300};

//! One sample of everything the `health:` line reports.
struct Snapshot
{
  std::string seat;
  std::string role;   //!< server | client | init
  int epoch = 0;      //!< app epochs started since launch
  long upSeconds = 0; //!< process uptime
  coordination::Coordinator::HealthStats stats;
  std::string tap;          //!< ok | missing (client relay tap) | idle (not a client)
  std::string ax;           //!< trusted | no | n/a
  bool guiIpc = false;      //!< a GUI is attached to the core IPC socket
  std::string mouserBridge; //!< attached | linked | none
};

//! Single grep-stable line: `health: seat=.. role=.. ... mouser_bridge=..`.
//! Field order is part of the contract (fleet-soak/fleet-health parse it).
std::string formatLine(const Snapshot &snapshot);

//! Tap field from the running role and the relay monitor state.
std::string tapState(coordination::Role role, bool relayRunning);

//! Mouser bridge field from the MouserLink mode/connection.
std::string mouserBridgeState(bool connected, bool legacy);

} // namespace deskflow::core::health
