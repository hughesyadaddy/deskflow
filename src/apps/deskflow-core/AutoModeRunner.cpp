/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "AutoModeRunner.h"

#include "base/EventQueue.h"
#include "base/Log.h"
#include "common/ExitCodes.h"
#include "common/FleetCursor.h"
#include "common/Settings.h"
#include "base/Event.h"
#include "coordination/CoordinationEvents.h"
#include "coordination/Coordinator.h"
#include "coordination/FleetState.h"
#include "coordination/Peer.h"
#include "coordination/PreConnectHosts.h"
#include "deskflow/ClientApp.h"
#include "deskflow/DeskflowException.h"
#include "deskflow/DisplayInvalidException.h"
#include "deskflow/MouserLink.h"
#include "deskflow/ServerApp.h"

#include <QThread>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <thread>

using deskflow::coordination::Coordinator;
using deskflow::coordination::CoordinatorConfig;
using deskflow::coordination::FleetState;
using deskflow::coordination::Role;
using deskflow::coordination::RoleDecision;

namespace {

QStringList settingsPreConnectHosts(const std::string &serverAddress, const std::string &selfName)
{
  const auto peers = deskflow::coordination::parsePeerList(
      Settings::value(Settings::Coordination::Peers).toStringList().join(QLatin1Char(',')).toStdString()
  );
  return deskflow::coordination::defaultPreConnectHosts(serverAddress, selfName, peers);
}

CoordinatorConfig configFromSettings()
{
  CoordinatorConfig config;
  config.selfName = Settings::value(Settings::Core::ComputerName).toString().toStdString();
  config.meshPort = Settings::value(Settings::Coordination::Port).toInt();
  config.deskflowPort = Settings::value(Settings::Core::Port).toInt();
  config.token = Settings::value(Settings::Coordination::Token).toString().toStdString();
  // QSettings turns comma-separated INI values into a QStringList; accept
  // both that and a plain string by normalizing through a list join.
  config.peers = deskflow::coordination::parsePeerList(
      Settings::value(Settings::Coordination::Peers).toStringList().join(QLatin1Char(',')).toStdString()
  );
  const auto followCursor = Settings::value(Settings::Coordination::KeyboardFollowCursor);
  config.keyboardFollowCursor = followCursor.isValid() ? followCursor.toBool() : true;
  return config;
}

} // namespace

EpochFlipGate::Config AutoModeRunner::gateConfigFromEnvironment()
{
  EpochFlipGate::Config config;
  if (const char *raw = std::getenv("DESKFLOW_AUTO_DWELL_MS"); raw != nullptr && *raw != '\0') {
    char *end = nullptr;
    const long ms = std::strtol(raw, &end, 10);
    if (end != raw && *end == '\0' && ms >= 0) {
      config.minDwell = std::chrono::milliseconds(ms);
      // Hysteresis ceiling scales with the base dwell (3x, i.e. 5 s -> 15 s).
      config.maxDwell = std::chrono::milliseconds(ms * kMaxDwellMultiplier);
    } else {
      LOG_WARN("auto mode: ignoring invalid DESKFLOW_AUTO_DWELL_MS=\"%s\"", raw);
    }
  }
  return config;
}

AutoModeRunner::AutoModeRunner(EventQueue &events, QString processName)
    : m_events(events),
      m_processName(std::move(processName)),
      m_gate(gateConfigFromEnvironment())
{
  // do nothing
}

AutoModeRunner::~AutoModeRunner()
{
  // Belt and braces: epochLoop() joins on every exit path, but a runner
  // destroyed before/without running must still not leak the thread.
  stopDeferredThread();
  // Say goodbye to Mouser while logging is still alive (the shared link's
  // own static destructor would otherwise do it silently at exit).
  deskflow::MouserLink::shared().stop("shutdown");
}

void AutoModeRunner::run(QThread &coreThread)
{
  LOG_INFO("starting core in auto (coordinated) mode");
  QObject::connect(&coreThread, &QThread::started, [this, &coreThread] {
    epochLoop();
    coreThread.quit();
  });
  coreThread.start();
}

void AutoModeRunner::requestQuit()
{
  // Set before the coordinator fires the interrupt callback so a quit is
  // never rate-limited like a role flip.
  m_quitRequested = true;
  if (m_coordinator) {
    m_coordinator->requestQuit();
  }
}

