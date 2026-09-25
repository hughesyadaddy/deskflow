/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/KeyTypes.h"

#include <functional>
#include <memory>

namespace deskflow::coordination {

//! Platform "genuine local hardware input" signal.
/*!
Fires the callback once per hardware-originated MOUSE event (motion,
buttons, wheel), and never for synthesized/injected events -- this is what
lets a client machine claim primary the instant its own mouse moves while
ignoring the motion the KVM server is injecting into it. Keys never fire
it: a client seat's keyboard is relayed to the cursor host on purpose.

Separately it reports genuine (non-repeat) key downs to an optional sink,
on the monitor's own thread: that is what feeds the Esc-burst keyboard
rescue in EVERY role, off the core event loop -- a wedged loop is exactly
the case the rescue exists for, so its counter must never live on that
loop.

Implementations (see behavior-spec.md §3.1):
 - macOS: listen-only CGEventTap; genuine iff the event source unix PID
   is 0. Requires the same Input Monitoring grant deskflow already holds.
 - Windows: Raw Input sink; genuine iff RAWINPUTHEADER.hDevice != NULL.
 - others: unsupported stub (never fires; auto mode then only follows
   claims and manual promotes).
*/
class ILocalInputMonitor
{
public:
  using Callback = std::function<void()>;
  //! A genuine key down (repeats excluded): the neutral KeyID (kKeyEscape
  //! for Esc; other keys need not be mapped precisely) and the modifier
  //! mask held at the time.
  using KeyDownSink = std::function<void(KeyID id, KeyModifierMask mask)>;

  virtual ~ILocalInputMonitor() = default;

  //! Start delivering callbacks (from the monitor's own thread).
  virtual bool start(Callback onGenuineInput) = 0;
  virtual void stop() = 0;

  //! Install the key-down sink (set before start(); platforms without key
  //! monitoring keep the default no-op).
  virtual void setKeyDownSink(KeyDownSink sink)
  {
    (void)sink;
  }
};

//! Create the monitor for the current platform.
std::unique_ptr<ILocalInputMonitor> createLocalInputMonitor();

} // namespace deskflow::coordination
