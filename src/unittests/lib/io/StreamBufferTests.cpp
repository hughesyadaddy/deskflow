/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "StreamBufferTests.h"

#include "io/StreamBuffer.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

std::vector<uint8_t> pattern(uint32_t n, uint32_t seed = 0)
{
  std::vector<uint8_t> data(n);
  for (uint32_t i = 0; i < n; ++i) {
    data[i] = static_cast<uint8_t>((i + seed) % 251);
  }
  return data;
}

} // namespace

void StreamBufferTests::peekWithinHead_doesNotCopyOrReallocate()
{
  StreamBuffer buffer;
  const auto data = pattern(3000);
  buffer.write(data.data(), 3000);

  const auto capacityBefore = buffer.getCapacity();
  QCOMPARE(buffer.getChunkCount(), std::size_t{1});
  QCOMPARE(buffer.getContiguousSize(), 3000u);

  const auto *p1 = static_cast<const uint8_t *>(buffer.peek(1000));
  QVERIFY(p1 != nullptr);
  QCOMPARE(buffer.getCapacity(), capacityBefore);

  // a larger span still inside the head returns the same storage
  const auto *p2 = static_cast<const uint8_t *>(buffer.peek(3000));
  QCOMPARE(p2, p1);
  QCOMPARE(buffer.getCapacity(), capacityBefore);
  QVERIFY(memcmp(p2, data.data(), 3000) == 0);

  // after a partial pop the head offset advances, still no copy
  buffer.pop(100);
  const auto *p3 = static_cast<const uint8_t *>(buffer.peek(2900));
  QCOMPARE(p3, p1 + 100);
  QCOMPARE(buffer.getCapacity(), capacityBefore);
  QCOMPARE(buffer.getContiguousSize(), 2900u);
}

void StreamBufferTests::peekAcrossChunks_consolidatesOnce()
{
  StreamBuffer buffer;
  const auto data = pattern(10000);
  buffer.write(data.data(), 10000);
  QCOMPARE(buffer.getChunkCount(), std::size_t{3});

  const auto *p1 = static_cast<const uint8_t *>(buffer.peek(10000));
  QCOMPARE(buffer.getChunkCount(), std::size_t{1});
  QCOMPARE(buffer.getContiguousSize(), 10000u);
  QVERIFY(memcmp(p1, data.data(), 10000) == 0);

  // a second peek of the same span is free
  const auto capacityAfter = buffer.getCapacity();
  const auto *p2 = static_cast<const uint8_t *>(buffer.peek(10000));
  QCOMPARE(p2, p1);
  QCOMPARE(buffer.getCapacity(), capacityAfter);
  QCOMPARE(buffer.getChunkCount(), std::size_t{1});
}

void StreamBufferTests::peekGrowing_amortizesReallocation()
{
  StreamBuffer buffer;
  const uint32_t chunk = StreamBuffer::chunkSize();
  const uint32_t total = 16 * chunk;
  const auto data = pattern(total);
  buffer.write(data.data(), total);
  QCOMPARE(buffer.getChunkCount(), std::size_t{16});

  // a reader probing for a growing message peeks 1 byte past each chunk
  // boundary in turn; the head must grow geometrically, not per peek.
  // (head capacity = total capacity minus the untouched trailing chunks)
  const auto headCapacity = [&]() { return buffer.getCapacity() - (buffer.getChunkCount() - 1) * chunk; };
  std::size_t lastCapacity = headCapacity();
  int growths = 0;
  for (uint32_t n = chunk + 1; n <= total; n += chunk) {
    const auto *p = static_cast<const uint8_t *>(buffer.peek(n));
    QVERIFY(memcmp(p, data.data(), n) == 0);
    const auto capacity = headCapacity();
    if (capacity != lastCapacity) {
      ++growths;
      lastCapacity = capacity;
    }
  }
  // 4 KiB -> 8 -> 16 -> 32 -> 64 KiB is four reallocations; the old
  // exact-fit reserve reallocated on every one of the 16 peeks.
  QVERIFY2(growths <= 5, qPrintable(QString("head reallocated %1 times").arg(growths)));
  QCOMPARE(buffer.getChunkCount(), std::size_t{1});
}