void AutoModeRunner::epochLoop()
{
  const auto config = configFromSettings();
  if (config.selfName.empty() || config.peers.empty()) {
    LOG_CRIT(
        "auto mode requires core/computerName and coordination/peers settings "
        "(computerName=\"%s\" peers=\"%s\" from %s)",
        config.selfName.c_str(), qPrintable(Settings::value(Settings::Coordination::Peers).toString()),
        qPrintable(Settings::settingsFile())
    );
    m_exitCode = s_exitFailed;
    return;
  }

  m_coordinator = std::make_unique<Coordinator>(config);
  m_coordinator->setInterruptCallback([this] { onFlipRequested(); });
  if (!m_coordinator->start()) {
    LOG_CRIT("auto mode could not start the coordination mesh");
    m_exitCode = s_exitFailed;
    return;
  }
  m_coordinator->setEventQueue(&m_events);
  m_healthCoordinator = m_coordinator.get();
  m_coordinator->setKeyClearAllHandler([this](const std::string &sender) {
    m_events.addEvent(
        Event(EventTypes::CoordinationKeyClearAll, m_events.getSystemTarget(), new CoordinationKeyClearAllInfo(sender))
    );
  });

  {
    std::scoped_lock lock{m_gateMutex};
    m_gateStop = false;
  }
  m_deferredThread = std::thread([this] { deferredInterruptThread(); });
  LOG_INFO(
      "auto mode: epoch flip dwell %lld ms (max %lld ms)", static_cast<long long>(m_gate.currentDwell().count()),
      static_cast<long long>(gateConfigFromEnvironment().maxDwell.count())
  );

  while (true) {
    const RoleDecision decision = takeDecision();
    if (decision.quit) {
      break;
    }

    const int result = runEpoch(decision.role, decision.serverAddress);
    if (result != s_exitSuccess) {
      LOG_WARN("coordination: %s epoch ended with code %d", roleName(decision.role), result);
      // Pause briefly so a persistent failure cannot hot-loop. Plain
      // std sleep: Arch::sleep() requires an Arch-registered thread and
      // this is a QThread (it crashes in testCancelThread otherwise).
      std::this_thread::sleep_for(kFailureBackoff);
    }
    bool decisionWaiting = false;
    {
      std::scoped_lock lock{m_gateMutex};
      decisionWaiting = m_consumedDecision != nullptr;
    }
    // Only re-arm the same role when the app really exited on its own;
    // an epoch cut by the deferred timer already has its successor.
    if (!decisionWaiting) {
      m_coordinator->notifyEpochEnded();
    }
  }

  stopDeferredThread();
  m_healthCoordinator = nullptr;
  m_coordinator->stop();
  LOG_INFO("auto mode stopped");
}

RoleDecision AutoModeRunner::takeDecision()
{
  {
    std::scoped_lock lock{m_gateMutex};
    if (m_consumedDecision) {
      RoleDecision decision = *m_consumedDecision;
      m_consumedDecision.reset();
      return decision;
    }
  }
  // The timer only consumes while an app runs, and this thread is the
  // one that runs apps, so blocking here cannot race the timer.
  return m_coordinator->awaitRoleDecision();
}

void AutoModeRunner::onFlipRequested()
{
  if (m_quitRequested) {
    if (m_appRunning) {
      m_events.addEvent(Event(EventTypes::Quit));
    }
    return;
  }

  std::scoped_lock lock{m_gateMutex};
  switch (m_gate.requestFlip(EpochFlipGate::Clock::now())) {
  case EpochFlipGate::Action::Ignored:
    // No app running; the loop is (about to be) blocked in takeDecision().
    break;
  case EpochFlipGate::Action::InterruptNow:
    if (consumePendingDecisionLocked()) {
      m_events.addEvent(Event(EventTypes::Quit));
    }
    break;
  case EpochFlipGate::Action::Deferred: {
    const auto wait = m_gate.deferredDeadline().value() - EpochFlipGate::Clock::now();
    LOG_INFO(
        "coordination: role flip requested %lld ms into the epoch; deferring %lld ms (dwell %lld ms)",
        static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(m_gate.currentDwell() - wait).count()
        ),
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(wait).count()),
        static_cast<long long>(m_gate.currentDwell().count())
    );
    m_gateCv.notify_all();
    break;
  }
  case EpochFlipGate::Action::Coalesced:
    LOG_DEBUG("coordination: role flip request coalesced into pending deferred flip");
    break;
  }
}

