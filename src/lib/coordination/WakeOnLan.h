/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace deskflow::coordination {

using MacAddress = std::array<uint8_t, 6>;

//! Parse a MAC address string (`aa:bb:cc:dd:ee:ff` or `aa-bb-...`).
/*!
Returns std::nullopt for anything that is not exactly six two-hex-digit
octets separated by `:` or `-`.
*/
std::optional<MacAddress> parseMacAddress(const std::string &text);

//! Build the 102-byte Wake-on-LAN magic packet: 6x 0xFF then 16x MAC.
std::vector<uint8_t> buildMagicPacket(const MacAddress &mac);

//! Broadcast a Wake-on-LAN magic packet for \p mac (UDP port 9).
/*!
Returns false when the MAC does not parse or the send fails; the failure
is logged. Fire-and-forget: WoL offers no delivery confirmation.
*/
bool sendWakeOnLan(const std::string &mac);

} // namespace deskflow::coordination