void StreamBufferTests::popDrain_releasesChunks()
{
  StreamBuffer buffer;
  const uint32_t total = 1024 * 1024;
  const auto data = pattern(total);
  buffer.write(data.data(), total);
  QVERIFY(buffer.getCapacity() >= total);

  // consolidate into one big head, as a full-buffer write pass would
  buffer.peek(total);
  QCOMPARE(buffer.getChunkCount(), std::size_t{1});

  buffer.pop(total);
  QCOMPARE(buffer.getSize(), 0u);
  QCOMPARE(buffer.getChunkCount(), std::size_t{0});
  QCOMPARE(buffer.getCapacity(), std::size_t{0});

  // and via chunk-by-chunk consumption without prior consolidation
  buffer.write(data.data(), total);
  uint32_t left = total;
  while (left > 0) {
    const uint32_t n = std::min(left, 4000u);
    buffer.pop(n);
    left -= n;
  }
  QCOMPARE(buffer.getSize(), 0u);
  QCOMPARE(buffer.getCapacity(), std::size_t{0});
}

void StreamBufferTests::popPartial_compactsOversizedHead()
{
  StreamBuffer buffer;
  const uint32_t total = 1024 * 1024;
  const auto data = pattern(total);
  buffer.write(data.data(), total);
  buffer.peek(total);
  QVERIFY(buffer.getCapacity() >= total);

  // consume all but a tail that fits in a normal chunk: the 1 MiB head
  // must not stay pinned behind 100 bytes
  buffer.pop(total - 100);
  QCOMPARE(buffer.getSize(), 100u);
  QVERIFY2(
      buffer.getCapacity() <= StreamBuffer::chunkSize(),
      qPrintable(QString("capacity still %1 bytes").arg(buffer.getCapacity()))
  );
  QCOMPARE(buffer.getContiguousSize(), 100u);
  const auto *p = static_cast<const uint8_t *>(buffer.peek(100));
  QVERIFY(memcmp(p, data.data() + total - 100, 100) == 0);

  // subsequent writes append normally
  buffer.write(data.data(), 50);
  QCOMPARE(buffer.getSize(), 150u);
  const auto *q = static_cast<const uint8_t *>(buffer.peek(150));
  QVERIFY(memcmp(q, data.data() + total - 100, 100) == 0);
  QVERIFY(memcmp(q + 100, data.data(), 50) == 0);
}

void StreamBufferTests::fifoOrder_preservedAcrossConsolidation()
{
  StreamBuffer buffer;
  const uint32_t total = 50000;
  const auto data = pattern(total, 7);

  // write in uneven pieces so chunk boundaries fall mid-message
  uint32_t written = 0;
  uint32_t piece = 1;
  while (written < total) {
    const uint32_t n = std::min(total - written, piece);
    buffer.write(data.data() + written, n);
    written += n;
    piece = (piece * 3 + 1) % 9000 + 1;
  }
  QCOMPARE(buffer.getSize(), total);

  // read back in uneven pieces, some contiguous, some spanning chunks
  uint32_t consumed = 0;
  uint32_t take = 5;
  while (consumed < total) {
    const uint32_t n = std::min(total - consumed, take);
    const auto *p = static_cast<const uint8_t *>(buffer.peek(n));
    QVERIFY2(memcmp(p, data.data() + consumed, n) == 0, qPrintable(QString("mismatch at %1").arg(consumed)));
    buffer.pop(n);
    consumed += n;
    take = (take * 7 + 3) % 12000 + 1;
  }
  QCOMPARE(buffer.getSize(), 0u);
  QCOMPARE(buffer.getCapacity(), std::size_t{0});
}

QTEST_MAIN(StreamBufferTests)
