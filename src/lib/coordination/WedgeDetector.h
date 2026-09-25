/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <string>

namespace deskflow::coordination {

//! Loopback host to probe for a server listener bound to \p listenInterface.
/*!
The server binds `core/interface` when set, else the IPv4 any-address
(NetworkAddress(port) -> INADDR_ANY): the probe must connect in the same
family, or a v6-only listener would look wedged forever. Empty, "*",
"0.0.0.0" -> 127.0.0.1; "::" (any spelling) -> ::1; anything else (a
specific address or host name) is probed as-is.
*/
std::string probeHostForListenInterface(const std::string &listenInterface);

//! Pure "server transport wedged" decision behind the coordinator's probe.
/*!
History: the probe used to restart the server epoch in-process after two
failed connects with no other condition. On 2026-09-25 it fired on epochs
that were still starting (a 10 s display wait), and on an epoch whose
listener had leaked into the same process (so every rebuild failed with
EADDRINUSE), restarting a broken epoch every 60 s forever. Now:

- probes run only while the running server epoch reports its listener
  bound (setListening(true)); a starting epoch is never probed;
- strikes count only after the listener has answered at least once in
  this epoch (hysteresis: a listener that never answered is a wiring or
  address-family problem, not a wedge -- it is logged, not restarted);
- a restart is requested at most once per \c cooldownS, and the detector
  disarms until the rebuilt epoch's listener answers again.

Time is injected (monotonic seconds) so the schedule is unit-tested.
*/
class WedgeDetector
{
public:
  struct Config
  {
    int strikesToRestart = 2;
    double cooldownS = 60.0;
  };

  WedgeDetector() = default;
  explicit WedgeDetector(Config config) : m_config(config)
  {
  }

  //! The running server epoch bound (true) or released (false) its listener.
  void setListening(bool listening)
  {
    m_listening = listening;
    m_armed = false;
    m_strikes = 0;
  }

  //! Whether a probe is worth attempting right now.
  bool shouldProbe() const
  {
    return m_listening;
  }

  //! Record a probe outcome; true when the server epoch must be restarted.
  bool recordProbe(bool ok, double now)
  {
    if (!m_listening) {
      return false;
    }
    if (ok) {
      m_armed = true;
      m_strikes = 0;
      return false;
    }
    if (!m_armed) {
      return false; // never answered yet: not a wedge we can judge
    }
    ++m_strikes;
    if (m_strikes < m_config.strikesToRestart || now - m_lastRestartAt < m_config.cooldownS) {
      return false;
    }
    m_strikes = 0;
    m_armed = false;
    m_lastRestartAt = now;
    return true;
  }

  bool listening() const
  {
    return m_listening;
  }
  bool armed() const
  {
    return m_armed;
  }
  int strikes() const
  {
    return m_strikes;
  }
  double lastRestartAt() const
  {
    return m_lastRestartAt;
  }

private:
  Config m_config;
  bool m_listening = false;
  bool m_armed = false;
  int m_strikes = 0;
  double m_lastRestartAt = -1.0e9;
};

} // namespace deskflow::coordination
