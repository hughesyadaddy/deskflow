/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace deskflow::coordination {

//! A peer entry in the fleet snapshot.
struct FleetPeer
{
  std::string name;
  std::string ip;
  std::string lan;

  bool operator==(const FleetPeer &) const = default;
};

//! One directed screen adjacency edge.
struct FleetLink
{
  std::string fromScreen;
  std::string toScreen;
  std::string direction; // left, right, top, bottom

  bool operator==(const FleetLink &) const = default;
};

//! A screen known to the fleet.
struct FleetScreen
{
  std::string name;

  bool operator==(const FleetScreen &) const = default;
};

//! Fleet snapshot: the merged local view and the server-originated `fleet`
//! fragment share one shape; the alias below keeps call sites readable.
struct FleetState
{
  std::string server;
  std::string cursorHost;
  std::string cursorScreen;
  std::vector<FleetPeer> peers;
  std::vector<FleetLink> links;
  std::vector<FleetScreen> screens;
  int64_t seq = 0;
};

using FleetFragment = FleetState;

bool operator==(const FleetState &a, const FleetState &b);
bool operator!=(const FleetState &a, const FleetState &b);

} // namespace deskflow::coordination
