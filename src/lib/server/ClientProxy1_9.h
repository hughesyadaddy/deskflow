/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "server/ClientProxy1_8.h"

//! Protocol 1.9 client: understands DMWX, so every wheel event goes out
//! extended (a legacy notch stream is re-expressed as whole lines).
class ClientProxy1_9 : public ClientProxy1_8
{
public:
  ClientProxy1_9(const std::string &name, deskflow::IStream *adoptedStream, Server *server, IEventQueue *events);
  ~ClientProxy1_9() override = default;

  void mouseWheel(int32_t xDelta, int32_t yDelta) override;
  void mouseWheelEx(const WheelEx &ex) override;
};
