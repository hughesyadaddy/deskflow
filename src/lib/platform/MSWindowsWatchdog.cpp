/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2025 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsWatchdog.h"

#include "arch/Arch.h"
#include "arch/win32/XArchWindows.h"
#include "base/Log.h"
#include "base/LogOutputters.h"
#include "base/TMethodJob.h"
#include "common/Constants.h"
#include "common/LogLevel.h"
#include "deskflow/App.h"
#include "mt/Thread.h"
#include "platform/MSWindowsHandle.h"

#include <Shellapi.h>
#include <UserEnv.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Wtsapi32.h>
#include <shlobj.h>
#include <tchar.h>
#include <tlhelp32.h>

#include <QStringDecoder>

#include <algorithm>
#include <iterator>

//
// Free functions
//

HANDLE openProcessForKill(const PROCESSENTRY32 &entry)
{
  // pid 0 is 'system idle process'
  if (entry.th32ProcessID == 0)
    return nullptr;

  if (_wcsicmp(entry.szExeFile, L"deskflow-client.exe") != 0 && //
      _wcsicmp(entry.szExeFile, L"deskflow-server.exe") != 0 && //
      _wcsicmp(entry.szExeFile, L"deskflow-core.exe") != 0) {
    return nullptr;
  }

  const DWORD desiredAccess = PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
  HANDLE handle = OpenProcess(desiredAccess, FALSE, entry.th32ProcessID);
  if (handle == nullptr) {
    LOG_WARN(
        "could not open process handle for kill, pid=%u, error=%s", entry.th32ProcessID,
        windowsErrorToString(GetLastError()).c_str()
    );
    return nullptr;
  }

  // only shut down if not current process (daemon is now the same unified binary).
  if (entry.th32ProcessID == GetCurrentProcessId()) {
    CloseHandle(handle);
    return nullptr;
  }

  return handle;
}

// Thrown when the medium-integrity core cannot launch because no interactive
// user is logged on (login/lock screen). This is an expected wait state, not a
// crash -- the watchdog reschedules quietly instead of crit-logging and backing
// off like a real start failure.
struct NoInteractiveSessionError : std::runtime_error
{
  NoInteractiveSessionError() : std::runtime_error("no interactive user session")
  {
  }
};

//
// MSWindowsWatchdog
//

MSWindowsWatchdog::MSWindowsWatchdog(bool foreground, FileLogOutputter &fileLogOutputter)
    : m_fileLogOutputter(fileLogOutputter),
      m_foreground(foreground)
{
  initSasFunc();
  initOutputReadPipe();
  initJobObject();
}

MSWindowsWatchdog::~MSWindowsWatchdog()
{
  // Closing the last handle to a kill-on-close job terminates every process
  // still in it: any core we spawned dies with the daemon.
  if (m_job != nullptr) {
    CloseHandle(m_job);
    m_job = nullptr;
  }
}

void MSWindowsWatchdog::initJobObject()
{
  m_job = CreateJobObjectW(nullptr, nullptr);
  if (m_job == nullptr) {
    LOG_ERR("could not create core job object, error: %s", windowsErrorToString(GetLastError()).c_str());
    return;
  }

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
  ZeroMemory(&limits, sizeof(limits));
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
    LOG_ERR("could not set kill-on-close on core job object, error: %s", windowsErrorToString(GetLastError()).c_str());
    CloseHandle(m_job);
    m_job = nullptr;
    return;
  }

  LOG_DEBUG("created kill-on-close job object for core processes");
}

void MSWindowsWatchdog::startAsync()
{
  m_mainThread = std::make_unique<Thread>(new TMethodJob(this, &MSWindowsWatchdog::mainLoop, nullptr));
  m_outputThread = std::make_unique<Thread>(new TMethodJob(this, &MSWindowsWatchdog::outputLoop, nullptr));
  m_sasThread = std::make_unique<Thread>(new TMethodJob(this, &MSWindowsWatchdog::sasLoop, nullptr));
}

