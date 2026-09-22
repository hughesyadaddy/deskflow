/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

namespace deskflow::gui {

//! Who owns this GUI process's launch on macOS, decided from three facts.
/*!
  Three launchers can start the GUI at login: the fleet LaunchAgent
  (~/Library/LaunchAgents/io.github.hughesyadaddy.deskflow.plist, which sets
  DESKFLOW_LAUNCHD=1 in the child's environment), a BTM / LaunchServices login
  item (SMAppService or a legacy record), and a plain `open`. Only the launchd
  copy is canonical: KeepAlive and the converge agent respawn it, so it must
  never step aside, and it is the one allowed to take over an unmanaged peer.
  Everything else honours a quit request from that peer and dies.

  The decision is a pure function of its inputs so the policy can be unit
  tested without launchd, SMAppService or a home directory.
*/
struct LaunchOwnership
{
  enum class LoginItem
  {
    None,      //!< leave the SMAppService registration as it is
    Unregister //!< remove this bundle's SMAppService login item
  };

  //! True only when launchd started this very process (DESKFLOW_LAUNCHD=1).
  //! A plist on disk says nothing about *this* process: the BTM copy sees the
  //! same file and would otherwise also claim to be launchd's.
  bool isLaunchdProcess = false;
  //! True when this process must quit when a peer asks (the unmanaged copy).
  bool honoursQuit = true;
  //! True when this process may ask an unmanaged peer to quit and take its
  //! lock (only launchd's copy).
  bool mayTakeOver = false;
  //! What to do with the SMAppService login item. Never Register: the fleet
  //! LaunchAgent is the only launcher, and a Login Item next to it races the
  //! agent at login (one copy exits 5).
  LoginItem loginItem = LoginItem::None;
};

//! \p envLaunchd: DESKFLOW_LAUNCHD == "1" in this process's environment.
//! \p agentPlistInstalled: the fleet GUI LaunchAgent plist exists on disk.
//! \p loginItemEnabled: SMAppService reports this bundle's login item enabled.
inline LaunchOwnership decideLaunchOwnership(bool envLaunchd, bool agentPlistInstalled, bool loginItemEnabled)
{
  LaunchOwnership o;
  o.isLaunchdProcess = envLaunchd;
  o.mayTakeOver = envLaunchd;
  o.honoursQuit = !envLaunchd;
  o.loginItem = ((agentPlistInstalled || envLaunchd) && loginItemEnabled) ? LaunchOwnership::LoginItem::Unregister
                                                                          : LaunchOwnership::LoginItem::None;
  return o;
}

} // namespace deskflow::gui
