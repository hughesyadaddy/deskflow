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

QTEST_MAIN(LogTests)
