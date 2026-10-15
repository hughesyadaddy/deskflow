/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

// ClipboardReadCache: an unchanged platform clipboard (same change count) is
// served from the last read instead of being re-read format by format.

#include "arch/Arch.h"
#include "base/Log.h"
#include "deskflow/Clipboard.h"
#include "deskflow/ClipboardReadCache.h"

#include <QTest>

#include "deskflow/PasteChord.h"

#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace {

class CountingClipboard : public Clipboard
{
public:
  std::string get(Format format) const override
  {
    ++m_gets;
    return Clipboard::get(format);
  }
  void setText(const std::string &text)
  {
    open(0);
    empty();
    add(Format::Text, text);
    close();
  }
  mutable int m_gets = 0;
};

class SlowClipboard : public CountingClipboard
{
public:
  std::string get(Format format) const override
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return CountingClipboard::get(format);
  }
};

void fill(IClipboard &clipboard, const std::string &text, const std::string &html)
{
  clipboard.open(0);
  clipboard.empty();
  clipboard.add(IClipboard::Format::Text, text);
  clipboard.add(IClipboard::Format::HTML, html);
  clipboard.close();
}

bool hasFormat(const IClipboard &clipboard, IClipboard::Format format)
{
  clipboard.open(0);
  const bool has = clipboard.has(format);
  clipboard.close();
  return has;
}

std::string textOf(const IClipboard &clipboard)
{
  clipboard.open(0);
  auto text = clipboard.has(IClipboard::Format::Text) ? clipboard.get(IClipboard::Format::Text) : std::string();
  clipboard.close();
  return text;
}

} // namespace

class ClipboardReadCacheTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void unchangedCountServesCache();
  void changedCountRereads();
  void changeDuringReadIsNotCached();
  void invalidateForcesReread();
  void liveCountMovingIsNotCached();
  void liveAheadOfKeyIsNotCached();
  void oversizedFormatIsSkipped();
  void overBudgetSendsTextOnly();
  void onlyPasteChordsFlush();

private:
  std::unique_ptr<Arch> m_arch;
  Log m_log;
};

void ClipboardReadCacheTests::initTestCase()
{
  m_arch = std::make_unique<Arch>();
  m_log.setFilter(LogLevel::Level::Error);
}

void ClipboardReadCacheTests::unchangedCountServesCache()
{
  CountingClipboard source;
  source.setText("hello");
  ClipboardReadCache cache;
  long count = 5;

  Clipboard a;
  QVERIFY(!cache.read(&a, source, [&] { return count; }));
  const int afterFirst = source.m_gets;
  QVERIFY(afterFirst > 0);
  QCOMPARE(textOf(a), std::string("hello"));

  Clipboard b;
  QVERIFY(cache.read(&b, source, [&] { return count; }));
  QCOMPARE(source.m_gets, afterFirst); // source not read again
  QCOMPARE(textOf(b), std::string("hello"));
}

void ClipboardReadCacheTests::changedCountRereads()
{
  CountingClipboard source;
  source.setText("one");
  ClipboardReadCache cache;
  long count = 1;
  Clipboard dst;
  cache.read(&dst, source, [&] { return count; });

  source.setText("two");
  count = 2;
  Clipboard dst2;
  QVERIFY(!cache.read(&dst2, source, [&] { return count; }));
  QCOMPARE(textOf(dst2), std::string("two"));
}

void ClipboardReadCacheTests::changeDuringReadIsNotCached()
{
  CountingClipboard source;
  source.setText("racy");
  ClipboardReadCache cache;
  long count = 0;
  const auto moving = [&] { return count++; }; // moves on every query

  Clipboard dst;
  QVERIFY(!cache.read(&dst, source, moving));
  QCOMPARE(textOf(dst), std::string("racy")); // caller still gets the data
  QVERIFY(!cache.isValid());
}

