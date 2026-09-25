/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/WedgeDetector.h"

#include <algorithm>
#include <cctype>

namespace deskflow::coordination {

std::string probeHostForListenInterface(const std::string &listenInterface)
{
  std::string host = listenInterface;
  host.erase(
      std::remove_if(host.begin(), host.end(), [](unsigned char c) { return std::isspace(c) != 0; }), host.end()
  );
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
    host = host.substr(1, host.size() - 2);
  }
  if (host.empty() || host == "*" || host == "0.0.0.0") {
    return "127.0.0.1";
  }
  if (host == "::" || host == "::0" || host == "0:0:0:0:0:0:0:0") {
    return "::1";
  }
  return host;
}

} // namespace deskflow::coordination
