/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/Peer.h"

#include <array>

namespace deskflow::coordination {

namespace {

std::string trimmed(const std::string &value)
{
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

} // namespace

PeerList parsePeerList(const std::string &setting)
{
  PeerList peers;
  std::string::size_type start = 0;
  while (start <= setting.size()) {
    auto end = setting.find(',', start);
    if (end == std::string::npos) {
      end = setting.size();
    }
    const std::string entry = trimmed(setting.substr(start, end - start));
    start = end + 1;
    if (entry.empty()) {
      continue;
    }

    const auto equals = entry.find('=');
    if (equals == std::string::npos) {
      // Bare entry: a machine name or address. The peer name is the first
      // dot-label, the token itself is the stable address, and a plain
      // name additionally gets its mDNS `.local` form as the LAN
      // candidate (probed first; see behavior-spec §3.4).
      Peer peer;
      const auto dot = entry.find('.');
      peer.name = dot == std::string::npos ? entry : entry.substr(0, dot);
      peer.ip = entry;
      peer.lan = dot == std::string::npos ? entry + ".local" : entry;
      peers.push_back(std::move(peer));
      continue;
    }
    if (equals == 0 || equals == entry.size() - 1) {
      continue; // malformed: empty name or empty address
    }

    Peer peer;
    peer.name = trimmed(entry.substr(0, equals));
    // Pipe-separated segments after the name: ip|lan|mac|wakeCommand.
    std::string rest = entry.substr(equals + 1);
    std::array<std::string, 4> segments;
    std::size_t segment = 0;
    std::string::size_type segmentStart = 0;
    while (segment < segments.size()) {
      const auto pipe = rest.find('|', segmentStart);
      // The final segment (wakeCommand) keeps everything remaining so a
      // command containing '|' survives intact.
      if (pipe == std::string::npos || segment == segments.size() - 1) {
        segments[segment++] = trimmed(rest.substr(segmentStart));
        break;
      }
      segments[segment++] = trimmed(rest.substr(segmentStart, pipe - segmentStart));
      segmentStart = pipe + 1;
    }
    peer.ip = segments[0];
    peer.lan = segments[1];
    peer.mac = segments[2];
    peer.wakeCommand = segments[3];
    if (peer.name.empty() || peer.ip.empty()) {
      continue;
    }
    if (peer.lan.empty()) {
      peer.lan = peer.ip;
    }
    peers.push_back(std::move(peer));
  }
  return peers;
}

} // namespace deskflow::coordination
