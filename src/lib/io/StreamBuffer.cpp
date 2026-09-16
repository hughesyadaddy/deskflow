/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "io/StreamBuffer.h"

#include <algorithm>
#include <assert.h>
#include <iterator>

//
// StreamBuffer
//

const uint32_t StreamBuffer::kChunkSize = 4096;

const void *StreamBuffer::peek(uint32_t n)
{
  assert(n <= m_size);

  // if requesting no data then return nullptr so we don't try to access
  // an empty list.
  if (n == 0) {
    return nullptr;
  }

  auto head = m_chunks.begin();

  // fast path: the requested span already lies in the head chunk, so hand
  // out a pointer without touching any memory.
  if (static_cast<uint32_t>(head->size()) - m_headUsed >= n) {
    return static_cast<const void *>(head->data() + m_headUsed);
  }

  // drop the consumed prefix before consolidating so the head does not
  // carry dead bytes (and their capacity) across repeated consolidations.
  if (m_headUsed > 0) {
    head->erase(head->begin(), head->begin() + m_headUsed);
    m_headUsed = 0;
  }

  // grow the head geometrically: a sequence of growing peeks (e.g. a
  // reader probing for a full message) must not reallocate on every call.
  if (head->capacity() < n) {
    head->reserve(std::max<std::size_t>(n, head->capacity() * 2));
  }

  // consolidate chunks into the first chunk until it has n bytes
  auto scan = std::next(head);
  while (head->size() < n && scan != m_chunks.end()) {
    head->insert(head->end(), scan->begin(), scan->end());
    scan = m_chunks.erase(scan);
  }

  return static_cast<const void *>(head->data());
}

void StreamBuffer::pop(uint32_t n)
{
  // discard all chunks if n is greater than or equal to m_size
  if (n >= m_size) {
    m_size = 0;
    m_headUsed = 0;
    m_chunks.clear();
    return;
  }

  // update size
  m_size -= n;

  // discard chunks until more than n bytes would've been discarded
  auto scan = m_chunks.begin();
  assert(scan != m_chunks.end());
  while (scan->size() - m_headUsed <= n) {
    n -= (uint32_t)scan->size() - m_headUsed;
    m_headUsed = 0;
    scan = m_chunks.erase(scan);
    assert(scan != m_chunks.end());
  }

  // remove left over bytes from the head chunk
  if (n > 0) {
    m_headUsed += n;
  }

  // shrink policy: a head that was consolidated into a large chunk keeps
  // its full capacity until it is popped.  Once the unread remainder fits
  // in a normal chunk, move it into a fresh chunk and free the big one so
  // a transient burst (e.g. a clipboard transfer) does not pin its peak.
  Chunk &head = *scan;
  if (head.capacity() > 2 * static_cast<std::size_t>(kChunkSize)) {
    const auto remaining = static_cast<uint32_t>(head.size()) - m_headUsed;
    if (remaining <= kChunkSize) {
      Chunk compact;
      compact.reserve(kChunkSize);
      compact.assign(head.begin() + m_headUsed, head.end());
      head.swap(compact);
      m_headUsed = 0;
    }
  }
}

void StreamBuffer::write(const void *vdata, uint32_t n)
{
  assert(vdata != nullptr);

  // ignore if no data, otherwise update size
  if (n == 0) {
    return;
  }
  m_size += n;

  // cast data to bytes
  const auto *data = static_cast<const uint8_t *>(vdata);

  // point to last chunk if it has space, otherwise append an empty chunk
  auto scan = m_chunks.end();
  if (scan != m_chunks.begin()) {
    --scan;
    if (scan->size() >= kChunkSize) {
      ++scan;
    }
  }
  if (scan == m_chunks.end()) {
    scan = m_chunks.emplace(scan, Chunk());
    scan->reserve(kChunkSize);
  }

  // append data in chunks
  while (n > 0) {
    // choose number of bytes for next chunk
    assert(scan->size() <= kChunkSize);
    uint32_t count = kChunkSize - (uint32_t)scan->size();
    if (count > n)
      count = n;

    // transfer data
    scan->insert(scan->end(), data, data + count);
    n -= count;
    data += count;

    // append another empty chunk if we're not done yet
    if (n > 0) {
      ++scan;
      scan = m_chunks.emplace(scan, Chunk());
      scan->reserve(kChunkSize);
    }
  }
}

uint32_t StreamBuffer::getSize() const
{
  return m_size;
}

uint32_t StreamBuffer::getContiguousSize() const
{
  if (m_chunks.empty()) {
    return 0;
  }
  return static_cast<uint32_t>(m_chunks.front().size()) - m_headUsed;
}

std::size_t StreamBuffer::getCapacity() const
{
  std::size_t total = 0;
  for (const auto &chunk : m_chunks) {
    total += chunk.capacity();
  }
  return total;
}

std::size_t StreamBuffer::getChunkCount() const
{
  return m_chunks.size();
}

uint32_t StreamBuffer::chunkSize()
{
  return kChunkSize;
}
