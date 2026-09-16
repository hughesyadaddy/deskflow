/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 *
 * Mouser bridge (fork extension), thinned: the loopback listener that used
 * to live here (and die with every Server) is now deskflow::MouserLink, a
 * process-lifetime connector. What remains is the event payload the link
 * thread posts so the Server handles Mouser lines on its own event loop.
 */

#pragma once

#include "base/Event.h"

#include <string>

//! Event data for EventTypes::ServerMouserBridgeLine.
class MouserBridgeLineData : public EventData
{
public:
  explicit MouserBridgeLineData(std::string line) : m_line(std::move(line))
  {
    // do nothing
  }
  ~MouserBridgeLineData() override = default;

  const std::string &line() const
  {
    return m_line;
  }

private:
  std::string m_line;
};
