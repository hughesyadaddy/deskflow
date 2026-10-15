/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

// ClipboardSender: a clipboard goes out one data chunk per output flush, so
// input written meanwhile is never queued behind the whole clipboard, and the
// receiver (ClipboardChunk::assemble, unchanged since protocol 1.6) still
// reassembles it exactly.

#include "arch/Arch.h"
#include "base/EventQueue.h"
#include "base/Log.h"
#include "deskflow/ClipboardChunk.h"
#include "deskflow/ClipboardSender.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "io/IStream.h"

#include <QTest>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

// In-memory stream: writes append, reads consume from the front.
class LoopbackStream : public deskflow::IStream
{
public:
  void close() override
  {
  }
  uint32_t read(void *buffer, uint32_t n) override
  {
    n = std::min<uint32_t>(n, static_cast<uint32_t>(m_data.size() - m_readPos));
    if (buffer != nullptr) {
      std::memcpy(buffer, m_data.data() + m_readPos, n);
    }
    m_readPos += n;
    return n;
  }
  void write(const void *buffer, uint32_t n) override
  {
    m_data.append(static_cast<const char *>(buffer), n);
  }
  void flush() override
  {
  }
  void shutdownInput() override
  {
  }
  void shutdownOutput() override
  {
  }
  void *getEventTarget() const override
  {
    return const_cast<LoopbackStream *>(this);
  }
  bool isReady() const override
  {
    return pending() > 0;
  }
  uint32_t getSize() const override
  {
    return static_cast<uint32_t>(pending());
  }
  size_t pending() const
  {
    return m_data.size() - m_readPos;
  }
  size_t written() const
  {
    return m_data.size();
  }

private:
  std::string m_data;
  size_t m_readPos = 0;
};

struct Message
{
  enum Kind
  {
    Start,
    Data,
    End,
    Mouse
  } kind;
  ClipboardID id = 0;
  size_t bytes = 0;
};

// Parse everything written so far; clipboard payloads reassemble into |assembled|.
std::vector<Message> drain(LoopbackStream &stream, std::string &assembled, bool &finished)
{
  std::vector<Message> out;
  while (stream.pending() >= 4) {
    char code[4];
    stream.read(code, 4);
    if (std::memcmp(code, kMsgDClipboard, 4) == 0) {
      ClipboardID id = 0;
      uint32_t seq = 0;
      const auto before = assembled.size();
      const auto r = ClipboardChunk::assemble(&stream, assembled, id, seq);
      if (r == TransferState::Started) {
        out.push_back({Message::Start, id, 0});
      } else if (r == TransferState::InProgress) {
        out.push_back({Message::Data, id, assembled.size() - before});
      } else if (r == TransferState::Finished) {
        out.push_back({Message::End, id, 0});
        finished = true;
      } else {
        qFatal("clipboard parse error");
      }
    } else if (std::memcmp(code, kMsgDMouseMove, 4) == 0) {
      int16_t x = 0;
      int16_t y = 0;
      ProtocolUtil::readf(&stream, kMsgDMouseMove + 4, &x, &y);
      out.push_back({Message::Mouse, 0, 0});
    } else {
      qFatal("unexpected message");
    }
  }
  return out;
}

std::string payload(size_t bytes, char seed)
{
  std::string s(bytes, seed);
  for (size_t i = 0; i < bytes; i += 97) {
    s[i] = static_cast<char>('a' + (i % 26));
  }
  return s;
}

int count(const std::vector<Message> &messages, Message::Kind kind)
{
  int n = 0;
  for (const auto &m : messages) {
    n += (m.kind == kind) ? 1 : 0;
  }
  return n;
}

} // namespace

class ClipboardSenderTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void sendsOneChunkPerFlush();
  void inputInterleavesBetweenChunks();
  void sendRemainingWritesEverythingNow();
  void newerDataRestartsTheTransfer();
  void otherClipboardQueuesBehind();
  void emptyClipboardCompletesAtOnce();
  void destroyedSenderIgnoresFlush();

private:
  void flush(EventQueue &events, LoopbackStream &stream)
  {
    events.dispatchEvent(Event(EventTypes::StreamOutputFlushed, stream.getEventTarget()));
  }

  std::unique_ptr<Arch> m_arch;
  Log m_log;
};

void ClipboardSenderTests::initTestCase()
{
  m_arch = std::make_unique<Arch>();
  m_log.setFilter(LogLevel::Level::Error);
}

void ClipboardSenderTests::sendsOneChunkPerFlush()
{
  EventQueue events;
  LoopbackStream stream;
  ClipboardSender sender(&events, &stream);
  const auto data = payload(5 * ClipboardSender::kChunkSize - 100, 'x');

  sender.sendClipboard(data, kClipboardClipboard, 7);
  std::string assembled;
  bool finished = false;
  auto messages = drain(stream, assembled, finished);
  // start + the first chunk only; NOT the whole clipboard
  QCOMPARE(count(messages, Message::Start), 1);
  QCOMPARE(count(messages, Message::Data), 1);
  QVERIFY(sender.isSending());

  int flushes = 0;
  while (!finished && flushes < 100) {
    flush(events, stream);
    ++flushes;
    messages = drain(stream, assembled, finished);
    QVERIFY(messages.size() <= 1); // never more than one message per flush
  }
  QVERIFY(finished);
  QCOMPARE(flushes, 5); // 4 more data chunks, then the end
  QVERIFY(assembled == data);
  QVERIFY(!sender.isSending());

  // a flush after completion writes nothing
  const auto before = stream.written();
  flush(events, stream);
  QCOMPARE(stream.written(), before);
}

