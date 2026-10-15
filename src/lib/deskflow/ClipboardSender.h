/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2013 - 2016, 2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/ClipboardTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

class IEventQueue;

namespace deskflow {
class IStream;
}

//! Paced clipboard transfer over one stream
/*!
Writes a clipboard as start / data / end ClipboardChunk messages, but only
ONE data chunk per StreamOutputFlushed, so mouse and keyboard messages
written meanwhile reach the peer between chunks instead of queueing behind
the whole clipboard (the old StreamChunker posted every chunk at once).

The wire format is unchanged: a receiver since protocol 1.6 accepts any
chunk size, other messages between chunks, and restarts on a new start
chunk.  Two clipboards are never interleaved (a receiver has a single
assembly buffer): a different clipboard id queues behind the current one;
newer data for the clipboard in flight restarts it.

Ported from Synergy's ClipboardSender (symless/synergy 309111cb, deskflow
57d16c89 "pace clipboard chunks so input isn't stuck behind them").
*/
class ClipboardSender
{
public:
  ClipboardSender(IEventQueue *events, deskflow::IStream *stream);
  ClipboardSender(ClipboardSender const &) = delete;
  ClipboardSender(ClipboardSender &&) = delete;
  ~ClipboardSender();

  ClipboardSender &operator=(ClipboardSender const &) = delete;
  ClipboardSender &operator=(ClipboardSender &&) = delete;

  //! Start (or queue) sending marshalled clipboard \p data
  void sendClipboard(std::string data, ClipboardID id, uint32_t sequence);

  //! Write everything still pending now, so the next message follows it
  /*!
  Used before a key press or click, which may be a paste.
  */
  void sendRemaining();

  //! True while a transfer is in flight or queued
  bool isSending() const
  {
    return m_sending;
  }

  //! Data chunk size (one per flush)
  static constexpr size_t kChunkSize = 64 * 1024;

private:
  struct Transfer
  {
    std::string data;
    uint32_t sequence = 0;
  };

  void beginTransfer(ClipboardID id, Transfer transfer);
  void sendNextChunk();

  IEventQueue *m_events;
  deskflow::IStream *m_stream;
  void *m_streamTarget;
  Transfer m_current;
  ClipboardID m_currentId = 0;
  size_t m_sent = 0;
  bool m_sending = false;
  std::array<std::optional<Transfer>, kClipboardEnd> m_queued;
};