void MSWindowsWatchdog::stop()
{
  const auto kThreadWaitSeconds = 5;
  // The main loop's last act is MSWindowsProcess::shutdown() on the core:
  // graceful window + post-terminate wait. Waiting for less than that (the
  // old 5 s) returned from stop() with the core still alive and the daemon
  // then exited underneath it.
  const auto kMainThreadWaitSeconds = deskflow::platform::kMaxShutdownSeconds + kThreadWaitSeconds;

  m_running = false;

  if (!m_mainThread->wait(kMainThreadWaitSeconds)) {
    LOG_WARN("could not stop main thread within %ds", kMainThreadWaitSeconds);
  }

  if (!m_outputThread->wait(kThreadWaitSeconds)) {
    LOG_WARN("could not stop output thread");
  }

  if (!m_sasThread->wait(kThreadWaitSeconds)) {
    LOG_WARN("could not stop sas thread");
  }
}

HANDLE
MSWindowsWatchdog::duplicateProcessToken(HANDLE process, LPSECURITY_ATTRIBUTES security)
{
  HANDLE sourceToken;
  if (!OpenProcessToken(process, TOKEN_ASSIGN_PRIMARY | TOKEN_ALL_ACCESS, &sourceToken)) {
    LOG_ERR("could not open token, process handle: %p", process);
    throw std::runtime_error(windowsErrorToString(GetLastError()));
  }

  HANDLE newToken;
  if (!DuplicateTokenEx(
          sourceToken, TOKEN_ASSIGN_PRIMARY | TOKEN_ALL_ACCESS, security, SecurityImpersonation, TokenPrimary, &newToken
      )) {
    CloseHandle(sourceToken);
    LOG_ERR("could not duplicate token");
    throw std::runtime_error(windowsErrorToString(GetLastError()));
  }

  CloseHandle(sourceToken);
  return newToken;
}

HANDLE
MSWindowsWatchdog::getUserToken(LPSECURITY_ATTRIBUTES security, bool elevatedToken)
{
  m_session.updateActiveSession();

  if (elevatedToken) {
    // Login/lock screen: inject on the secure Winlogon desktop by running the
    // core with winlogon.exe's SYSTEM token.
    LOG_DEBUG("getting elevated (winlogon) token for login screen");
    HANDLE process = nullptr;
    if (!m_session.isProcessInSession(L"winlogon.exe", &process) || process == nullptr) {
      throw std::runtime_error("cannot get elevated token without winlogon.exe");
    }
    try {
      HANDLE token = duplicateProcessToken(process, security);
      CloseHandle(process);
      return token;
    } catch (...) {
      CloseHandle(process);
      throw;
    }
  }

  // Probe the active console session first so we can distinguish the benign
  // "no user logged on" case (login/lock screen -> wait and retry) from real
  // token failures (missing SeTcbPrivilege, duplication errors) that must
  // escalate to the crit-log/backoff path instead of retrying forever.
  HANDLE probe = nullptr;
  if (!WTSQueryUserToken(m_session.getActiveSessionId(), &probe)) {
    const DWORD err = GetLastError();
    if (err == ERROR_NO_TOKEN) {
      LOG_DEBUG("no user token in session %d (login/lock screen)", m_session.getActiveSessionId());
      throw NoInteractiveSessionError();
    }
    LOG_ERR("could not query user token from session %d", m_session.getActiveSessionId());
    throw std::runtime_error(windowsErrorToString(err));
  }
  CloseHandle(probe);

  LOG_DEBUG("getting user session token");
  return m_session.getUserToken(security);
}

bool MSWindowsWatchdog::loginScreenActive()
{
  // session-0 daemon can't query session 1's input desktop directly; the
  // lock/login screen is owned by LogonUI.exe. Unlike consent.exe (transient
  // UAC prompt) LogonUI is stable while shown, so keying elevation off it does
  // not cause relaunch churn.
  MSWindowsHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
  if (snapshot.get() == INVALID_HANDLE_VALUE) {
    return false;
  }

  PROCESSENTRY32 entry;
  entry.dwSize = sizeof(PROCESSENTRY32);
  if (!Process32First(snapshot.get(), &entry)) {
    return false;
  }

  do {
    if (_wcsicmp(entry.szExeFile, L"LogonUI.exe") == 0) {
      return true;
    }
  } while (Process32Next(snapshot.get(), &entry));

  return false;
}

