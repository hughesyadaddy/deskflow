/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/EscDropout.h"

namespace deskflow::coordination {
namespace {
class StubEscDropoutMonitor : public IEscDropoutMonitor
{
public:
  bool start(std::function<void()>) override
  {
    return false;
  }
  void stop() override
  {
  }
};
} // namespace

std::unique_ptr<IEscDropoutMonitor> createEscDropoutMonitor()
{
  return std::make_unique<StubEscDropoutMonitor>();
}
} // namespace deskflow::coordination
