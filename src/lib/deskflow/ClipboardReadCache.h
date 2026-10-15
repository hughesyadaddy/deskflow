/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/Clipboard.h"

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

  //! Copy \p source into \p dst, from the cache while \p changeCount is unchanged
  /*!
  Returns true when served from the cache.  A read during which the counter
  moved is not cached.
  */
  bool read(IClipboard *dst, const IClipboard &source, const ChangeCount &changeCount);

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
};