void MSWindowsWatchdog::mainLoop(const void *)
{
  using enum ProcessState;

  shutdownExistingProcesses();

  LOG_DEBUG("starting watchdog main loop");
  while (m_running) {
    LOG_VERBOSE("locking process state mutex in watchdog main loop");
    std::unique_lock lock(m_processStateMutex);

    // The core normally runs at medium integrity so user-level input hooks
    // (PowerToys) keep working (see plan 2026-07-07). It is relaunched only for:
    //   1. a console-session change (fast user switch / logon), and
    //   2. the login/lock screen appearing or clearing -- there the core must
    //      run SYSTEM to inject on the secure Winlogon desktop.
    // In-session UAC (consent.exe) is intentionally NOT a trigger: it flickers
    // and would churn the mesh / break PowerToys. LogonUI is stable, so its
    // transitions cost at most one relaunch each (lock and unlock).
    if (m_processState == Running && !m_command.empty() && !m_foreground) {
      if (m_session.hasChanged()) {
        LOG_DEBUG("session changed, queueing process start");
        m_processState = StartPending;
        m_nextStartTime.reset();
      } else if (loginScreenActive() != m_lastElevated) {
        LOG_DEBUG("login-screen transition (active=%s), queueing process start", loginScreenActive() ? "yes" : "no");
        m_processState = StartPending;
        m_nextStartTime.reset();
      }
    }

    switch (m_processState) {
    case Idle:
      LOG_VERBOSE("watchdog process state idle");
      break;

    case StartScheduled: {
      LOG_VERBOSE("watchdog process start scheduled");
      if (m_nextStartTime.has_value() && m_nextStartTime.value() <= Arch::time()) {
        LOG_DEBUG("start time reached, queueing process start");
        m_processState = StartPending;
      }
    } break;

    case StartPending: {
      LOG_DEBUG("watchdog starting new process");
      try {
        startProcess();
        m_startFailures = 0;
        m_awaitingUserSession = false;
        m_processState = Running;
      } catch (const NoInteractiveSessionError &) { // NOSONAR - expected wait state
        m_processState = handleNoInteractiveSession();
      } catch (std::exception &e) { // NOSONAR - Catching all exceptions
        m_processState = handleStartError(e.what());
      } catch (...) { // NOSONAR - Catching remaining exceptions
        m_processState = handleStartError();
      }
    } break;

    case Running: {
      LOG_VERBOSE("watchdog process in running state");
      if (!isProcessRunning()) {
        m_processState = handleProcessExit();
      }
    } break;

    case StopPending: {
      LOG_DEBUG("watchdog stopping current process");
      if (m_process != nullptr) {
        m_process->shutdown();
        m_process.reset();
      } else {
        LOG_WARN("no process to stop");
      }
      shutdownExistingProcesses();
      m_processState = Idle;
    } break;
    }

    LOG_VERBOSE("unlocking process state mutex in watchdog main loop");
    lock.unlock();

    // Sleep for only 100ms rather than 1 second so that the service can shut down faster.
    LOG_VERBOSE("watchdog main loop sleeping");
    Arch::sleep(0.1);
  }

  LOG_DEBUG("watchdog main loop finished");

  if (m_process != nullptr) {
    LOG_DEBUG("terminating running process on exit");
    m_process->shutdown();
    m_process.reset();
  }

  shutdownExistingProcesses();
}

bool MSWindowsWatchdog::isProcessRunning()
{
  if (m_process == nullptr) {
    return false;
  }

  DWORD exitCode;
  GetExitCodeProcess(m_process->info().hProcess, &exitCode);
  return exitCode == STILL_ACTIVE;
}