void ClipboardReadCacheTests::invalidateForcesReread()
{
  CountingClipboard source;
  source.setText("data");
  ClipboardReadCache cache;
  const auto fixed = [] { return 9L; };
  Clipboard dst;
  cache.read(&dst, source, fixed);
  QVERIFY(cache.isValid());

  cache.invalidate(); // e.g. we wrote the platform clipboard ourselves
  QVERIFY(!cache.isValid());
  const int before = source.m_gets;
  Clipboard dst2;
  QVERIFY(!cache.read(&dst2, source, fixed));
  QVERIFY(source.m_gets > before);
}

void ClipboardReadCacheTests::liveCountMovingIsNotCached()
{
  CountingClipboard source;
  source.setText("racy");
  ClipboardReadCache cache;
  long live = 5;
  Clipboard dst;
  QVERIFY(!cache.read(&dst, source, [] { return 5L; }, [&] { return live++; }));
  QCOMPARE(textOf(dst), std::string("racy"));
  QVERIFY(!cache.isValid());
}

void ClipboardReadCacheTests::liveAheadOfKeyIsNotCached()
{
  CountingClipboard source;
  source.setText("snapshot");
  ClipboardReadCache cache;
  Clipboard dst;
  // a copy landed after the last poll: the snapshot read must not be cached
  QVERIFY(!cache.read(&dst, source, [] { return 5L; }, [] { return 6L; }));
  QVERIFY(!cache.isValid());
  Clipboard dst2;
  QVERIFY(!cache.read(&dst2, source, [] { return 6L; }, [] { return 6L; }));
  QVERIFY(cache.isValid());
}

void ClipboardReadCacheTests::oversizedFormatIsSkipped()
{
  Clipboard source;
  fill(source, "text", std::string(200, 'h'));
  ClipboardReadCache::Limits limits;
  limits.maxFormatBytes = 100;
  Clipboard dst;
  QVERIFY(ClipboardReadCache::copyBounded(&dst, &source, limits));
  QCOMPARE(textOf(dst), std::string("text"));
  QVERIFY(!hasFormat(dst, IClipboard::Format::HTML));
}

void ClipboardReadCacheTests::overBudgetSendsTextOnly()
{
  SlowClipboard source;
  fill(source, "text", "<b>html</b>");
  ClipboardReadCache::Limits limits;
  limits.budget = std::chrono::milliseconds(1);
  Clipboard dst;
  QVERIFY(ClipboardReadCache::copyBounded(&dst, &source, limits));
  QCOMPARE(textOf(dst), std::string("text"));
  QVERIFY(!hasFormat(dst, IClipboard::Format::HTML));

  // within budget every format is copied
  limits.budget = std::chrono::milliseconds(0);
  Clipboard all;
  QVERIFY(ClipboardReadCache::copyBounded(&all, &source, limits));
  QVERIFY(hasFormat(all, IClipboard::Format::HTML));
}

void ClipboardReadCacheTests::onlyPasteChordsFlush()
{
  using deskflow::isPasteChord;
  QVERIFY(isPasteChord('v', KeyModifierSuper));
  QVERIFY(isPasteChord('v', KeyModifierControl));
  QVERIFY(isPasteChord('V', KeyModifierControl | KeyModifierShift));
  QVERIFY(isPasteChord(kKeyInsert, KeyModifierShift));
  QVERIFY(!isPasteChord('v', 0));
  QVERIFY(!isPasteChord('v', KeyModifierShift));
  QVERIFY(!isPasteChord('a', KeyModifierSuper));
  QVERIFY(!isPasteChord(kKeySuper_L, KeyModifierSuper));
  QVERIFY(!isPasteChord(kKeyShift_L, KeyModifierShift));
  QVERIFY(!isPasteChord(kKeyInsert, 0));
}

QTEST_APPLESS_MAIN(ClipboardReadCacheTests)
#include "ClipboardReadCacheTests.moc"
