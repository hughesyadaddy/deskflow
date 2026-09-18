/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QTest>

class CoreProcessTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();
  void crash_schedules_exponential_backoff();
  void crash_after_max_retries_stops_with_error();
  void duplicate_exit_stops_without_retry();
  void normal_exit_while_started_retries_after_base_delay();
  void crash_while_stopping_is_not_a_crash();
  void restart_coalesces_within_window();
  void windows_service_forces_service_mode();
  void externally_supervised_core_attaches_via_ipc_and_kickstarts_on_restart();
  void macos_gui_never_spawns_a_core();
  void stop_releases_process_object();

private:
  inline static const QString m_settingsPath = QStringLiteral("tmp/coreprocess-tests");
  inline static const QString m_settingsFile = QStringLiteral("%1/Deskflow.conf").arg(m_settingsPath);
  inline static const QString m_stateFile = QStringLiteral("%1/Deskflow.state").arg(m_settingsPath);
};
