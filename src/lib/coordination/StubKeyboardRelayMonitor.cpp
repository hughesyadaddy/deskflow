/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/KeyboardRelayMonitor.h"

namespace deskflow::coordination {

class StubKeyboardRelayMonitor : public IKeyboardRelayMonitor
{
public:
  bool start(RelayPassThroughQuery, KeyForwardSend) override
  {
    m_running = true;
    return true;
  }
  void stop() override
  {
    m_running = false;
  }
  bool running() const override
  {
    return m_running;
  }

private:
  bool m_running = false;
};

std::unique_ptr<IKeyboardRelayMonitor> createKeyboardRelayMonitor()
{
  return std::make_unique<StubKeyboardRelayMonitor>();
}

} // namespace deskflow::coordination
