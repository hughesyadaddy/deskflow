/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "arch/Arch.h"
#include "arch/IArchMultithread.h"
#include "coordination/CoordinationEvents.h"
#include "coordination/FleetState.h"
#include "common/ExitCodes.h"
#include "deskflow/App.h"
#include "net/NetworkAddress.h"
#include "server/Config.h"

#include <functional>
#include <memory>

enum class ServerState
{
  Uninitialized,
  Initializing,
  InitializingToStart,
  Initialized,
  Starting,
  Started
};

class Server;
namespace deskflow {
class Screen;
}
class ClientListener;
class EventQueueTimer;
class ILogOutputter;
class IEventQueue;
class ISocketFactory;

namespace deskflow {
class ServerArgs;
}

class ServerApp : public App
{
  using ServerConfig = deskflow::server::Config;

public:
  explicit ServerApp(IEventQueue *events, const QString &processName = QString());
  //! Tears down whatever startServer()/initServer() built if mainLoop()
  //! did not get to (an exception out of the event loop); the listener,
  //! server, screen and primary client are raw owned pointers.
  ~ServerApp() override;

  //
  // IApp overrides
  //

  void parseArgs() override;
  const char *daemonName() const override;
  void loadConfig() override;
  bool loadConfig(const QString &filename) override;
  deskflow::Screen *createScreen() override;
  int mainLoop() override;
  int runInner(StartupFunc startup) override;
  int start() override;
  void startNode() override;

  //
  // Regular functions
  //

  void reloadConfig();
  void forceReconnect();
  void resetServer();
  void handleClientConnected(const Event &e, ClientListener *listener);
  void closeServer(Server *server);
  void stopRetryTimer();
  void closeClientListener(ClientListener *listen);
  void stopServer();
  void closePrimaryClient(PrimaryClient *primaryClient);
  void closeServerScreen(deskflow::Screen *screen);
  void cleanupServer();
  //! mainLoop() teardown: handlers off, then cleanupServer().
  void shutdownServerNode();
  bool initServer();
  void retryHandler();
  deskflow::Screen *openServerScreen();
  PrimaryClient *openPrimaryClient(const std::string &name, deskflow::Screen *screen);
  void handleSuspend();
  void handleResume();
  ClientListener *openClientListener(const NetworkAddress &address);
  Server *openServer(ServerConfig &config, PrimaryClient *primaryClient);
  bool startServer();
  Server *getServerPtr()
  {
    return m_server;
  }

  void setCursorBroadcastCallback(std::function<void(const std::string &host)> callback)
  {
    m_cursorBroadcastCallback = std::move(callback);
  }

  void setFleetTopologyPublishCallback(
      std::function<
          void(std::vector<deskflow::coordination::FleetLink>, std::vector<deskflow::coordination::FleetScreen>)>
          callback
  )
  {
    m_fleetTopologyPublishCallback = std::move(callback);
  }

  void setFleetSnapshotCallback(std::function<deskflow::coordination::FleetState()> callback)
  {
    m_fleetSnapshotCallback = std::move(callback);
  }

  void setWakePeerCallback(std::function<void(const std::string &name)> callback)
  {
    m_wakePeerCallback = std::move(callback);
  }

  //! Called with true once the client listener is bound and accepting,
  //! and with false when it is torn down (stop, suspend, teardown).
  /*!
  The coordinator's wedge detector keys off this: it must never probe a
  server epoch that has not bound its port yet (display wait, config
  retry) and only counts strikes after the listener existed.
  */
  void setListeningCallback(std::function<void(bool listening)> callback)
  {
    m_listeningCallback = std::move(callback);
  }

  //
  // Static functions
  //

  static void reloadSignalHandler(Arch::ThreadSignal, void *);
  static ServerApp &instance()
  {
    return (ServerApp &)App::instance();
  }

private:
  void handleScreenSwitched(const Event &event);
  void handleWakePeerRequested(const Event &event);
  void publishFleetTopologyFromConfig();
  void applyFleetTopologyFromSnapshot();
  void registerFleetTopologyHandlers();
  void unregisterFleetTopologyHandlers();
  void registerKeyForwardHandler();
  void unregisterKeyForwardHandler();
  void handleCoordinationKeyForward(const Event &event);
  std::unique_ptr<ISocketFactory> getSocketFactory() const;
  NetworkAddress getAddress(const NetworkAddress &address) const;

  bool m_suspended = false;
  Server *m_server = nullptr;
  ServerState m_serverState = ServerState::Uninitialized;
  deskflow::Screen *m_serverScreen = nullptr;
  PrimaryClient *m_primaryClient = nullptr;
  ClientListener *m_listener = nullptr;
  EventQueueTimer *m_timer = nullptr;
  NetworkAddress *m_deskflowAddress = nullptr;
  std::string m_name;
  std::shared_ptr<deskflow::server::Config> m_config;
  std::function<void(const std::string &host)> m_cursorBroadcastCallback;
  std::function<void(std::vector<deskflow::coordination::FleetLink>, std::vector<deskflow::coordination::FleetScreen>)>
      m_fleetTopologyPublishCallback;
  std::function<deskflow::coordination::FleetState()> m_fleetSnapshotCallback;
  std::function<void(const std::string &name)> m_wakePeerCallback;
  std::function<void(bool listening)> m_listeningCallback;
  //! Exit code startNode() reports when startServer() fails: s_exitFailed,
  //! or s_exitAddressInUse when the listen address was already taken so
  //! the epoch loop can back off and eventually hand the port to a fresh
  //! process instead of retrying at 500 ms forever.
  int m_startFailureCode = s_exitFailed;
  bool m_fleetTopologyHandlersRegistered = false;
  bool m_keyForwardHandlerRegistered = false;
};
