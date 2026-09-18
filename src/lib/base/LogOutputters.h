/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "base/ILogOutputter.h"

#include <QFile>
#include <QString>

#include <mutex>
//! Stop traversing log chain outputter
/*!
This outputter performs no output and returns false from \c write(),
causing the logger to stop traversing the outputter chain.  Insert
this to prevent already inserted outputters from writing.
*/
class StopLogOutputter : public ILogOutputter
{
public:
  StopLogOutputter() = default;
  ~StopLogOutputter() override = default;

  // ILogOutputter overrides
  void open(const QString &title) override;
  void close() override;
  bool write(LogLevel::Level level, const QString &message) override;
};

//! Write log to console
/*!
This outputter writes output to the console.  The level for each
message is ignored.
*/
class ConsoleLogOutputter : public ILogOutputter
{
public:
  ConsoleLogOutputter() = default;
  ~ConsoleLogOutputter() override = default;

  // ILogOutputter overrides
  void open(const QString &title) override;
  void close() override;
  bool write(LogLevel::Level level, const QString &message) override;
  void flush() const;
};

//! Write log to file
/*!
This outputter writes output to the file.  The level for each
message is ignored.  The file is kept open across writes and rotated
(renamed to \c .1 .. \c .N, oldest dropped) once it exceeds \c kSizeLimit.
Thread-safe: the Windows watchdog writes the core's piped output straight
into the daemon's outputter while the daemon logs through \c Log.
*/

class FileLogOutputter : public ILogOutputter
{
public:
  static constexpr qint64 kSizeLimit = 5 * 1024 * 1024;
  static constexpr int kGenerations = 3;

  explicit FileLogOutputter(const QString &logFile);
  ~FileLogOutputter() override = default;

  // ILogOutputter overrides
  void open(const QString &title) override;
  void close() override;
  bool write(LogLevel::Level level, const QString &message) override;

  void setLogFilename(const QString &title);

  //! Path of rotated generation \p generation (1 = newest).
  QString generationName(int generation) const;

private:
  static constexpr int kExistsCheckInterval = 64;
  static constexpr int kRotateRetryInterval = 256;

  bool ensureOpen();
  void rotate();

  std::mutex m_mutex;
  QString m_fileName;
  QFile m_file;
  int m_writesSinceExistsCheck = 0;
  int m_writesUntilRotateRetry = 0;
};

//! Write log to system log
/*!
This outputter writes output to the system log.
*/
class SystemLogOutputter : public ILogOutputter
{
public:
  SystemLogOutputter() = default;
  ~SystemLogOutputter() override = default;

  // ILogOutputter overrides
  void open(const QString &title) override;
  void close() override;
  bool write(LogLevel::Level level, const QString &message) override;
};

//! Write log to system log only
/*!
Creating an object of this type inserts a StopLogOutputter followed
by a SystemLogOutputter into Log.  The destructor removes those
outputters.  Add one of these to any scope that needs to write to
the system log (only) and restore the old outputters when exiting
the scope.
*/
class SystemLogger
{
public:
  SystemLogger(const QString &title, bool blockConsole);
  SystemLogger(SystemLogger const &) = delete;
  SystemLogger(SystemLogger &&) = delete;
  ~SystemLogger();

  SystemLogger &operator=(SystemLogger const &) = delete;
  SystemLogger &operator=(SystemLogger &&) = delete;

private:
  ILogOutputter *m_syslog = nullptr;
  ILogOutputter *m_stop = nullptr;
};
