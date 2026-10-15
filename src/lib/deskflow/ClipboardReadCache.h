/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/Clipboard.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>

//! Reuses the last read of a platform clipboard while it is unchanged
/*!
The server re-reads the primary screen's clipboard on every leave, once per
clipboard id (macOS reports one pasteboard as both ids).  Reading a platform
clipboard converts and copies every format synchronously, and can make the
copying app serve its data again, so a large copy cost that on every
crossing.  A change counter that moves on every change (NSPasteboard
changeCount) proves the content is the same, so an unchanged clipboard is
served from the last read.
*/
class ClipboardReadCache
{
public:
  using ChangeCount = std::function<long()>;

  //! Bounds on one read of the platform clipboard
  struct Limits
  {
    //! A format bigger than this is dropped (0 = no cap)
    size_t maxFormatBytes = 8 * 1024 * 1024;
    //! Past this, formats after text are skipped (0 = no budget)
    std::chrono::milliseconds budget{250};
  };

  //! Copy \p source into \p dst, from the cache while \p changeCount is unchanged
  /*!
  Returns true when served from the cache.  \p changeCount keys the cache (the
  count the source snapshot belongs to); \p liveCount, when given, is the
  platform's live counter: a read during which it moved, or that started with
  it ahead of the key, is not cached.  Without it \p changeCount is used for
  both.
  */
  bool read(
      IClipboard *dst, const IClipboard &source, const ChangeCount &changeCount, const ChangeCount &liveCount = {}
  );

  //! Copy \p src into \p dst within \p limits (text first; see Limits)
  static bool copyBounded(IClipboard *dst, const IClipboard *src, const Limits &limits);

  void setLimits(const Limits &limits)
  {
    m_limits = limits;
  }

  //! Drop the cached copy (call before writing the platform clipboard)
  void invalidate();

  bool isValid() const
  {
    return m_valid;
  }

private:
  // heap so invalidate() really frees a large copy (Clipboard::empty() keeps capacity)
  std::unique_ptr<Clipboard> m_cache;
  bool m_valid = false;
  long m_changeCount = 0;
  Limits m_limits;
};
