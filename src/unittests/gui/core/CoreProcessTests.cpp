/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "CoreProcessTests.h"

#include "common/Settings.h"
#include "gui/config/ServerConfig.h"
#include "gui/core/CoreProcess.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QSignalSpy>

using namespace deskflow::gui;
using ProcessState = deskflow::core::ProcessState;
using ProcessMode = Settings::ProcessMode;

namespace {

//! CoreProcess with the process boundary faked: nothing is ever spawned, probed or kickstarted.
class FakeCoreProcess : public CoreProcess
{
public:
  explicit FakeCoreProcess(const ServerConfig &config)
      : CoreProcess(config, QStringLiteral("/nonexistent/fake-deskflow-core"))
  {
    setMode(Settings::CoreMode::Client);
  }

  int spawnCount = 0;
  bool supervised = false;
  bool windowsService = false;
  int kickstarts = 0;

  //! Simulate the child process ending, exactly as QProcess::finished would deliver it.
  void finish(int exitCode, QProcess::ExitStatus status)
  {
    onProcessFinished(exitCode, status);
  }

  int processObjects() const
  {
    return static_cast<int>(findChildren<QProcess *>().size());
  }

protected:
  bool spawnCoreProcess(QProcess *, const QString &, const QStringList &) override
  {
    ++spawnCount;
    return true;
  }
  bool hasExternalSupervisor() const override
  {
    return supervised;
  }
  void kickstartExternalCore() override
  {
    ++kickstarts;
  }
  bool isWindowsServiceInstalled() const override
  {
    return windowsService;
  }
};

//! Real platform supervision answer; only the process boundary itself is faked.
class PlatformCoreProcess : public CoreProcess
{
public:
  explicit PlatformCoreProcess(const ServerConfig &config)
      : CoreProcess(config, QStringLiteral("/nonexistent/fake-deskflow-core"))
  {
    setMode(Settings::CoreMode::Client);
  }

