/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <cstdint>
#include <list>
#include <vector>

//! FIFO of bytes
/*!
This class maintains a FIFO (first-in, last-out) buffer of bytes.
*/
class StreamBuffer
{
public:
  StreamBuffer() = default;
  ~StreamBuffer() = default;

  //! @name manipulators
  //@{

  //! Read data without removing from buffer
  /*!
  Return a pointer to memory with the next \c n bytes in the buffer
  (which must be <= getSize()).  The caller must not modify the returned
  memory nor delete it.

  If the requested span already lies inside the head chunk no bytes are
  copied.  Otherwise the following chunks are consolidated into the head
  chunk; the head's capacity is grown geometrically so that a sequence of
  growing peeks does not reallocate on every call.
  */
  const void *peek(uint32_t n);

  //! Discard data
  /*!
  Discards the next \c n bytes.  If \c n >= getSize() then the buffer
  is cleared.  Chunks that become fully consumed are freed, and a head
  chunk that grew large through consolidation is compacted once the
  remaining bytes fit in a normal chunk, so a transient burst does not
  pin its peak allocation.
  */
  void pop(uint32_t n);

  //! Write data to buffer
  /*!
  Appends \c n bytes from \c data to the buffer.
  */
  void write(const void *data, uint32_t n);

  //@}
  //! @name accessors
  //@{

  //! Get size of buffer
  /*!
  Returns the number of bytes in the buffer.
  */
  uint32_t getSize() const;

  //! Get contiguous size
  /*!
  Returns the number of bytes that \c peek() can return without copying,
  i.e. the unread bytes in the head chunk.
  */
  uint32_t getContiguousSize() const;

  //! Get allocated capacity
  /*!
  Returns the sum of the capacities of all chunks, in bytes.  This is the
  memory the buffer currently pins (excluding list node overhead).
  */
  std::size_t getCapacity() const;

  //! Get chunk count
  std::size_t getChunkCount() const;

  //! Nominal chunk size in bytes
  static uint32_t chunkSize();

  //@}

private:
  static const uint32_t kChunkSize;

  using Chunk = std::vector<uint8_t>;
  using ChunkList = std::list<Chunk>;

  ChunkList m_chunks;
  uint32_t m_size = 0;
  uint32_t m_headUsed = 0;
};
