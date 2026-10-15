/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2015 - 2016 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/ClipboardSender.h"
#include "server/ClientProxy1_5.h"

class Server;
class IEventQueue;

//! Proxy for client implementing protocol version 1.6
class ClientProxy1_6 : public ClientProxy1_5
{
public:
  ClientProxy1_6(const std::string &name, deskflow::IStream *adoptedStream, Server *server, IEventQueue *events);
  ~ClientProxy1_6() override = default;

  void setClipboard(ClipboardID id, const IClipboard *clipboard) override;
  void finishClipboardTransfers() override;
  bool recvClipboard() override;

private:
  IEventQueue *m_events;
  // paced: one chunk per output flush, so input is never stuck behind a clipboard
  ClipboardSender m_clipboardSender;
};
