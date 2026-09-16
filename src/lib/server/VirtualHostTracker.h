/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <set>
#include <string>
#include <utility>

class BaseClientProxy;

//! Tracks which remote client hosts the Mouser virtual device.
/*!
Focus is a notice, not a device lifecycle: a client receives the cached
`connect` line once (the first time it gains focus, or again after the
device identity changes) and from then on only `{"t":"focus",...}` lines
follow the cursor. `disconnect` is reserved for the device really going
away (detach). Used only on the Server event thread (not thread-safe).
*/
class VirtualHostTracker
{
public:
  static constexpr const char *kDefaultDisconnect = R"({"type": "disconnect"})";

  //! `{"t":"focus","screen":<screen>,"here":<here>}` (compact JSON).
  [[nodiscard]] static std::string focusLine(const std::string &screen, bool here);

  //! Cache connect JSON; pass an empty string to clear. A changed device
  //! identity is re-announced to every client on its next focus.
  void setConnectLine(std::string line = {});
  [[nodiscard]] bool hasConnectLine() const;
  [[nodiscard]] const std::string &connectLine() const;

  [[nodiscard]] BaseClientProxy *host() const;
  //! Forget a client that went away (no lines are sent).
  void clearHostIf(BaseClientProxy *client);
  [[nodiscard]] bool hostsActiveClient(BaseClientProxy *active) const;
  [[nodiscard]] bool isAttached(const BaseClientProxy *client) const;

  //! Focus moved to \p dst (named \p screenName). \p send receives
  //! (client, line) for every line to relay.
  template<typename SendFn>
  void onFocusChange(
      BaseClientProxy *dst, BaseClientProxy *primary, const std::string &screenName, SendFn &&send,
      const std::string &connectPayload = {}
  )
  {
    if (dst == nullptr || primary == nullptr) {
      return;
    }

    BaseClientProxy *previous = m_host;
    if (dst == primary) {
      if (previous != nullptr) {
        send(previous, focusLine(screenName, false));
        m_host = nullptr;
      }
      return;
    }

    const std::string &line = connectPayload.empty() ? m_connectLine : connectPayload;
    if (line.empty()) {
      return;
    }
    if (previous != nullptr && previous != dst) {
      send(previous, focusLine(screenName, false));
    }
    const bool newlyAttached = m_attached.insert(dst).second;
    if (newlyAttached) {
      send(dst, line);
    }
    if (newlyAttached || previous != dst) {
      send(dst, focusLine(screenName, true));
    }
    m_host = dst;
  }

  //! The device is gone: every attached client gets \p disconnectLine.
  template<typename SendFn>
  void detach(SendFn &&send, const std::string &disconnectLine = kDefaultDisconnect)
  {
    for (auto *client : m_attached) {
      send(client, disconnectLine);
    }
    m_attached.clear();
    m_host = nullptr;
  }

private:
  BaseClientProxy *m_host = nullptr;
  std::set<BaseClientProxy *> m_attached;
  std::string m_connectLine;
};
