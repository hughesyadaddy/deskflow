/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/ClipboardReadCache.h"

#include "base/Log.h"

bool ClipboardReadCache::copyBounded(IClipboard *dst, const IClipboard *src, const Limits &limits)
{
  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  const auto time = src->getTime();
  bool success = false;
  if (src->open(time)) {
    if (dst->open(time)) {
      if (dst->empty()) {
        for (int32_t format = 0; format != static_cast<int>(IClipboard::Format::TotalFormats); ++format) {
          const auto eFormat = static_cast<IClipboard::Format>(format);
          if (eFormat != IClipboard::Format::Text && limits.budget.count() > 0 &&
              Clock::now() - start > limits.budget) {
            LOG_WARN(
                "clipboard read over its %lld ms budget, sending text only", static_cast<long long>(limits.budget.count())
            );
            break;
          }
          if (!src->has(eFormat)) {
            continue;
          }
          auto data = src->get(eFormat);
          if (limits.maxFormatBytes != 0 && data.size() > limits.maxFormatBytes) {
            LOG_WARN(
                "skipping clipboard format %d: %zu bytes is over the %zu byte cap", format, data.size(),
                limits.maxFormatBytes
            );
            continue;
          }
          dst->add(eFormat, data);
        }
        success = true;
      }
      dst->close();
    }
    src->close();
  }
  return success;
}

bool ClipboardReadCache::read(
    IClipboard *dst, const IClipboard &source, const ChangeCount &changeCount, const ChangeCount &liveCount
)
{
  const ChangeCount &live = liveCount ? liveCount : changeCount;
  const long before = changeCount();
  if (m_valid && m_cache != nullptr && before == m_changeCount) {
    IClipboard::copy(dst, m_cache.get());
    return true;
  }

  invalidate();
  const long liveBefore = live();
  auto cache = std::make_unique<Clipboard>();
  if (!copyBounded(cache.get(), &source, m_limits)) {
    // source could not be opened: same result as reading it directly
    copyBounded(dst, &source, m_limits);
    return false;
  }
  IClipboard::copy(dst, cache.get());

  // changed mid-read: don't cache, the next read must go to the source
  if (changeCount() == before && live() == liveBefore && (!liveCount || liveBefore == before)) {
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
