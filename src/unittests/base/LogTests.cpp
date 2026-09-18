/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2024 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "LogTests.h"
#include "base/LogOutputters.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <clocale>
#include <iostream>
#include <sstream>
#include <thread>

#define LEVEL_PRINT "%z\057"
#define LEVEL_ERR "%z\061"
#define LEVEL_INFO "%z\063"
#define LEVEL_DEBUG "%z\064"
#define LEVEL_VERBOSE "%z\065"

QString sanitizeBuffer(const std::stringstream &in)
{
  static QRegularExpression timestampRegex("\\[\\S+\\] ");
  QString rtn = QString::fromStdString(in.str()).simplified();
  rtn.remove(timestampRegex);
  return rtn;
}

void LogTests::initTestCase()
{
  std::setlocale(LC_NUMERIC, "C");
  m_log.setFilter(LogLevel::Level::Debug);
}

void LogTests::printWithErrorValidOutput()
{
  std::stringstream buffer;
  std::streambuf *old = std::cerr.rdbuf(buffer.rdbuf());

  m_log.print(nullptr, 0, LEVEL_ERR "test message");

  auto string = sanitizeBuffer(buffer);
  std::cerr.rdbuf(old);

  QCOMPARE(string, "ERROR: test message");
}

void LogTests::printTestPrintLevel()
{
  std::stringstream buffer;
  std::streambuf *old = std::cout.rdbuf(buffer.rdbuf());

  m_log.print(nullptr, 0, LEVEL_PRINT "test message");

  auto string = sanitizeBuffer(buffer);
  std::cout.rdbuf(old);

  QCOMPARE(string, "test message");
}

void LogTests::printTestWithArgs()
{
  std::stringstream buffer;
  std::streambuf *old = std::cout.rdbuf(buffer.rdbuf());

  m_log.print(nullptr, 0, LEVEL_INFO "test %s", "IamARG");

  auto string = sanitizeBuffer(buffer);
  std::cout.rdbuf(old);

  QCOMPARE(string, "INFO: test IamARG");
}

void LogTests::printTestLogString()
{
  std::stringstream buffer;
  std::streambuf *old = std::cout.rdbuf(buffer.rdbuf());

  auto longString = QString(10000, 'a');
  m_log.print(nullptr, 0, LEVEL_INFO "%s", qPrintable(longString));

  auto string = sanitizeBuffer(buffer);
  std::cout.rdbuf(old);

  QCOMPARE(string, QString("INFO: %1").arg(longString));
}

void LogTests::printLevelToHigh()
{
  std::stringstream buffer;
  std::streambuf *old = std::cout.rdbuf(buffer.rdbuf());

  m_log.print(CLOG_VERBOSE "test message");

  auto string = sanitizeBuffer(buffer);
  std::cout.rdbuf(old);

  QCOMPARE(string, QString{});
}

void LogTests::printInfoWithFileAndLine()
{
  std::stringstream buffer;
  std::streambuf *old = std::cout.rdbuf(buffer.rdbuf());

  m_log.print("test file", 123, LEVEL_INFO "test message");

  auto string = sanitizeBuffer(buffer);
  std::cout.rdbuf(old);

  QCOMPARE(string, "INFO: test message test file:123");
}

void LogTests::printErrWithFileAndLine()
{
  std::stringstream buffer;
  std::streambuf *old = std::cerr.rdbuf(buffer.rdbuf());

  m_log.print("test file", 123, LEVEL_ERR "test message");

  auto string = sanitizeBuffer(buffer);
  std::cerr.rdbuf(old);

  QCOMPARE(string, "ERROR: test message test file:123");
}

void LogTests::debugCategoryPassesInfoFilter()
{
  m_log.setFilter(LogLevel::Level::Info);
  m_log.setDebugCategories({QStringLiteral(" coordination "), QString{}});

  std::stringstream buffer;
  std::streambuf *old = std::cout.rdbuf(buffer.rdbuf());
  m_log.print(nullptr, 0, LEVEL_DEBUG "coordination: traced %d", 1);
  m_log.print(nullptr, 0, LEVEL_DEBUG "coordinationX: not the category");
  m_log.print(nullptr, 0, LEVEL_DEBUG "server: still filtered");
  m_log.print(nullptr, 0, LEVEL_VERBOSE "coordination: verbose stays filtered");
  auto string = sanitizeBuffer(buffer);
  std::cout.rdbuf(old);
  QCOMPARE(string, "DEBUG: coordination: traced 1");

  m_log.setDebugCategories({});
  std::stringstream cleared;
  old = std::cout.rdbuf(cleared.rdbuf());
  m_log.print(nullptr, 0, LEVEL_DEBUG "coordination: gone again");
  string = sanitizeBuffer(cleared);
  std::cout.rdbuf(old);
  QCOMPARE(string, QString{});

  m_log.setFilter(LogLevel::Level::Debug);
}

static int lineCount(const QString &path)
{
  QFile file(path);
  if (!file.open(QFile::ReadOnly)) {
    return -1;
  }
  return static_cast<int>(file.readAll().count('\n'));
}

