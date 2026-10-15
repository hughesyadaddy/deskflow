// SPDX-FileCopyrightText: (C) 2026 Deskflow Contributors
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
//
// Remote-desktop / display-sleep input reclaim (Oct 15 03:56 ET incident:
// a RustDesk session on the server could not move or click because the
// server's cursor had been parked on a sleeping client since 02:10, so every
// remote-injected event was captured and forwarded there).
//
// Rules (pure, unit-tested):
//  * Input injected by a remote-desktop tool is never captured: the server
//    brings its cursor home and lets the event through.
//  * While such a session is active (input within kRemoteHold), the remote
//    cursor touching a screen edge does NOT cross onto a client.
//  * Physical input and Deskflow's own crossing are untouched, so moving the
//    real mouse onto a sleeping client still crosses and wakes it.

#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <string>
#include <string_view>

namespace deskflow::reclaim {

using Clock = std::chrono::steady_clock;
inline constexpr std::chrono::seconds kRemoteHold{10};

//! True when an executable path belongs to a remote-desktop / screen-sharing tool.
inline bool isRemoteDesktopProcessPath(std::string_view path)
{
  if (path.empty()) {
    return false;
  }
  std::string lower(path);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
  static constexpr std::string_view kMarkers[] = {
      "rustdesk",      "teamviewer",     "screensharingd",      "ardagent",     "screensharingagent",
      "remoting_me2me", "chrome remote", "chromeremotedesktop", "jump desktop", "jumpconnect",
      "anydesk",       "parsec",         "splashtop",           "nomachine",    "remotedesktop",
      "remote desktop", "microsoft remote desktop", "windows app.app", "screens connect",
      // VNC tools by name only: a bare "vnc" substring matched unrelated paths
      "realvnc",       "tightvnc",       "tigervnc",            "ultravnc",     "x11vnc",
      "vncserver",     "vncviewer",      "vnc viewer",          "vnc server",
  };
  for (const auto marker : kMarkers) {
    if (lower.find(marker) != std::string::npos) {
      return true;
    }
  }
  return false;
}

//! Hold window after the last remote-desktop event. Atomic: the event tap
//! arms it synchronously, the server's event loop reads it.
class RemoteInputHold
{
public:
  void note(Clock::time_point now)
  {
    m_untilNs.store((now + kRemoteHold).time_since_epoch().count(), std::memory_order_release);
  }
  bool active(Clock::time_point now) const
  {
    return now.time_since_epoch().count() < m_untilNs.load(std::memory_order_acquire);
  }
  void reset()
  {
    m_untilNs.store(0, std::memory_order_release);
  }

private:
  std::atomic<Clock::rep> m_untilNs{0};
};

//! The one hold shared by the primary screen's tap and the server.
inline RemoteInputHold &sharedRemoteHold()
{
  static RemoteInputHold hold;
  return hold;
}

} // namespace deskflow::reclaim
