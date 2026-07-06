/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "coordination/FleetState.h"
#include "coordination/Peer.h"

#include <QStringList>

#include <string>

namespace deskflow::coordination {

//! Client pre-connect candidate list from the merged fleet snapshot.
/*!
Candidate order is connect order and each dead candidate costs a connect
timeout: the elected server's LAN address first (lowest-latency route),
then its stable address, then \p serverAddress, then other peers as
takeover fallbacks. Self is never listed; duplicates are removed.
*/
QStringList
preConnectHostsFromFleet(const FleetState &fleet, const std::string &serverAddress, const std::string &selfName);

//! Fallback candidate list from configured peers (no fleet snapshot yet).
/*!
\p serverAddress first, then every configured peer's LAN and stable
addresses (self excluded, deduplicated).
*/
QStringList
defaultPreConnectHosts(const std::string &serverAddress, const std::string &selfName, const PeerList &peers);

} // namespace deskflow::coordination
