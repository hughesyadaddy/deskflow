/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <string>
#include <vector>

namespace deskflow::coordination {

//! A coordination-mesh member.
/*!
A peer is reachable at a primary address (\c ip, typically stable / VPN)
and optionally a LAN address (\c lan) preferred when reachable. Peers that
sleep can carry wake hints: \c mac (Wake-on-LAN target) and/or
\c wakeCommand (shell command, e.g. a hypervisor resume for VMs).
*/
struct Peer
{
  std::string name;
  std::string ip;
  std::string lan;
  std::string mac;         // optional: WoL target, e.g. "bc:24:11:aa:bb:cc"
  std::string wakeCommand; // optional: e.g. "ssh proxmox qm wakeup 100"

  bool hasAddress(const std::string &address) const
  {
    return !address.empty() && (address == ip || address == lan);
  }
};

using PeerList = std::vector<Peer>;

//! Parse a peers setting string: comma-separated peer entries.
/*!
Each entry is either `name=address[|lan[|mac[|wakeCommand]]]` or a bare
name/address:

- `desktop=192.0.2.10|desktop.local` — explicit name, stable address, and
  preferred LAN address.
- `laptop=laptop.example.net` — explicit name and address (LAN defaults to
  the same address).
- `gamepc` — bare machine name: address is the name itself (DNS / search
  domain) and `gamepc.local` is the LAN candidate.
- `gamepc.local` — bare address: the peer name is the first dot-label.
- `vm=10.0.0.5|vm.local|bc:24:11:aa:bb:cc|ssh proxmox qm wakeup 100` —
  peer with wake hints. Wake commands cannot contain commas (the comma is
  the entry separator).

Whitespace around entries is ignored; malformed entries are skipped.
Example: `"desktop=192.0.2.10|desktop.local, laptop, gamepc.local"`.
*/
PeerList parsePeerList(const std::string &setting);

} // namespace deskflow::coordination
