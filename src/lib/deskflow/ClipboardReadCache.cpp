/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/ClipboardReadCache.h"

bool ClipboardReadCache::read(IClipboard *dst, const IClipboard &source, const ChangeCount &changeCount)
{
  const long before = changeCount();
  if (m_valid && m_cache != nullptr && before == m_changeCount) {
    IClipboard::copy(dst, m_cache.get());
    return true;
  }

  invalidate();
  auto cache = std::make_unique<Clipboard>();
  if (!IClipboard::copy(cache.get(), &source)) {
    // source could not be opened: same result as reading it directly
    IClipboard::copy(dst, &source);
    return false;
  }
  IClipboard::copy(dst, cache.get());

  // changed mid-read: don't cache, the next read must go to the source
  if (changeCount() == before) {
    m_cache = std::move(cache);
    m_valid = true;
    m_changeCount = before;
  }
  return false;
}

void ClipboardReadCache::invalidate()
{
  m_valid = false;
  m_cache.reset();
}