void MSWindowsWatchdog::startProcess()
{
  if (m_command.empty()) {
    throw std::runtime_error("cannot start process, command is empty");
  }

  if (m_process != nullptr) {
    LOG_DEBUG("closing existing process to make way for new one");
    // Short timeout: on a relaunch (crash or session change) the core is being
    // replaced immediately, so don't wait the full graceful window.
    m_process->shutdown(2);
    m_process.reset();
  }

  m_process = std::make_unique<deskflow::platform::MSWindowsProcess>(m_command, m_outputWritePipe, m_outputWritePipe);

  // The core runs at medium integrity during a normal desktop session so
  // user-level hook tools (PowerToys Keyboard Manager, Mouser) can intercept
  // and remap its injected input. It is elevated ONLY while the login/lock
  // screen (LogonUI) is active, where it must run SYSTEM to inject on the
  // secure Winlogon desktop and where no user-level hooks are running anyway.
  // In-session UAC (consent.exe) is deliberately not elevated for -- that flip
  // churned the mesh and broke PowerToys; the VHID bridge handles it instead.
  const bool elevate = loginScreenActive();
  m_lastElevated = elevate;

  // Grant UIAccess on the normal desktop (not the login screen, which is
  // already SYSTEM). UIAccess raises the core enough to bypass UIPI, so its
  // injected input reaches an elevated foreground window (e.g. elevated
  // PowerToys) instead of being dropped -- while remaining visible to
  // PowerToys' own low-level hook so remaps still apply. Windows only honors
  // the UIAccess bit if the binary is Authenticode-signed and under Program
  // Files; if it isn't, the bit is silently ignored (medium core).
  const bool uiAccess = !elevate && m_uiAccessCore;

  LOG_INFO("running command: %ls", m_command.c_str());
  LOG_INFO("core integrity: %s", elevate ? "SYSTEM (login screen active)" : (uiAccess ? "medium + UIAccess" : "medium"));

  BOOL createRet;
  if (m_foreground) {
    LOG_DEBUG("starting command in foreground");
    createRet = m_process->startInForeground();
  } else {
    LOG_DEBUG("starting new process in user session");

    SECURITY_ATTRIBUTES sa;
    ZeroMemory(&sa, sizeof(SECURITY_ATTRIBUTES));
    HANDLE userToken = getUserToken(&sa, elevate);

    // Request UIAccess on the token. On the login screen the SYSTEM token
    // already drives the secure desktop; on the normal desktop this is what
    // lets the (medium) core reach elevated windows.
    if (elevate || uiAccess) {
      DWORD enable = 1;
      if (!SetTokenInformation(userToken, TokenUIAccess, &enable, sizeof(DWORD))) {
        LOG_WARN(
            "could not set UIAccess on core token (error %s); core may be unable to reach elevated windows",
            windowsErrorToString(GetLastError()).c_str()
        );
      }
    }

    createRet = m_process->startAsUser(userToken, &sa);
  }

  if (!createRet) {
    DWORD exitCode = 0;
    if (GetExitCodeProcess(m_process->info().hProcess, &exitCode)) {
      LOG_ERR("daemon failed to run command, exit code: %d", exitCode);
    } else {
      LOG_ERR("daemon failed to run command, unknown exit code");
      throw std::runtime_error(windowsErrorToString(GetLastError()));
    }
  } else {
    m_processStartTime = Arch::time();

    // Tie the core to this daemon's lifetime (see initJobObject). Done right
    // after creation so there is no window where a daemon crash orphans it.
    if (m_job != nullptr) {
      if (!AssignProcessToJobObject(m_job, m_process->info().hProcess)) {
        LOG_WARN(
            "could not assign core (pid=%d) to job object, error: %s; it will outlive a daemon crash",
            m_process->info().dwProcessId, windowsErrorToString(GetLastError()).c_str()
        );
      }
    }

    // Wait for program to fail. This needs to be 1 second, as the process may take some time to fail.
    LOG_DEBUG("watchdog waiting for process start result");
    Arch::sleep(1);

    if (!isProcessRunning()) {
      // Not a spawn failure: the core ran and chose to exit (typically exit 5,
      // duplicate instance). Leave m_process in place and let the Running
      // state route it through handleProcessExit(), which reads the exit
      // code and applies the restart policy instead of the blind
      // handleStartError() backoff.
      LOG_WARN("core process exited during startup, pid=%d", m_process->info().dwProcessId);
      return;
    }

    LOG_DEBUG("started core process from watchdog");
    LOG_VERBOSE(
        "process info, session=%i, elevated=%s, command: %s", //
        m_session.getActiveSessionId(), elevate ? "yes" : "no", m_command.c_str()
    );
  }
}