void ClipboardSenderTests::inputInterleavesBetweenChunks()
{
  EventQueue events;
  LoopbackStream stream;
  ClipboardSender sender(&events, &stream);
  const auto data = payload(3 * ClipboardSender::kChunkSize + 5, 'y');

  sender.sendClipboard(data, kClipboardClipboard, 0);
  std::vector<Message> all;
  std::string assembled;
  bool finished = false;
  for (int i = 0; i < 10 && !finished; ++i) {
    ProtocolUtil::writef(&stream, kMsgDMouseMove, 10, 20); // input written while sending
    auto m = drain(stream, assembled, finished);
    all.insert(all.end(), m.begin(), m.end());
    flush(events, stream);
    m = drain(stream, assembled, finished);
    all.insert(all.end(), m.begin(), m.end());
  }
  QVERIFY(finished);
  QVERIFY(assembled == data);
  // the first mouse move goes out right after the first chunk, not after the end
  QCOMPARE(all[0].kind, Message::Start);
  QCOMPARE(all[1].kind, Message::Data);
  QCOMPARE(all[2].kind, Message::Mouse);
  QCOMPARE(all[3].kind, Message::Data);
  QCOMPARE(all[4].kind, Message::Mouse);
}

void ClipboardSenderTests::sendRemainingWritesEverythingNow()
{
  EventQueue events;
  LoopbackStream stream;
  ClipboardSender sender(&events, &stream);
  const auto data = payload(4 * ClipboardSender::kChunkSize + 1, 'z');

  sender.sendClipboard(data, kClipboardClipboard, 0);
  sender.sendRemaining();
  QVERIFY(!sender.isSending());
  ProtocolUtil::writef(&stream, kMsgDMouseMove, 1, 2); // e.g. the paste key

  std::string assembled;
  bool finished = false;
  const auto messages = drain(stream, assembled, finished);
  QVERIFY(finished);
  QVERIFY(assembled == data);
  QCOMPARE(messages.back().kind, Message::Mouse); // input lands after the clipboard
  QCOMPARE(messages[messages.size() - 2].kind, Message::End);
}

void ClipboardSenderTests::newerDataRestartsTheTransfer()
{
  EventQueue events;
  LoopbackStream stream;
  ClipboardSender sender(&events, &stream);
  const auto oldData = payload(3 * ClipboardSender::kChunkSize, 'o');
  const auto newData = payload(2 * ClipboardSender::kChunkSize + 3, 'n');

  sender.sendClipboard(oldData, kClipboardClipboard, 0);
  flush(events, stream);
  sender.sendClipboard(newData, kClipboardClipboard, 0); // same id, newer data
  sender.sendRemaining();

  std::string assembled;
  bool finished = false;
  const auto messages = drain(stream, assembled, finished);
  QVERIFY(finished);
  QCOMPARE(count(messages, Message::Start), 2);
  QCOMPARE(count(messages, Message::End), 1);
  QVERIFY(assembled == newData); // receiver restarted on the second start
}

void ClipboardSenderTests::otherClipboardQueuesBehind()
{
  EventQueue events;
  LoopbackStream stream;
  ClipboardSender sender(&events, &stream);
  const auto first = payload(2 * ClipboardSender::kChunkSize + 9, 'a');
  const auto second = payload(ClipboardSender::kChunkSize / 2, 'b');

  sender.sendClipboard(first, kClipboardClipboard, 0);
  sender.sendClipboard(second, kClipboardSelection, 0);
  sender.sendRemaining();

  std::string assembled;
  bool finished = false;
  std::vector<Message> messages;
  // parse the first transfer completely before the second begins
  bool sawFirstEnd = false;
  while (stream.pending() >= 4) {
    bool f = false;
    auto m = drain(stream, assembled, f);
    messages.insert(messages.end(), m.begin(), m.end());
    finished = finished || f;
  }
  for (size_t i = 0; i < messages.size(); ++i) {
    if (messages[i].kind == Message::End && messages[i].id == kClipboardClipboard) {
      sawFirstEnd = true;
    }
    if (messages[i].id == kClipboardSelection) {
      QVERIFY2(sawFirstEnd, "clipboards must never interleave");
    }
  }
  QVERIFY(finished);
  QCOMPARE(count(messages, Message::Start), 2);
  QCOMPARE(count(messages, Message::End), 2);
  QVERIFY(assembled == second); // last transfer reassembled intact
  QVERIFY(!sender.isSending());
}

void ClipboardSenderTests::emptyClipboardCompletesAtOnce()
{
  EventQueue events;
  LoopbackStream stream;
  ClipboardSender sender(&events, &stream);
  sender.sendClipboard(std::string(), kClipboardClipboard, 0);
  QVERIFY(!sender.isSending());
  std::string assembled;
  bool finished = false;
  const auto messages = drain(stream, assembled, finished);
  QVERIFY(finished);
  QCOMPARE(messages.size(), size_t(2));
  QVERIFY(assembled.empty());
}

void ClipboardSenderTests::destroyedSenderIgnoresFlush()
{
  EventQueue events;
  LoopbackStream stream;
  {
    ClipboardSender sender(&events, &stream);
    sender.sendClipboard(payload(3 * ClipboardSender::kChunkSize, 'd'), kClipboardClipboard, 0);
  }
  const auto before = stream.written();
  QVERIFY(!events.dispatchEvent(Event(EventTypes::StreamOutputFlushed, stream.getEventTarget())));
  QCOMPARE(stream.written(), before);
}

QTEST_APPLESS_MAIN(ClipboardSenderTests)
#include "ClipboardSenderTests.moc"