  int spawnCount = 0;
  int kickstarts = 0;

protected:
  bool spawnCoreProcess(QProcess *, const QString &, const QStringList &) override
  {
    ++spawnCount;
    return true;
  }
  void kickstartExternalCore() override
  {
    ++kickstarts;
  }
};

void flushDeferredDeletes()
{
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

} // namespace

void CoreProcessTests::initTestCase()
{
  QDir dir;
  QVERIFY(dir.mkpath(m_settingsPath));
  QFile::remove(m_settingsFile);
  Settings::setSettingsFile(m_settingsFile);
  Settings::setStateFile(m_stateFile);
  Settings::setValue(Settings::Core::ProcessMode, ProcessMode::Desktop);
}

void CoreProcessTests::cleanupTestCase()
{
  QFile::remove(m_settingsFile);
  QFile::remove(m_stateFile);
}

void CoreProcessTests::crash_schedules_exponential_backoff()
{
  ServerConfig config;
  FakeCoreProcess core(config);

  core.start(ProcessMode::Desktop);
  QCOMPARE(core.processState(), ProcessState::Started);
  QCOMPARE(core.spawnCount, 1);

  // SIGKILL/SIGTRAP children report exit code 0 + CrashExit on Unix; the old code treated
  // that as a clean exit and re-armed a flat 1 s retry forever.
  const QList<int> expectedDelays = {1000, 2000, 4000, 8000, 16000};
  for (int i = 0; i < expectedDelays.size(); ++i) {
    core.finish(0, QProcess::CrashExit);
    QCOMPARE(core.processState(), ProcessState::RetryPending);
    QCOMPARE(core.consecutiveCrashes(), i + 1);
    QCOMPARE(core.pendingRetryDelayMs(), expectedDelays[i]);

    // Fire the retry now instead of waiting for the timer; a retry keeps the crash count.
    core.start();
    QCOMPARE(core.processState(), ProcessState::Started);
    QCOMPARE(core.pendingRetryDelayMs(), -1);
    QCOMPARE(core.consecutiveCrashes(), i + 1);
  }
  QCOMPARE(core.spawnCount, 1 + expectedDelays.size());

  core.stop();
  QCOMPARE(core.consecutiveCrashes(), 0);
}

void CoreProcessTests::crash_after_max_retries_stops_with_error()
{
  ServerConfig config;
  FakeCoreProcess core(config);
  QSignalSpy errorSpy(&core, &CoreProcess::error);

  core.start(ProcessMode::Desktop);
  for (int i = 0; i < CoreProcess::kMaxCrashRetries; ++i) {
    core.finish(0, QProcess::CrashExit);
    QCOMPARE(core.processState(), ProcessState::RetryPending);
    core.start();
  }
  QCOMPARE(errorSpy.count(), 0);

  // One more crash than we are willing to retry: give up, tell the user, no timer armed.
  core.finish(0, QProcess::CrashExit);
  QCOMPARE(core.processState(), ProcessState::Stopped);
  QCOMPARE(core.pendingRetryDelayMs(), -1);
  QCOMPARE(errorSpy.count(), 1);
  QCOMPARE(errorSpy.first().first().value<CoreProcess::Error>(), CoreProcess::Error::CrashLoop);

  const auto spawnsBefore = core.spawnCount;
  QTest::qWait(1200);
  QCOMPARE(core.spawnCount, spawnsBefore);
  QCOMPARE(core.processState(), ProcessState::Stopped);

  // A user-initiated start is a fresh attempt.
  core.start(ProcessMode::Desktop);
  QCOMPARE(core.consecutiveCrashes(), 0);
  QCOMPARE(core.processState(), ProcessState::Started);
  core.stop();
}

void CoreProcessTests::duplicate_exit_stops_without_retry()
{
  ServerConfig config;
  FakeCoreProcess core(config);
  QSignalSpy stateSpy(&core, &CoreProcess::processStateChanged);

  core.start(ProcessMode::Desktop);
  stateSpy.clear();

  core.finish(5, QProcess::NormalExit); // s_exitDuplicate
  QCOMPARE(core.processState(), ProcessState::Stopped);
  QCOMPARE(core.pendingRetryDelayMs(), -1);
  QCOMPARE(core.consecutiveCrashes(), 0);

  // Never passes through RetryPending (the old code IPC-stopped the other core and re-armed).
  for (const auto &args : stateSpy) {
    QVERIFY(args.first().value<ProcessState>() != ProcessState::RetryPending);
  }

  QTest::qWait(1200);
  QCOMPARE(core.spawnCount, 1);
  QCOMPARE(core.processState(), ProcessState::Stopped);
}

void CoreProcessTests::normal_exit_while_started_retries_after_base_delay()
{
  ServerConfig config;
  FakeCoreProcess core(config);

  core.start(ProcessMode::Desktop);
  core.finish(0, QProcess::NormalExit);
  QCOMPARE(core.processState(), ProcessState::RetryPending);
  QCOMPARE(core.pendingRetryDelayMs(), 1000);
  QCOMPARE(core.consecutiveCrashes(), 0);

  QTRY_COMPARE_WITH_TIMEOUT(core.spawnCount, 2, 3000);
  QCOMPARE(core.processState(), ProcessState::Started);
  core.stop();
}

void CoreProcessTests::crash_while_stopping_is_not_a_crash()
{
  ServerConfig config;
  FakeCoreProcess core(config);

  core.start(ProcessMode::Desktop);
  // The fake never runs a child, so stop() settles synchronously; a late SIGTERM/SIGKILL
  // exit arriving afterwards must not be counted as a crash or re-arm a retry.
  core.stop();
  QCOMPARE(core.processState(), ProcessState::Stopped);
  core.finish(0, QProcess::CrashExit);
  QCOMPARE(core.processState(), ProcessState::Stopped);
  QCOMPARE(core.pendingRetryDelayMs(), -1);
}

void CoreProcessTests::restart_coalesces_within_window()
{
  ServerConfig config;
  FakeCoreProcess core(config);

  core.start(ProcessMode::Desktop);
  QCOMPARE(core.spawnCount, 1);

  core.restart();
  QCOMPARE(core.spawnCount, 2);
  QVERIFY(!core.hasPendingRestart());

  // Burst inside the 5 s window: no extra spawn, exactly one deferred restart.
  core.restart();
  core.restart();
  core.restart();
  QCOMPARE(core.spawnCount, 2);
  QVERIFY(core.hasPendingRestart());
  QCOMPARE(core.processState(), ProcessState::Started);

  core.stop();
}

void CoreProcessTests::windows_service_forces_service_mode()
{
#ifdef Q_OS_WIN
  QSKIP("would send a real start request to an installed Deskflow daemon");
#endif
  ServerConfig config;
  FakeCoreProcess core(config);
  core.windowsService = true;

  Settings::setValue(Settings::Core::ProcessMode, ProcessMode::Desktop);
  Settings::save(false);

  core.start(ProcessMode::Desktop);
  // Service mode: nothing spawned, the daemon is asked instead (we stay Starting until it answers).
  QCOMPARE(core.spawnCount, 0);
  QCOMPARE(core.processObjects(), 0);
  QCOMPARE(core.processState(), ProcessState::Starting);

  // The forced mode must be persisted to the settings file the daemon is handed, otherwise
  // the daemon reads Desktop, refuses to spawn, and the GUI shows Started with no core.
  QCOMPARE(Settings::value(Settings::Core::ProcessMode).value<ProcessMode>(), ProcessMode::Service);
  {
    QSettings onDisk(m_settingsFile, QSettings::IniFormat);
    QCOMPARE(onDisk.value(Settings::Core::ProcessMode).toInt(), static_cast<int>(ProcessMode::Service));
  }

  core.stop(ProcessMode::Service);
  QCOMPARE(core.processState(), ProcessState::Stopped);
  Settings::setValue(Settings::Core::ProcessMode, ProcessMode::Desktop);
  Settings::save(false);
}

void CoreProcessTests::externally_supervised_core_attaches_via_ipc_and_kickstarts_on_restart()
{
  ServerConfig config;
  FakeCoreProcess core(config);
  core.supervised = true;
  QSignalSpy stateSpy(&core, &CoreProcess::processStateChanged);

  core.start(ProcessMode::Desktop);
  QVERIFY(core.isExternallySupervised());
  QCOMPARE(core.processState(), ProcessState::Started);
  QCOMPARE(core.spawnCount, 0);
  QCOMPARE(core.processObjects(), 0);
  stateSpy.clear();

  // Restart is one kickstart: no stop()/start() bounce, so the state never leaves
  // Started and the IPC client (not a respawn) is what re-attaches.
  core.restart();
  QCOMPARE(core.kickstarts, 1);
  QCOMPARE(core.spawnCount, 0);
  QCOMPARE(core.processState(), ProcessState::Started);
  QCOMPARE(stateSpy.count(), 0);
  QCOMPARE(core.connectionState(), deskflow::core::ConnectionState::Connecting);

  core.stop();
  QCOMPARE(core.processState(), ProcessState::Stopped);
  QCOMPARE(core.kickstarts, 1);

  // Restart from Stopped is a plain attach, never a kickstart of a core the user stopped.
  FakeCoreProcess stopped(config);
  stopped.supervised = true;
  stopped.start(ProcessMode::Desktop);
  stopped.stop();
  QCOMPARE(stopped.processState(), ProcessState::Stopped);
  stopped.restart();
  QCOMPARE(stopped.kickstarts, 0);
  QCOMPARE(stopped.spawnCount, 0);
  QCOMPARE(stopped.processState(), ProcessState::Started);
  stopped.stop();
}

void CoreProcessTests::macos_gui_never_spawns_a_core()
{
  ServerConfig config;
  PlatformCoreProcess core(config);

  core.start(ProcessMode::Desktop);
  QCOMPARE(core.processState(), ProcessState::Started);
#ifdef Q_OS_MACOS
  QVERIFY(core.isExternallySupervised());
  QCOMPARE(core.spawnCount, 0);
  core.restart();
  QCOMPARE(core.kickstarts, 1);
  QCOMPARE(core.spawnCount, 0);
#else
  QVERIFY(!core.isExternallySupervised());
  QCOMPARE(core.spawnCount, 1);
  core.restart();
  QCOMPARE(core.kickstarts, 0);
  QCOMPARE(core.spawnCount, 2);
#endif
  core.stop();
  QCOMPARE(core.processState(), ProcessState::Stopped);
}

void CoreProcessTests::stop_releases_process_object()
{
  ServerConfig config;
  FakeCoreProcess core(config);

  for (int i = 0; i < 3; ++i) {
    core.start(ProcessMode::Desktop);
    QCOMPARE(core.processObjects(), 1);
    core.stop();
    flushDeferredDeletes();
    QCOMPARE(core.processObjects(), 0);
  }

  // A crash retry must not leak the crashed QProcess either.
  core.start(ProcessMode::Desktop);
  core.finish(0, QProcess::CrashExit);
  flushDeferredDeletes();
  QCOMPARE(core.processObjects(), 0);
  core.start();
  QCOMPARE(core.processObjects(), 1);
  core.stop();
  flushDeferredDeletes();
  QCOMPARE(core.processObjects(), 0);
}

QTEST_MAIN(CoreProcessTests)