void MSWindowsWatchdog::setProcessConfig(const std::string_view &command, bool uiAccessCore)
{
  LOG_VERBOSE("locking process state mutex for watchdog config change");
  std::scoped_lock lock{m_processStateMutex};

  LOG_DEBUG("setting watchdog process config (uiAccessCore=%s)", uiAccessCore ? "yes" : "no");
  m_command = std::wstring(command.begin(), command.end());
  m_uiAccessCore = uiAccessCore;

  // A config change / IPC start is the explicit "try again" that ends a
  // give-up (see handleProcessExit).
  m_consecutiveFastExits = 0;

  if (m_command.empty()) {
    LOG_DEBUG("command cleared, queueing process stop");
    m_processState = ProcessState::StopPending;
  } else {
    LOG_DEBUG("command changed, queueing process start");
    m_processState = ProcessState::StartPending;
    m_nextStartTime.reset();
  }
}

void MSWindowsWatchdog::requestRestart()
{
  LOG_VERBOSE("locking process state mutex for watchdog restart request");
  std::scoped_lock lock{m_processStateMutex};

  if (m_command.empty()) {
    LOG_WARN("keyboard rescue: core restart requested but no watchdog command is configured; ignoring");
    return;
  }
  if (m_processState == ProcessState::StopPending) {
    LOG_WARN("keyboard rescue: core restart requested while a stop is pending; ignoring");
    return;
  }

  LOG_INFO("keyboard rescue: relaunching core via watchdog");
  m_processState = ProcessState::StartPending;
  m_nextStartTime.reset();
}

void MSWindowsWatchdog::requestStopAll(const std::wstring &installRoot)
{
  {
    LOG_VERBOSE("locking process state mutex for watchdog stop-all request");
    std::scoped_lock lock{m_processStateMutex};
    LOG_INFO("[rescue] stop-all: clearing the watchdog command so no core is relaunched");
    m_command.clear();
    m_nextStartTime.reset();
    m_processState = ProcessState::StopPending;
  }
  terminateProcessesUnderRoot(installRoot);
}

void MSWindowsWatchdog::terminateProcessesUnderRoot(const std::wstring &installRoot)
{
  // Prefix match on the full image path, case-insensitive, with a trailing
  // separator so "C:\Deskflow" never matches "C:\DeskflowOther\...".
  std::wstring root = installRoot;
  if (!root.empty() && root.back() != L'\\' && root.back() != L'/') {
    root.push_back(L'\\');
  }
  for (auto &ch : root) {
    if (ch == L'/') {
      ch = L'\\';
    }
  }

  MSWindowsHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
  if (snapshot.get() == INVALID_HANDLE_VALUE) {
    LOG_ERR("[rescue] could not get process snapshot: %s", windowsErrorToString(GetLastError()).c_str());
    return;
  }

  PROCESSENTRY32 entry;
  entry.dwSize = sizeof(PROCESSENTRY32);
  const DWORD self = GetCurrentProcessId();
  for (BOOL more = Process32First(snapshot.get(), &entry); more; more = Process32Next(snapshot.get(), &entry)) {
    if (entry.th32ProcessID == 0 || entry.th32ProcessID == self) {
      continue;
    }
    const bool isCore = _wcsicmp(entry.szExeFile, L"deskflow-core.exe") == 0 ||
                        _wcsicmp(entry.szExeFile, L"deskflow-client.exe") == 0 ||
                        _wcsicmp(entry.szExeFile, L"deskflow-server.exe") == 0;
    const bool isGui = _wcsicmp(entry.szExeFile, L"deskflow.exe") == 0;
    if (!isCore && !isGui) {
      continue;
    }
    const DWORD desiredAccess = PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
    MSWindowsHandle process(OpenProcess(desiredAccess, FALSE, entry.th32ProcessID));
    if (process.get() == nullptr) {
      LOG_WARN(
          "[rescue] could not open pid %lu for termination: %s", entry.th32ProcessID,
          windowsErrorToString(GetLastError()).c_str()
      );
      continue;
    }
    wchar_t image[MAX_PATH * 2] = {};
    DWORD size = static_cast<DWORD>(std::size(image));
    if (!QueryFullProcessImageNameW(process.get(), 0, image, &size)) {
      continue;
    }
    if (size < root.size() || _wcsnicmp(image, root.c_str(), root.size()) != 0) {
      LOG_DEBUG("[rescue] leaving pid %lu alone: %ls is outside the install root", entry.th32ProcessID, image);
      continue;
    }
    LOG_INFO("[rescue] stopping %ls pid %lu (%ls)", entry.szExeFile, entry.th32ProcessID, image);
    if (isCore) {
      // Close event first (ledger releases, graceful exit), terminate after.
      deskflow::platform::MSWindowsProcess::shutdown(process.get(), entry.th32ProcessID, 3);
    } else if (TerminateProcess(process.get(), 0)) {
      WaitForSingleObject(process.get(), 2000);
    }
  }
}