bool AutoModeRunner::consumePendingDecisionLocked()
{
  if (!m_coordinator->hasPendingDecision()) {
    return false;
  }
  // Non-blocking here: a decision (or quit) is pending and this thread is
  // the only consumer while an app runs (the loop is inside the app).
  RoleDecision decision = m_coordinator->awaitRoleDecision();
  if (keepsRunningEpoch(decision, m_runningRole, m_runningServer)) {
    LOG_INFO(
        "coordination: latest decision is the running %s epoch%s%s; keeping it (no rebuild, %d request(s) coalesced)",
        roleName(decision.role), m_runningServer.empty() ? "" : " towards ", m_runningServer.c_str(),
        m_gate.coalescedRequests()
    );
    return false;
  }
  m_consumedDecision = std::make_unique<RoleDecision>(std::move(decision));
  return true;
}

void AutoModeRunner::deferredInterruptThread()
{
  std::unique_lock lock{m_gateMutex};
  while (!m_gateStop) {
    const auto deadline = m_gate.deferredDeadline();
    if (!deadline) {
      m_gateCv.wait(lock, [this] { return m_gateStop || m_gate.deferredDeadline().has_value(); });
      continue;
    }
    m_gateCv.wait_until(lock, *deadline, [this, deadline] {
      return m_gateStop || m_gate.deferredDeadline() != deadline;
    });
    if (m_gateStop) {
      break;
    }
    if (!m_gate.takeDeferredIfDue(EpochFlipGate::Clock::now())) {
      continue;
    }
    if (m_appRunning && consumePendingDecisionLocked()) {
      LOG_INFO("coordination: dwell elapsed; interrupting the running epoch for the deferred flip");
      m_events.addEvent(Event(EventTypes::Quit));
    }
  }
}

void AutoModeRunner::stopDeferredThread()
{
  {
    std::scoped_lock lock{m_gateMutex};
    m_gateStop = true;
  }
  m_gateCv.notify_all();
  if (m_deferredThread.joinable()) {
    m_deferredThread.join();
  }
}