void LogTests::fileOutputterRotatesKeepingGenerations()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const auto path = dir.filePath(QStringLiteral("core.log"));
  FileLogOutputter out(path);

  const QString line(1023, QLatin1Char('x'));
  const int linesPerGeneration = static_cast<int>(FileLogOutputter::kSizeLimit / 1024) + 1;
  int written = 0;
  // Live file only: nothing rotated until the limit is crossed.
  for (int i = 0; i < linesPerGeneration - 1; ++i) {
    QVERIFY(out.write(LogLevel::Level::Info, line));
    ++written;
  }
  QVERIFY(!QFile::exists(out.generationName(1)));
  QCOMPARE(lineCount(path), written);

  QVERIFY(out.write(LogLevel::Level::Info, line));
  ++written;
  QVERIFY(QFile::exists(out.generationName(1)));
  QCOMPARE(lineCount(out.generationName(1)), written);
  QVERIFY(!QFile::exists(path) || lineCount(path) == 0);

  // Every line written after the rotate lands in the fresh live file.
  QVERIFY(out.write(LogLevel::Level::Info, QStringLiteral("after rotate")));
  QCOMPARE(lineCount(path), 1);

  for (int generation = 2; generation <= FileLogOutputter::kGenerations + 1; ++generation) {
    for (int i = 0; i < linesPerGeneration; ++i) {
      QVERIFY(out.write(LogLevel::Level::Info, line));
    }
  }
  for (int generation = 1; generation <= FileLogOutputter::kGenerations; ++generation) {
    QVERIFY2(QFile::exists(out.generationName(generation)), qPrintable(out.generationName(generation)));
  }
  QVERIFY(!QFile::exists(out.generationName(FileLogOutputter::kGenerations + 1)));
  // Generations shifted without loss: the oldest kept file (.N) is the one
  // that started with the marker written right after the first rotate.
  QFile oldest(out.generationName(FileLogOutputter::kGenerations));
  QVERIFY(oldest.open(QFile::ReadOnly));
  QCOMPARE(QString::fromUtf8(oldest.readLine()).trimmed(), QStringLiteral("after rotate"));
}

void LogTests::fileOutputterReopensAfterExternalRemove()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const auto path = dir.filePath(QStringLiteral("core.log"));
  FileLogOutputter out(path);

  QVERIFY(out.write(LogLevel::Level::Info, QStringLiteral("before")));
  QVERIFY(QFile::remove(path));
  for (int i = 0; i < 200; ++i) {
    QVERIFY(out.write(LogLevel::Level::Info, QStringLiteral("line")));
  }
  QVERIFY(QFile::exists(path));
  QVERIFY(lineCount(path) > 0);
  QVERIFY(lineCount(path) < 200);
}

void LogTests::fileOutputterReopensAfterExternalRecreate()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const auto path = dir.filePath(QStringLiteral("core.log"));
  FileLogOutputter out(path);

  const QString line(200, QLatin1Char('y'));
  for (int i = 0; i < 10; ++i) {
    QVERIFY(out.write(LogLevel::Level::Info, line));
  }
  // newsyslog-style: rename the live file away and create an empty one
  QVERIFY(QFile::rename(path, dir.filePath(QStringLiteral("core.log.0"))));
  {
    QFile fresh(path);
    QVERIFY(fresh.open(QFile::WriteOnly));
  }
  for (int i = 0; i < 200; ++i) {
    QVERIFY(out.write(LogLevel::Level::Info, QStringLiteral("after")));
  }
  QVERIFY(lineCount(path) > 0);
  QVERIFY(lineCount(path) < 200);
}

void LogTests::fileOutputterKeepsGenerationsWhenLiveRenameFails()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  // A directory the process cannot rename out of: the live rename fails,
  // so the existing generations must be left exactly as they were.
  const auto sub = dir.filePath(QStringLiteral("locked"));
  QVERIFY(QDir().mkpath(sub));
  const auto path = QStringLiteral("%1/core.log").arg(sub);
  FileLogOutputter out(path);
  {
    QFile gen1(out.generationName(1));
    QVERIFY(gen1.open(QFile::WriteOnly));
    gen1.write("older generation\n");
  }

  const QString line(1023, QLatin1Char('x'));
  const int linesToLimit = static_cast<int>(FileLogOutputter::kSizeLimit / 1024) + 1;
  for (int i = 0; i < linesToLimit - 1; ++i) {
    QVERIFY(out.write(LogLevel::Level::Info, line));
  }
  out.close();
  QVERIFY(QFile::setPermissions(sub, QFile::ReadOwner | QFile::ExeOwner));
  const bool wrote = out.write(LogLevel::Level::Info, line);
  QVERIFY(QFile::setPermissions(sub, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
  QVERIFY(wrote);
  QVERIFY(QFile::exists(path));
  QVERIFY(lineCount(path) >= linesToLimit);
  QFile gen1(out.generationName(1));
  QVERIFY(gen1.open(QFile::ReadOnly));
  QCOMPARE(QString::fromUtf8(gen1.readAll()), QStringLiteral("older generation\n"));
  QVERIFY(!QFile::exists(out.generationName(2)));
  QVERIFY(!QFile::exists(QStringLiteral("%1.rotating").arg(path)));
}

void LogTests::fileOutputterSerialisesConcurrentWriters()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const auto path = dir.filePath(QStringLiteral("daemon.log"));
  FileLogOutputter out(path);

  constexpr int kThreads = 4;
  constexpr int kLines = 2000;
  std::vector<std::thread> writers;
  for (int t = 0; t < kThreads; ++t) {
    writers.emplace_back([&out, t] {
      const QString line = QStringLiteral("writer%1 ").arg(t) + QString(120, QLatin1Char('a' + t));
      for (int i = 0; i < kLines; ++i) {
        out.write(LogLevel::Level::Info, line);
      }
    });
  }
  for (auto &w : writers) {
    w.join();
  }
  out.close();

  QFile file(path);
  QVERIFY(file.open(QFile::ReadOnly));
  int lines = 0;
  while (!file.atEnd()) {
    const auto raw = QString::fromUtf8(file.readLine()).trimmed();
    QVERIFY2(raw.startsWith(QStringLiteral("writer")) && raw.size() == 8 + 120, qPrintable(raw.left(40)));
    ++lines;
  }
  QCOMPARE(lines, kThreads * kLines);
}

QTEST_MAIN(LogTests)
