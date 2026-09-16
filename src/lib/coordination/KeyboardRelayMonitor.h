/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "coordination/CoordinationProtocol.h"
#include "deskflow/KeyTypes.h"

#include <functional>
#include <memory>
#include <vector>

namespace deskflow::coordination {

//! Outcome of handing a key to the coordinator (KeyForwardSend).
enum class KeyForwardResult
{
  Local,     //!< not forwarded: the hook delivers the key to the local OS
  Forwarded, //!< sent to the peer: the hook swallows it and records the hold
  Swallowed, //!< consumed here (5x Esc rescue): swallow, but NOT held remotely
};

//! Fleet keyboard relay: forward local keys when the cursor is on another peer.
//! During client epoch, the query returns true when the key should reach the
//! local OS (pass-through) and false when it should be forwarded to the server.
class IKeyboardRelayMonitor
{
public:
  using RelayPassThroughQuery = std::function<bool()>;
  //! Deprecated alias; returns true when the key should reach the local OS.
  using CursorOnSelfQuery = RelayPassThroughQuery;
  //! Hands a key to the coordinator; see KeyForwardResult for what the
  //! hook does with the answer.
  using KeyForwardSend = std::function<KeyForwardResult(
      Message::KeyPhase phase, KeyID id, KeyModifierMask mask, KeyButton button, const std::string &lang
  )>;
  //! Receives the buttons still held on the peer when the relay stops, so
  //! the coordinator can post their Ups (survives backoff; see stop()).
  using ForwardedReleaseSink = std::function<void(const std::vector<KeyButton> &buttons)>;

  virtual ~IKeyboardRelayMonitor() = default;

  virtual bool start(RelayPassThroughQuery passThrough, KeyForwardSend send) = 0;
  //! Boundary: every hold whose Down was forwarded is reported to the
  //! release sink (best effort) and the ledger is cleared, so no key stays
  //! held on the peer across an epoch flip.
  virtual void stop() = 0;

  //! Set where stop() reports forwarded holds. Optional.
  virtual void setForwardedReleaseSink(ForwardedReleaseSink)
  {
    // default: holds are dropped silently
  }

  //! Boundary resync from another thread (lane failure, rescue): every
  //! forwarded hold is re-labelled Local at the next hook callback, so its
  //! Up reaches the local OS instead of being swallowed for a peer that
  //! can no longer be told. Thread-safe; the peer side is released by
  //! KeyClearAll.
  virtual void releaseForwardedLocally()
  {
    // default: no ledger
  }

  //! True while the monitor's capture machinery is actually live.
  /*!
  Distinguishes "thread exists" from "capture works": a tap/hook that
  failed to install (permissions, transient OS state) leaves a finished
  thread behind. The coordination worker reconciles against this so a
  client epoch can never silently run without its keyboard relay.
  */
  virtual bool running() const = 0;
};

std::unique_ptr<IKeyboardRelayMonitor> createKeyboardRelayMonitor();

} // namespace deskflow::coordination