int AutoModeRunner::runEpoch(Role role, const std::string &serverAddress)
{
  ++m_epochCount;
  const std::string selfName = Settings::value(Settings::Core::ComputerName).toString().toStdString();
  if (role == Role::Client) {
    QStringList hosts = settingsPreConnectHosts(serverAddress, selfName);
    const auto fleet = m_coordinator->fleetSnapshot();
    if (!fleet.links.empty()) {
      hosts = deskflow::coordination::preConnectHostsFromFleet(fleet, serverAddress, selfName);
    }
    Settings::setValue(Settings::Client::RemoteHost, hosts.join(QLatin1Char(',')));
  }

  m_coordinator->updateKeyboardRelayForRole(role);
  // The Mouser link is process-scoped (it must outlive every epoch); the
  // epoch only tells it which role is running.
  deskflow::MouserLink::shared().setRole(
      role == Role::Server ? deskflow::MouserLink::Role::Server : deskflow::MouserLink::Role::Client
  );

  // Screen enter/leave handlers are scoped to this client epoch so stale
  // events from a prior epoch cannot set cursorScreenKnown after reset.
  const bool trackCursorHere = role == Role::Client;
  if (trackCursorHere) {
    m_events.addHandler(EventTypes::CoordinationScreenEntered, m_events.getSystemTarget(), [this](const auto &) {
      m_coordinator->notifyCursorHere(true);
    });
    m_events.addHandler(EventTypes::CoordinationScreenLeft, m_events.getSystemTarget(), [this](const auto &) {
      m_coordinator->notifyCursorHere(false);
    });
  }

  bool trackTopologyReady = false;
  ClientApp *clientAppPtr = nullptr;
  std::function<void(const Event &)> topologyReadyHandler;

  // The App (and with it the platform screen / event tap / client
  // threads) lives exactly as long as this scope; every exit path below
  // releases it through the unique_ptr destructor.
  std::unique_ptr<App> app;
  if (role == Role::Server) {
    auto serverApp = std::make_unique<ServerApp>(&m_events, m_processName);
    serverApp->setCursorBroadcastCallback([this](const std::string &screenName) {
      m_coordinator->updateCursorHost(screenName);
    });
    serverApp->setFleetTopologyPublishCallback([this](auto links, auto screens) {
      m_coordinator->publishFleetTopology(std::move(links), std::move(screens));
    });
    serverApp->setFleetSnapshotCallback([this] { return m_coordinator->fleetSnapshot(); });
    serverApp->setWakePeerCallback([this](const std::string &name) { m_coordinator->wakePeer(name); });
    app = std::move(serverApp);
  } else {
    auto clientApp = std::make_unique<ClientApp>(&m_events, m_processName);
    clientAppPtr = clientApp.get();
    {
      topologyReadyHandler = [this, clientAppPtr, serverAddress, selfName](const Event &) {
        const auto fleet = m_coordinator->fleetSnapshot();
        if (fleet.links.empty()) {
          return;
        }
        clientAppPtr->appendPreConnectHosts(
            deskflow::coordination::preConnectHostsFromFleet(fleet, serverAddress, selfName)
        );
      };
      m_events.addHandler(EventTypes::CoordinationTopologyReady, m_events.getSystemTarget(), topologyReadyHandler);
      trackTopologyReady = true;
    }
    app = std::move(clientApp);
  }
  LOG_INFO(
      "coordination: starting %s epoch%s%s", roleName(role), serverAddress.empty() ? "" : " towards ",
      serverAddress.c_str()
  );

  {
    std::scoped_lock lock{m_gateMutex};
    m_runningRole = role;
    m_runningServer = serverAddress;
    m_gate.epochStarted(EpochFlipGate::Clock::now());
    m_appRunning = true;
    // Published before the app's loop starts, so the coordinator's relay
    // reconciler and key forwarding follow the app that actually runs.
    m_coordinator->setRunningRole(role);
    // A decision can land in the gap before this epoch's loop starts. It
    // was made while no epoch ran, so it was never rate-limited: consume
    // it now rather than deferring a whole dwell (a same-role decision is
    // still dropped by consumePendingDecisionLocked, so no hot loop).
    if (m_coordinator->hasPendingDecision() && consumePendingDecisionLocked()) {
      LOG_INFO("coordination: decision arrived while the %s epoch was being built; interrupting it", roleName(role));
      m_events.addEvent(Event(EventTypes::Quit));
    }
  }

  int result = s_exitFailed;
  try {
    result = app->runSynchronously();
  } catch (ExitAppException &e) {
    result = e.getCode();
  } catch (DisplayInvalidException &die) {
    LOG_CRIT("a display invalid exception error occurred: %s\n", die.what());
    std::this_thread::sleep_for(std::chrono::seconds(10));
  } catch (std::runtime_error &re) {
    LOG_CRIT("a runtime error occurred: %s\n", re.what());
  } catch (std::exception &e) {
    LOG_CRIT("an error occurred: %s\n", e.what());
  } catch (...) {
    LOG_CRIT("an unknown error occurred\n");
  }
  {
    std::scoped_lock lock{m_gateMutex};
    m_appRunning = false;
    m_gate.epochEnded(EpochFlipGate::Clock::now());
    m_runningRole = Role::Init;
    m_runningServer.clear();
    m_coordinator->setRunningRole(Role::Init);
  }
  m_gateCv.notify_all();

  if (trackCursorHere) {
    m_events.removeHandler(EventTypes::CoordinationScreenEntered, m_events.getSystemTarget());
    m_events.removeHandler(EventTypes::CoordinationScreenLeft, m_events.getSystemTarget());
  }
  if (trackTopologyReady) {
    m_events.removeHandler(EventTypes::CoordinationTopologyReady, m_events.getSystemTarget());
  }

  m_coordinator->updateKeyboardRelayForRole(Role::Init);
  deskflow::MouserLink::shared().setRole(deskflow::MouserLink::Role::None);

  m_exitCode = result;
  return result;
}