void MSWindowsWatchdog::outputLoop(const void *)
{
  static constexpr DWORD kBufSize = 4096;

  BYTE raw[kBufSize];
  DWORD bytesRead = 0;

  // Warning: Manual decoding while we still use Win32 APIs for process launching and output reading.
  // Using a byte decoder should help to prevent mojibake when we get a partial UTF-8 sequence in a read chunk.
  // In future when we move to Qt process APIs, this can all be simplified.
  QStringDecoder decoder(QStringDecoder::Utf8);

  while (m_running) {
    const BOOL ok = ::ReadFile(m_outputReadPipe, raw, kBufSize, &bytesRead, nullptr);

    if (!ok || bytesRead == 0) {
      const DWORD err = ::GetLastError();
      if (err != NO_ERROR && err != ERROR_NO_DATA) {
        LOG_WARN("could not read from output pipe, error: %s", windowsErrorToString(err).c_str());
      }

      // Retry immediately when nothing to read or we get a transient error like ERROR_NO_DATA (pipe is non-blocking).
      Arch::sleep(0.1);
      continue;
    }

    const QString decoded =
        decoder.decode(QByteArray::fromRawData(reinterpret_cast<const char *>(raw), int(bytesRead)));

    // The file log outputter adds its own newlines, so trim the decoded string to avoid double newlines.
    const auto trimmed = decoded.trimmed();
    m_fileLogOutputter.write(LogLevel::Level::Print, trimmed);

    if (m_foreground) {
      // Doesn't add it's own newlines, so use the original ones from the process output.
      ::OutputDebugString(decoded.toStdWString().c_str());
    }
  }
}

void MSWindowsWatchdog::shutdownExistingProcesses()
{
  LOG_DEBUG("shutting down any existing processes");

  const auto kAllProcesses = 0;

  // first we need to take a snapshot of the running processes
  MSWindowsHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, kAllProcesses));
  if (snapshot.get() == INVALID_HANDLE_VALUE) {
    LOG_ERR("could not get process snapshot");
    throw std::runtime_error(windowsErrorToString(GetLastError()));
  }

  PROCESSENTRY32 entry;
  entry.dwSize = sizeof(PROCESSENTRY32);

  // get the first process, and if we can't do that then it's
  // unlikely we can go any further
  BOOL gotEntry = Process32First(snapshot.get(), &entry);
  if (!gotEntry) {
    LOG_ERR("could not get first process entry");
    throw std::runtime_error(windowsErrorToString(GetLastError()));
  }

  // now just iterate until we can find winlogon.exe pid
  while (gotEntry) {

    if (HANDLE handle = openProcessForKill(entry); handle != nullptr) {
      LOG_DEBUG("shutting down process, name=%s, pid=%d", entry.szExeFile, entry.th32ProcessID);
      deskflow::platform::MSWindowsProcess::shutdown(handle, entry.th32ProcessID);
      CloseHandle(handle);
    }

    // now move on to the next entry (if we're not at the end)
    gotEntry = Process32Next(snapshot.get(), &entry);
    if (!gotEntry) {

      DWORD err = GetLastError();
      if (err != ERROR_NO_MORE_FILES) {

        // only worry about error if it's not the end of the snapshot
        LOG_ERR("could not get next process entry");
        throw std::runtime_error(windowsErrorToString(GetLastError()));
      }
    }
  }
}

