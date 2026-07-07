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
   * The core always runs at the user's (medium) integrity; the watchdog never
   * elevates it (see plan 2026-07-07). Secure-desktop input is the VHID
   * bridge's responsibility.
   */
  void setProcessConfig(const std::string_view &command);

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
   * @brief Get a security token for the active user session.
   *
   * Throws NoInteractiveSessionError when no user is logged on (login/lock
   * screen), so the caller can treat it as a benign wait state rather than a
   * start failure.
   */
  HANDLE getUserToken(LPSECURITY_ATTRIBUTES security);

  /**
   * @brief Start the core process at the user's (medium) integrity.
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
