/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "coordination/ElectionState.h"
#include "coordination/FleetState.h"
#include "coordination/RelayKeyEvent.h"

#include <cstdint>
#include <string>
#include <vector>

namespace deskflow::coordination {

//! One decoded coordination-mesh message.
/*!
Wire format is newline-delimited JSON, byte-compatible with the legacy
external coordinator (docs/coordination/behavior-spec.md §2), so existing
operator tooling (kvmctl) keeps working unchanged.
*/
struct Message
{
  //! Cursor and KeyFwd are legacy mesh v1 message types: still decoded so
  //! the transport can classify and drop them, never handled or sent.
  enum class Type
  {
    Invalid,
    Claim,
    Promote,
    Status,
    Cursor,
    KeyFwd,
    Key,
    Hello,
    Fleet,
    Rescue,
    //! Boundary resync (peer → cursor host): the sender lost track of which
    //! of its forwarded keys the receiver still holds (lane failure, relay
    //! stop); the receiver releases every key it holds on the sender's
    //! behalf (fakeAllKeysUp). Idempotent, safe to deliver late.
    KeyClearAll
  };

  using KeyPhase = RelayKeyPhase;

  Type type = Type::Invalid;
  std::string name;
  std::string ip;
  std::string lan;
  //! claim: election sequence. key: per-sender monotonic key sequence.
  int64_t seq = 0;
  std::string token;
  // cursor: host screen under the fleet cursor (server → peers)
  std::string host;
  // key: keyboard relay (peer → cursor host)
  KeyPhase keyPhase = KeyPhase::Down;
  uint16_t keyId = 0;
  uint16_t keyMask = 0;
  uint16_t keyButton = 0;
  std::string keyLang;
  //! key: sender wall clock at send, ms since the Unix epoch (0 = unknown,
  //! legacy sender). The receiver drops Down/Repeat older than
  //! kRelayKeyMaxAgeMs so a key delayed by a wedged lane is never typed late.
  int64_t keySentAtMs = 0;
  // hello version announcement
  int meshVersion = 0;
  // fleet fragment (decoded from `fleet` messages)
  FleetFragment fleet;
};

namespace protocol {

//! Decode one line; returns Type::Invalid on malformed input.
Message decode(const std::string &line);

std::string encodeClaim(
    const std::string &name, const std::string &ip, const std::string &lan, int64_t seq, const std::string &token
);
std::string encodePromote(const std::string &token);

//! Fleet-wide keyboard rescue: every peer restarts its local core.
std::string encodeRescue(const std::string &token);
std::string encodeStatus(const std::string &token);

//! Receiver-side freshness bound for relayed Down/Repeat (ms). Ups are
//! exempt: a late release is idempotent and always safer than a held key.
inline constexpr int64_t kRelayKeyMaxAgeMs = 250;

//! Keyboard relay (peer → cursor host). \p seq is the sender's monotonic key
//! sequence; \p sentAtMs the sender's wall clock (see Message::keySentAtMs).
std::string encodeKey(
    const std::string &from, RelayKeyPhase phase, uint16_t id, uint16_t mask, uint16_t button, const std::string &lang,
    const std::string &token, int64_t seq = 0, int64_t sentAtMs = 0
);

//! Boundary resync: release every key the receiver holds for \p from.
std::string encodeKeyClearAll(const std::string &from, const std::string &token);

//! Wall clock now, ms since the Unix epoch (the key sentAt stamp).
int64_t wallClockMs();

std::string encodeHello(int meshVersion, const std::string &name, const std::string &token);
std::string encodeFleet(const FleetFragment &fragment, const std::string &token);

//! Status reply (legacy shape: role/server_ip/seq/last_switch/name).
//! When \p fleet is set, a read-only \c fleet object is included for mesh v2 UIs.
std::string encodeStatusReply(
    Role role, const std::string &serverAddress, int64_t seq, double lastSwitchAt, const std::string &name,
    const FleetState *fleet = nullptr, int meshVersion = 0, const std::vector<std::string> &versionMismatchPeers = {}
);

//! A decoded status reply.
struct StatusReply
{
  bool valid = false;
  Role role = Role::Init;
  std::string serverAddress;
  std::string name;
  int meshVersion = 0;
  std::vector<std::string> versionMismatchPeers;
};

StatusReply decodeStatusReply(const std::string &line);

} // namespace protocol

} // namespace deskflow::coordination