MSWindowsWatchdog::ProcessState MSWindowsWatchdog::handleStartError(const std::string_view &message)
{
  m_startFailures++;

  if (!message.empty()) {
    LOG_CRIT("daemon failed to start process, error: %s", message.data());
  } else {
    LOG_CRIT("daemon failed to start process, unknown error");
  }

  // Exponential backoff so a crash loop on one node does not hammer the mesh.
  if (m_startFailures > 1) {
    const int delaySeconds = (std::min)(1 << (m_startFailures - 2), 30);
    m_nextStartTime = Arch::time() + delaySeconds;
    LOG_WARN("start failed %d times, delaying start %ds", m_startFailures, delaySeconds);
    LOG_DEBUG("start delay, seconds=%d, time=%f", delaySeconds, m_nextStartTime.value());
    return ProcessState::StartScheduled;
  }

  LOG_INFO("retrying process start immediately");
  m_nextStartTime.reset();
  return ProcessState::StartPending;
}

MSWindowsWatchdog::ProcessState MSWindowsWatchdog::handleNoInteractiveSession()
{
  // Expected at the login/lock screen: no user is logged on, so there is no
  // user token to launch the medium-integrity core. Wait quietly and retry
  // when a session appears -- do NOT treat this as a crash (no crit log, no
  // failure backoff escalation).
  if (!m_awaitingUserSession) {
    LOG_INFO("no interactive user session yet; deferring core launch until a user logs on");
    m_awaitingUserSession = true;
  } else {
    LOG_DEBUG("still waiting for an interactive user session");
  }
  m_startFailures = 0;
  m_nextStartTime = Arch::time() + 2.0;
  return ProcessState::StartScheduled;
}

MSWindowsWatchdog::ProcessState MSWindowsWatchdog::handleProcessExit()
{
  namespace policy = deskflow::platform::watchdog;

  const DWORD pid = m_process->info().dwProcessId;
  DWORD exitCode = 0;
  if (!GetExitCodeProcess(m_process->info().hProcess, &exitCode)) {
    LOG_WARN("could not read exit code of core pid=%d, error: %s", pid, windowsErrorToString(GetLastError()).c_str());
    exitCode = static_cast<DWORD>(s_exitFailed);
  }
  const auto uptimeMs = static_cast<long long>((Arch::time() - m_processStartTime) * 1000.0);

  // The process object is signaled, so its kernel objects (single-instance
  // mutex) are released; dropping our handle now is safe.
  m_process.reset();

  if (policy::isFastExit(static_cast<int>(exitCode), uptimeMs)) {
    m_consecutiveFastExits++;
  } else {
    m_consecutiveFastExits = 0;
  }

  LOG_WARN(
      "detected core not running, pid=%d, exit code=%d, uptime=%lldms, consecutive fast exits=%d", pid, exitCode,
      uptimeMs, m_consecutiveFastExits
  );

  const int delayMs = policy::nextRestartDelayMs(static_cast<int>(exitCode), m_consecutiveFastExits, uptimeMs);

  if (delayMs > 0) {
    m_nextStartTime = Arch::time() + delayMs / 1000.0;
    if (exitCode == static_cast<DWORD>(s_exitDuplicate)) {
      LOG_WARN("another core owns the machine (exit %d); retrying in %ds", s_exitDuplicate, delayMs / 1000);
    } else {
      LOG_WARN("core exited too soon after launch, delaying restart %dms", delayMs);
    }
    return ProcessState::StartScheduled;
  }

  LOG_INFO("restarting core immediately");
  m_nextStartTime.reset();
  return ProcessState::StartPending;
}

