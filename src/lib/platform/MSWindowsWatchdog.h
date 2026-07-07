/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2025 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "mt/Thread.h"
#include "platform/MSWindowsProcess.h"
#include "platform/MSWindowsSession.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>

typedef VOID(WINAPI *SendSas)(BOOL asUser);

class FileLogOutputter;

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
  ~MSWindowsWatchdog() = default;

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
  std::optional<double> m_nextStartTime = std::nullopt;
  ProcessState m_processState = ProcessState::Idle;
  std::wstring m_command = {};
  SendSas m_sendSasFunc = nullptr;
  std::mutex m_processStateMutex;
};
