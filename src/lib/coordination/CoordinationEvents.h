/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "base/Event.h"
#include "coordination/RelayKeyEvent.h"

#include <string>
#include <utility>

//! Event data for CoordinationKeyForward (cursor host injects relayed keys).
class CoordinationKeyForwardInfo : public EventData
{
public:
  deskflow::coordination::RelayKeyEvent event;

  explicit CoordinationKeyForwardInfo(deskflow::coordination::RelayKeyEvent event) : event(std::move(event))
  {
  }

  ~CoordinationKeyForwardInfo() override = default;
};

//! Event data for CoordinationKeyClearAll: which peer asked for the resync.
/*!
Only the keys THAT sender forwarded are released; another peer's relayed
holds are untouched (a fleet of three senders must not lose keys because
one lane failed).
*/
class CoordinationKeyClearAllInfo : public EventData
{
public:
  std::string sender;

  explicit CoordinationKeyClearAllInfo(std::string sender) : sender(std::move(sender))
  {
  }

  ~CoordinationKeyClearAllInfo() override = default;
};
