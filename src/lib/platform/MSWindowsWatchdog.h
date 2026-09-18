/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2025 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "common/ExitCodes.h"
#include "mt/Thread.h"
#include "platform/MSWindowsProcess.h"
#include "platform/MSWindowsSession.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

typedef VOID(WINAPI *SendSas)(BOOL asUser);

class FileLogOutputter;

namespace deskflow::platform::watchdog {

//! Restart policy for a core process that exited on its own (pure, unit-tested).
/*!
The watchdog used to relaunch on *any* exit with no delay, so a core that
died at startup (exit 5: the previous instance still held the single-instance
mutex) was respawned every ~100 ms and the storm choked the machine. These
constants and nextRestartDelayMs() bound that.
*/

//! An exit this soon after launch is a "fast exit" (crash loop candidate).
constexpr long long kFastExitUptimeMs = 2000;
//! Delay before retrying when another core instance owns the machine (exit 5).
constexpr int kDuplicateInstanceDelayMs = 30000;
//! First fast-exit backoff step; doubles per consecutive fast exit.
constexpr int kBaseBackoffMs = 1000;
//! Fast-exit backoff ceiling.
constexpr int kMaxBackoffMs = 30000;

//! Does this exit count toward the consecutive fast-exit counter?
/*!
Exit 5 (duplicate instance) always counts, whatever the uptime: the core
never got to run, so its lifetime says nothing about stability.
*/
constexpr bool isFastExit(int exitCode, long long uptimeMs)
{
  return exitCode == s_exitDuplicate || uptimeMs < kFastExitUptimeMs;
}

//! Milliseconds to wait before relaunching a core that just exited.
/*!
\param exitCode              the core's process exit code
\param consecutiveFastExits  fast exits in a row *including* this one (see isFastExit)
\param uptimeMs              how long this instance ran before exiting
\return 0 = restart now; > 0 = restart after that many ms. The watchdog never
gives up: with no GUI running nothing would ever ask it to try again, and a
core that is down forever is worse than one retried every kMaxBackoffMs.

Rules, in priority order:
 - exit 5: another core owns the machine; wait kDuplicateInstanceDelayMs.
 - uptime < kFastExitUptimeMs: exponential backoff 1 s, 2 s, 4 s ... capped
   at kMaxBackoffMs for as long as the fast exits continue.
 - otherwise: the core ran for a while and died; relaunch immediately.
*/
constexpr int nextRestartDelayMs(int exitCode, int consecutiveFastExits, long long uptimeMs)
{
  if (exitCode == s_exitDuplicate) {
    return kDuplicateInstanceDelayMs;
  }
  if (uptimeMs < kFastExitUptimeMs) {
    const int step = std::clamp(consecutiveFastExits - 1, 0, 20);
    return (std::min)(kBaseBackoffMs << step, kMaxBackoffMs);
  }
  return 0;
}

} // namespace deskflow::platform::watchdog

/**
 * @brief Monitors and (re)starts the core process on Windows at medium integrity.
 *
 * The watchdog relaunches the core only on crash or console-session change. It
 * never elevates the core: doing so broke user-level input hooks (PowerToys)
 * and could not reach the UAC secure desktop anyway (see plan 2026-07-07).
 */
class MSWindowsWatchdog
{
  enum class ProcessState
  {
    Idle,
    StartScheduled,
    StartPending,
    StopPending,
    Running
  };

public:
  explicit MSWindowsWatchdog(bool foreground, FileLogOutputter &fileLogOutputter);
  ~MSWindowsWatchdog();

  /**
   * @brief Start threads for main loop and and output loop.
   */
  void startAsync();

  /**
   * @brief Set the command to run for the core process.
   *
   * @param uiAccessCore when true, grant the core a UIAccess token on the
   * normal desktop so its injected input reaches elevated foreground windows
   * (e.g. an elevated PowerToys) that UIPI would otherwise block for a plain
   * medium-integrity injector. The core is still SYSTEM-elevated only while the
   * login/lock screen is active (see plan 2026-07-07).
   */
  void setProcessConfig(const std::string_view &command, bool uiAccessCore);

  /**
   * @brief Relaunch the core with the current config (keyboard rescue).
   *
   * Used when the 5x Esc rescue reaches a core that has no GUI IPC client to
   * restart it. Queues a start with the existing command; startProcess()
   * shuts the running core down and launches a fresh one. A no-op (logged)
   * when no command is configured, so it can never act as a kill switch.
   */
  void requestRestart();

  /**
   * @brief Stop the main loop and output loop threads.
   */
  void stop();

