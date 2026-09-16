/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2025 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/win32/AppUtilWindows.h"

#include "arch/Arch.h"
#include "arch/win32/ArchDaemonWindows.h"
#include "arch/win32/ArchMiscWindows.h"
#include "arch/win32/XArchWindows.h"
#include "base/Event.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "base/LogOutputters.h"
#include "common/Constants.h"
#include "deskflow/App.h"
#include "deskflow/DeskflowException.h"
#include "deskflow/Screen.h"
#include "mt/Thread.h"
#include "platform/MSWindowsScreen.h"

#include "base/ThreadJoin.h"

#include <Windows.h>
#include <conio.h>
#include <thread>

AppUtilWindows::AppUtilWindows(IEventQueue *events) : m_events(events), m_exitMode(kExitModeNormal)
{
  if (SetConsoleCtrlHandler((PHANDLER_ROUTINE)consoleHandler, TRUE) == FALSE) {
    throw std::runtime_error(windowsErrorToString(GetLastError()));
  }

  m_eventThread = std::thread(&AppUtilWindows::eventLoop, this); // NOSONAR - No jthread on Windows

  // Waiting for the event loop start prevents race condition in fast fail scenario,
  // where the dtor is called just before the event loop starts.
  LOG_DEBUG("waiting for event thread to start");
  std::unique_lock lock(m_eventThreadStartedMutex);
  m_eventThreadStartedCond.wait(lock, [this] { return m_eventThreadRunning.load(); });
  LOG_DEBUG("event thread started");
}

AppUtilWindows::~AppUtilWindows()
{
  m_eventThreadRunning = false;
  // Must not throw out of this noexcept destructor; see joinNoThrow. In auto
  // mode this util is built and torn down once per role epoch, so a recycled
  // event-thread id would otherwise std::terminate on every switch.
  deskflow::joinNoThrow(m_eventThread);
}

BOOL WINAPI AppUtilWindows::consoleHandler(DWORD)
{
  LOG_INFO("got shutdown signal");
  IEventQueue *events = AppUtil::instance().app().getEvents();
  events->addEvent(Event(EventTypes::Quit));
  return TRUE;
}

static int mainLoopStatic()
{
  return AppUtil::instance().app().mainLoop();
}

int AppUtilWindows::daemonNTMainLoop()
{
  app().initApp();

  return ArchDaemonWindows::runDaemon(mainLoopStatic);
}

void AppUtilWindows::exitApp(int code)
{
  switch (m_exitMode) {

  case kExitModeDaemon:
    ArchDaemonWindows::daemonFailed(code);
    break;

  default:
    throw ExitAppException(code);
  }
}

int daemonNTMainLoopStatic()
{
  return AppUtilWindows::instance().daemonNTMainLoop();
}

int AppUtilWindows::daemonNTStartup()
{
  SystemLogger sysLogger(app().daemonName(), false);
  m_exitMode = kExitModeDaemon;
  return ARCH->daemonize(daemonNTMainLoopStatic);
}

static int daemonNTStartupStatic()
{
  return AppUtilWindows::instance().daemonNTStartup();
}

static int foregroundStartupStatic()
{
  return AppUtil::instance().app().start();
}

int AppUtilWindows::run()
{
  // record window instance for tray icon, etc
  ArchMiscWindows::setInstanceWin32(GetModuleHandle(nullptr));

  MSWindowsScreen::init(ArchMiscWindows::instanceWin32());
  Thread::getCurrentThread().setPriority(-14);

  StartupFunc startup;
  if (ArchMiscWindows::wasLaunchedAsService()) {
    startup = &daemonNTStartupStatic;
  } else {
    startup = &foregroundStartupStatic;
  }

  return app().runInner(startup);
}

AppUtilWindows &AppUtilWindows::instance()
{
  return (AppUtilWindows &)AppUtil::instance();
}

void AppUtilWindows::startNode()
{
  app().startNode();
}

std::vector<std::string> AppUtilWindows::getKeyboardLayoutList()
{
  std::vector<std::string> layoutLangCodes;
  {
    auto uLayouts = GetKeyboardLayoutList(0, nullptr);
    auto lpList = (HKL *)LocalAlloc(LPTR, (uLayouts * sizeof(HKL)));
    uLayouts = GetKeyboardLayoutList(uLayouts, lpList);

    for (int i = 0; i < uLayouts; ++i) {
      std::string code("", 2);
      GetLocaleInfoA(
          MAKELCID(((ULONG_PTR)lpList[i] & 0xffffffff), SORT_DEFAULT), LOCALE_SISO639LANGNAME, &code[0],
          static_cast<int>(code.size())
      );
      layoutLangCodes.push_back(code);
    }

    if (lpList) {
      LocalFree(lpList);
    }
  }
  return layoutLangCodes;
}