std::string MSWindowsWatchdog::processStateToString(MSWindowsWatchdog::ProcessState state)
{
  switch (state) {
    using enum MSWindowsWatchdog::ProcessState;

  case Idle:
    return "Idle";
  case StartScheduled:
    return "StartScheduled";
  case StartPending:
    return "StartPending";
  case StopPending:
    return "StopPending";
  case Running:
    return "Running";
  }

  return "Unknown";
}

void MSWindowsWatchdog::initOutputReadPipe()
{
  SECURITY_ATTRIBUTES saAttr;
  saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
  saAttr.bInheritHandle = TRUE;
  saAttr.lpSecurityDescriptor = nullptr;

  if (!CreatePipe(&m_outputReadPipe, &m_outputWritePipe, &saAttr, 0)) {
    LOG_ERR("could not create output pipe");
    throw std::runtime_error(windowsErrorToString(GetLastError()));
  }

  // Set the pipe to non-blocking mode, which allows us to stop the output reader thread immediately
  // in order to speed up the shutdown process when the Windows service needs to stop.
  if (DWORD mode = PIPE_NOWAIT; !SetNamedPipeHandleState(m_outputReadPipe, &mode, nullptr, nullptr)) {
    LOG_ERR("could not set pipe to non-blocking mode");
    throw std::runtime_error(windowsErrorToString(GetLastError()));
  }
}

void MSWindowsWatchdog::initSasFunc()
{
  // the SendSAS function is used to send a sas (secure attention sequence) to the
  // winlogon process. this is used to switch to the login screen.
  HINSTANCE sasLib = LoadLibrary(L"sas.dll");
  if (!sasLib) {
    LOG_ERR("could not load sas.dll");
    throw std::runtime_error(windowsErrorToString(GetLastError()));
  }

  LOG_DEBUG("loaded sas.dll, used to simulate ctrl-alt-del");
  m_sendSasFunc = (SendSas)GetProcAddress(sasLib, "SendSAS");
  if (!m_sendSasFunc) {
    LOG_ERR("could not find SendSAS function in sas.dll");
    throw std::runtime_error(windowsErrorToString(GetLastError()));
  }

  LOG_DEBUG("found SendSAS function in sas.dll");
}

void MSWindowsWatchdog::sasLoop(const void *) // NOSONAR - Thread entry point signature
{
  LOG_VERBOSE("watchdog creating sas event");

  if (m_sendSasFunc == nullptr) {
    throw std::runtime_error("SendSAS function not initialized");
  }

  while (m_running) {
    if (m_processState != ProcessState::Running) {
      LOG_VERBOSE("watchdog not running, skipping SendSAS");
      Arch::sleep(1);
      continue;
    }

    // Create a an event so that other processes can tell the daemon to call the `SendSAS` function.
    MSWindowsHandle sendSasEvent(CreateEvent(nullptr, FALSE, FALSE, LPCWSTR(kSendSasEventName)));
    if (sendSasEvent.get() == nullptr) {
      LOG_ERR("could not create SAS event, error: %s", windowsErrorToString(GetLastError()).c_str());
      Arch::sleep(1);
      continue;
    }

    // Wait for the Core client to tell the daemon to call the `SendSAS` function.
    if (WaitForSingleObject(sendSasEvent.get(), 1000) == WAIT_OBJECT_0) {

      // The SoftwareSASGeneration registry key must be set for this to work:
      //   Set-ItemProperty -Path HKLM:Software\Microsoft\Windows\CurrentVersion\Policies\System
      //     -Name SoftwareSASGeneration -Value 1
      LOG_DEBUG("calling SendSAS to simulate ctrl+alt+del");
      m_sendSasFunc(FALSE);
    }
  }
}