  /**
   * @return True if the process is running.
   */
  bool isProcessRunning();

private:
  /**
   * @brief Monitor the process state and start/stop the process as necessary.
   */
  void mainLoop(const void *);

  /**
   * @brief Monitor the process standard out/error and write to the file log outputter.
   */
  void outputLoop(const void *);

  /**
   * @brief Duplicate the primary token of another process (e.g. winlogon.exe).
   *
   * Used only for the login/lock screen: to inject on the secure Winlogon
   * desktop the core must run with a SYSTEM token, which we obtain by
   * duplicating winlogon.exe's token.
   */
  HANDLE duplicateProcessToken(HANDLE process, LPSECURITY_ATTRIBUTES security);

  /**
   * @brief Get a security token for launching the core.
   *
   * When @p elevatedToken is true (login/lock screen active) duplicates
   * winlogon.exe's SYSTEM token so the core can inject on the secure desktop.
   * Otherwise returns the interactive user's medium token, throwing
   * NoInteractiveSessionError when no user is logged on yet.
   */
  HANDLE getUserToken(LPSECURITY_ATTRIBUTES security, bool elevatedToken);

  /**
   * @brief True while the login/lock screen (LogonUI.exe) is active.
   *
   * Detected in session 0 by the presence of LogonUI.exe. This is the ONLY
   * case the core is elevated for -- it is stable (unlike the transient
   * consent.exe UAC prompt) so it does not cause the mesh churn / PowerToys
   * breakage that the old consent-driven elevate flip did. In-session UAC is
   * intentionally NOT covered here (that is the VHID bridge's job).
   */
  bool loginScreenActive();

  /**
   * @brief Start the core process (elevated only while the login screen is up).
   */
  void startProcess();

  /**
   * @brief Controls whether the process should restart immediately or delay start.
   */
  ProcessState handleStartError(const std::string_view &message = "");

  /**
   * @brief Reschedule quietly when no interactive user session is available yet.
   */
  ProcessState handleNoInteractiveSession();

  /**
   * @brief Decide what to do after the core exited on its own (not stopped by us).
   *
   * Applies deskflow::platform::watchdog::nextRestartDelayMs to the exit code
   * and uptime of the process that just died.
   */
  ProcessState handleProcessExit();

  /**
   * @brief Create the job object that ties every core we spawn to this daemon's lifetime.
   *
   * JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: when the daemon dies (crash, SCM kill,
   * TerminateProcess) the kernel closes the last job handle and kills the core,
   * so a stopped daemon can never leave an orphaned core holding the
   * single-instance mutex.
   */
  void initJobObject();

  /**
   * @brief Init the output read pipe for standard out/error.
   */
  void initOutputReadPipe();

  /**
   * @brief Init the SendSAS function, used for Ctrl+Alt+Del emulation.
   */
  void initSasFunc();

  /**
   * @brief Allows the SendSAS function to be called from other processes.
   *
   * SendSAS sends a SAS (Secure Attention Sequence) for Ctrl+Alt+Del emulation.
   */
  void sasLoop(const void *);

  /**
   * @brief Convert the process state enum to a string (useful for logging).
   */
  static std::string processStateToString(ProcessState state);

  /**
   * @brief Stops any core processes which were not started by the watchdog.
   */
  static void shutdownExistingProcesses();

private:
  bool m_running = true;
  std::unique_ptr<Thread> m_mainThread;
  std::unique_ptr<Thread> m_outputThread;
  std::unique_ptr<Thread> m_sasThread;
  HANDLE m_outputWritePipe = nullptr;
  HANDLE m_outputReadPipe = nullptr;
  bool m_awaitingUserSession = false; // true while deferring launch for a login/lock screen
  bool m_lastElevated = false;        // integrity the running core was launched at (login-screen elevate)
  bool m_uiAccessCore = false;        // grant UIAccess on the normal desktop (reach elevated windows)
  MSWindowsSession m_session;
  int m_startFailures = 0;
  FileLogOutputter &m_fileLogOutputter;
  bool m_foreground = false;
  std::wstring m_activeDesktop = {};
  std::unique_ptr<deskflow::platform::MSWindowsProcess> m_process;
  HANDLE m_job = nullptr;          // kill-on-close job every spawned core is assigned to
  double m_processStartTime = 0.0; // Arch::time() when the current core was launched
  int m_consecutiveFastExits = 0;  // see deskflow::platform::watchdog::isFastExit
  std::optional<double> m_nextStartTime = std::nullopt;
  ProcessState m_processState = ProcessState::Idle;
  std::wstring m_command = {};
  SendSas m_sendSasFunc = nullptr;
  std::mutex m_processStateMutex;
};
