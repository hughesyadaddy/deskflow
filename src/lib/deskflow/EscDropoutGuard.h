/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "base/Event.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "common/ExitCodes.h"
#include "coordination/EscDropout.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <thread>

//! Owns the Esc x5 local dropout for one core process (server or client):
//! gate startup on a pending deadline, arm the HID-level observer, and on a
//! burst run the role's teardown on the main loop, then exit non-zero so
//! the supervisor (launchd KeepAlive / Windows daemon) respawns the core,
//! which then waits out the rest of the 60 s before serving/connecting.
class EscDropoutGuard
{
public:
  EscDropoutGuard(IEventQueue *events, const char *role, std::function<void()> teardown)
      : m_events(events),
        m_role(role),
        m_teardown(std::move(teardown))
  {
  }

  ~EscDropoutGuard()
  {
    if (m_monitor) {
      m_monitor->stop();
    }
    m_events->removeHandler(deskflow::EventTypes::LocalEscDropout, this);
  }

  //! blocking: sits out a pending dropout (startup only)
  void gateStartup() const
  {
    deskflow::coordination::waitOutDropoutDeadline(m_role);
  }

  void arm()
  {
    m_events->addHandler(deskflow::EventTypes::LocalEscDropout, this, [this](const Event &) { onMainLoop(); });
    m_monitor = deskflow::coordination::createEscDropoutMonitor();
    m_monitor->start([this] { onBurst(); });
  }

private:
  // observer thread: must work even when the main loop is wedged
  void onBurst()
  {
    if (m_fired.exchange(true)) {
      return;
    }
    deskflow::coordination::writeDropoutDeadline();
    LOG_WARN("[esc-dropout] Esc x5 on this keyboard: %s leaving Deskflow for %d s", m_role,
             deskflow::coordination::kDropoutSeconds);
    m_events->addEvent(Event(deskflow::EventTypes::LocalEscDropout, this));
    // Backstop: a wedged main loop never runs the teardown; leave anyway so
    // nothing of ours keeps capturing/swallowing this keyboard.
    std::thread([] {
      std::this_thread::sleep_for(std::chrono::seconds(4));
      LOG_WARN("[esc-dropout] main loop did not tear down in 4 s; exiting hard");
      std::_Exit(s_exitFailed);
    }).detach();
  }

  void onMainLoop()
  {
    if (m_teardown) {
      m_teardown();
    }
    LOG_INFO("[esc-dropout] %s torn down; core exits, respawns and waits out the deadline", m_role);
    m_events->addEvent(Event(deskflow::EventTypes::Quit, nullptr, new ExitEventData(s_exitFailed)));
  }

  IEventQueue *m_events;
  const char *m_role;
  std::function<void()> m_teardown;
  std::unique_ptr<deskflow::coordination::IEscDropoutMonitor> m_monitor;
  std::atomic<bool> m_fired{false};
};
