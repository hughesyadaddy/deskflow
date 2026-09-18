/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "base/LogOutputters.h"
#include "arch/Arch.h"

#include <iostream>

#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTextStream>

//
// StopLogOutputter
//

void StopLogOutputter::open(const QString &)
{
  // do nothing
}

void StopLogOutputter::close()
{
  // do nothing
}

bool StopLogOutputter::write(LogLevel::Level, const QString &)
{
  return false;
}

//
// ConsoleLogOutputter
//

void ConsoleLogOutputter::open(const QString &title)
{
  // do nothing
}

void ConsoleLogOutputter::close()
{
  // do nothing
}

bool ConsoleLogOutputter::write(LogLevel::Level level, const QString &msg)
{
  using enum LogLevel::Level;
  if ((level >= Fatal) && (level <= Warning))
    std::cerr << qPrintable(msg) << std::endl;
  else
    std::cout << qPrintable(msg) << std::endl;
  std::cout.flush();
  return true;
}

void ConsoleLogOutputter::flush() const
{
  // do nothing
}

//
// SystemLogOutputter
//

void SystemLogOutputter::open(const QString &title)
{
  ARCH->openLog(title);
}

void SystemLogOutputter::close()
{
  ARCH->closeLog();
}

bool SystemLogOutputter::write(LogLevel::Level level, const QString &msg)
{
  ARCH->writeLog(level, msg);
  return true;
}

//
// SystemLogger
//

SystemLogger::SystemLogger(const QString &title, bool blockConsole)
{
  // redirect log messages
  if (blockConsole) {
    m_stop = new StopLogOutputter; // NOSONAR - Adopted by `Log`
    CLOG->insert(m_stop);
  }
  m_syslog = new SystemLogOutputter; // NOSONAR - Adopted by `Log`
  m_syslog->open(title);
  CLOG->insert(m_syslog);
}

SystemLogger::~SystemLogger()
{
  CLOG->remove(m_syslog);
  delete m_syslog;
  if (m_stop != nullptr) {
    CLOG->remove(m_stop);
    delete m_stop;
  }
}

//
// FileLogOutputter
//

FileLogOutputter::FileLogOutputter(const QString &logFile)
{
  setLogFilename(logFile);
}

void FileLogOutputter::setLogFilename(const QString &logFile)
{
  assert(logFile != nullptr);
  m_file.close();
  m_fileName = logFile;
}

QString FileLogOutputter::generationName(int generation) const
{
  return QStringLiteral("%1.%2").arg(m_fileName).arg(generation);
}

bool FileLogOutputter::ensureOpen()
{
  // An external mv/rm (newsyslog, a human) leaves the open handle pointing at
  // the old inode; check the path occasionally instead of stat-ing per line.
  if (m_file.isOpen() && ++m_writesSinceExistsCheck >= kExistsCheckInterval) {
    m_writesSinceExistsCheck = 0;
    if (!QFileInfo::exists(m_fileName)) {
      m_file.close();
    }
  }
  if (m_file.isOpen()) {
    return true;
  }
  m_file.setFileName(m_fileName);
  m_writesSinceExistsCheck = 0;
  return m_file.open(QFile::WriteOnly | QFile::Append);
}

void FileLogOutputter::rotate()
{
  m_file.close();
  QFile::remove(generationName(kGenerations));
  for (int generation = kGenerations - 1; generation >= 1; --generation) {
    QFile::rename(generationName(generation), generationName(generation + 1));
  }
  // The live file is only ever renamed, never removed: if the rename fails
  // (a Windows handle without FILE_SHARE_DELETE) the log keeps growing in
  // place rather than losing what was already written.
  QFile::rename(m_fileName, generationName(1));
}

bool FileLogOutputter::write(LogLevel::Level, const QString &message)
{
  if (!ensureOpen()) {
    return false;
  }

  QTextStream(&m_file) << message << Qt::endl;

  if (m_file.size() > kSizeLimit) {
    rotate();
  }

  return true;
}

void FileLogOutputter::open(const QString &title)
{
  // do nothing
}

void FileLogOutputter::close()
{
  m_file.close();
}