std::string AppUtilWindows::getCurrentLanguageCode()
{
  std::string code("", 2);

  auto hklLayout = getCurrentKeyboardLayout();
  if (hklLayout) {
    auto localLayoutID = MAKELCID(LOWORD(hklLayout), SORT_DEFAULT);
    GetLocaleInfoA(localLayoutID, LOCALE_SISO639LANGNAME, &code[0], static_cast<int>(code.size()));
  }

  return code;
}

HKL AppUtilWindows::getCurrentKeyboardLayout() const
{
  HKL layout = nullptr;

  GUITHREADINFO gti = {sizeof(GUITHREADINFO)};
  if (GetGUIThreadInfo(0, &gti) && gti.hwndActive) {
    layout = GetKeyboardLayout(GetWindowThreadProcessId(gti.hwndActive, nullptr));
  } else {
    // No foreground window: routine on the secure desktop (UAC/LogonUI) and
    // during focus transitions. Fall back to the last known good layout so a
    // null HKL never propagates into ActivateKeyboardLayout/getKeyID and
    // degrades key translation exactly when the desktop state is fragile.
    layout = m_lastKeyboardLayout != nullptr ? m_lastKeyboardLayout : GetKeyboardLayout(0);
    LOG_DEBUG("no foreground window; using fallback keyboard layout");
  }

  m_lastKeyboardLayout = layout;
  return layout;
}

void AppUtilWindows::setProcessQuitHandler(ProcessQuitHandler handler)
{
  s_processQuitHandler = std::move(handler);
}

void AppUtilWindows::eventLoop()
{
  // Auto-reset (bManualReset = FALSE): the wait below consumes the signal.
  // With the old manual-reset event nothing ever called ResetEvent, so once
  // the daemon had signaled it the object stayed signaled for as long as any
  // handle was open, and a later epoch/instance opening the same name would
  // see a stale "close" the moment it started waiting.
  HANDLE hCloseEvent = CreateEvent(nullptr, FALSE, FALSE, kCloseEventName);
  if (!hCloseEvent) {
    LOG_CRIT("failed to create event for windows event loop");
    throw std::runtime_error(windowsErrorToString(GetLastError()));
  }

  LOG_DEBUG("windows event loop running");
  {
    std::scoped_lock lock{m_eventThreadStartedMutex};
    m_eventThreadRunning = true;
  }
  m_eventThreadStartedCond.notify_one();

  while (m_eventThreadRunning) {
    // Wait for 100ms at most so that we can stop the loop when the app is closing, if not already stopped.
    DWORD closeEventResult = MsgWaitForMultipleObjects(1, &hCloseEvent, FALSE, 100, QS_ALLINPUT);

    if (closeEventResult == WAIT_OBJECT_0) {
      LOG_INFO("windows event loop received close event (daemon requested process exit)");
      // The close event means "exit the *process*", not "end the current
      // app". In server/client mode the Quit event below does both. In auto
      // mode Quit only ends the current epoch and the coordination loop
      // re-elects and relaunches the role, so the daemon's graceful shutdown
      // always timed out and fell through to TerminateProcess. The process
      // quit handler is the auto-mode loop's own quit path (the same one the
      // IPC stop request uses); fire it first so the epoch that Quit ends is
      // the last one.
      if (s_processQuitHandler) {
        s_processQuitHandler();
      } else {
        // TODO(auto-mode): no handler is installed. deskflow-core.cpp must
        // wire it right after constructing the runner (auto-mode branch):
        //   AppUtilWindows::setProcessQuitHandler([&runner] { runner.requestQuit(); });
        // Until then the daemon's close event only ends the epoch in auto mode.
        LOG_DEBUG("no process quit handler installed; posting quit to the running app only");
      }
      m_events->addEvent(Event(EventTypes::Quit));
      m_eventThreadRunning = false;
    } else if (closeEventResult == WAIT_OBJECT_0 + 1) {
      MSG msg;
      while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
      }
    }
  }

  CloseHandle(hCloseEvent);
  LOG_DEBUG("windows event loop finished");
}
