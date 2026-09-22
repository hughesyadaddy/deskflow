/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 *
 * Mouser link (fork extension): the ONE role-agnostic connector between
 * deskflow-core and the local Mouser instance. Lives for the whole process
 * (owned by AutoModeRunner / the shared() singleton), never by a Server or
 * ServerProxy, so a role flip or a screen switch never tears the session
 * down. See docs/mouser-bridge.md for the wire contract ("proto 2").
 *
 * Two backends, picked per connection attempt:
 *  - lego   : Mouser owns 127.0.0.1:19795 and writes its token file; we
 *             connect, hello with proto 2, keep-alive with pings, and
 *             forward JSON lines both ways.
 *  - legacy : token file absent (Mouser not upgraded). Server role: we own
 *             the old 19796 listener (Deskflow.conf token). Client role: we
 *             connect to Mouser's old remote-device port with the v1 hello.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace deskflow {

class MouserLink
{
public:
  enum class Role
  {
    None,
    Server,
    Client
  };

  enum class Mode
  {
    Off,    //!< no backend engaged (no token file, legacy disabled or unconfigured)
    Lego,   //!< connected/connecting to Mouser's listener with proto 2
    Legacy  //!< compatibility shim: 19796 listener (server) / v1 connector (client)
  };

  struct Options
  {
    int port = 19795;      //!< Mouser's listener (lego contract)
    std::string tokenFile; //!< empty -> platform default (see defaultTokenFile())
    std::string appName = "deskflow-core";
    std::string appVersion;

    std::chrono::milliseconds initialBackoff{1000};
    std::chrono::milliseconds maxBackoff{30000};
    std::chrono::milliseconds pingInterval{2000};
    int pingMisses = 3;
    std::chrono::milliseconds protoMismatchSleep{30000};
    std::chrono::milliseconds authRetry{5000}; //!< `reason:"auth"`: re-read token file, retry
    std::chrono::milliseconds connectTimeout{2000};
    std::chrono::milliseconds helloTimeout{3000};

    //! Fall back to the pre-lego behaviour when the token file is absent.
    //! Snapshotted from Settings on the core thread (the link thread never
    //! touches QSettings).
    bool legacyEnabled = true;
    bool legacyServerEnabled = false; //!< server/mouserBridgeEnabled
    int legacyServerPort = 19796;     //!< server/mouserBridgePort
    std::string legacyServerToken;    //!< server/mouserBridgeToken
    bool legacyClientEnabled = false; //!< client/mouserEnabled
    int legacyClientPort = 19795;     //!< client/mouserPort
    std::string legacyClientToken;    //!< client/mouserToken
  };

  //! Counters exposed for tests and diagnostics (monotonic, thread-safe).
  struct Stats
  {
    std::atomic<int> connects{0};
    std::atomic<int> hellosAccepted{0};
    std::atomic<int> attachesSent{0};
    std::atomic<int> attachRejections{0};
    std::atomic<int> protoMismatches{0};
    std::atomic<int> protoMismatchLogs{0};
    std::atomic<int> authRejections{0};
    std::atomic<int> byesReceived{0};
    std::atomic<int> pingDrops{0};
    std::atomic<int> absentLogs{0};
  };

  //! Lines arriving from the local Mouser (link thread; must not block).
  using InboundHandler = std::function<void(const std::string &line)>;

  explicit MouserLink(Options options);
  MouserLink(const MouserLink &) = delete;
  MouserLink &operator=(const MouserLink &) = delete;
  ~MouserLink();

  //! Process-wide link, created (and started) on first use.
  static MouserLink &shared();

  //! Options for the process-wide link (settings + version).
  static Options optionsFromSettings();

  //! `~/Library/Application Support/Mouser/bridge.token`, `%APPDATA%\Mouser\bridge.token`, ...
  static std::string defaultTokenFile();

  static const char *roleName(Role role);
  static const char *modeName(Mode mode);

  void start();
  //! Send `{"t":"bye","reason":...}` when connected and stop the threads.
  void stop(std::string_view reason = "shutdown");

  //! Role notice: sent now if connected, re-sent after every hello. Same
  //! role twice is a no-op, so App-level and epoch-level callers may both
  //! announce it.
  void setRole(Role role);
  Role role() const;

  //! Focus moved to \p screen (\p here == focus is on this machine).
  //! Remembered and replayed after every hello.
  void notifyFocus(const std::string &screen, bool here);

  //! Queue one Mouser-protocol JSON line for the local Mouser (client role
  //! relay path). Dropped when no session is up, except that the latest
  //! `connect` line is remembered and replayed after the next hello.
  void deliver(const std::string &line);

  //! Queue one binary DFHR HID report frame (non-JSON; dropped when down).
  void deliverReport(const std::string &frame);

  //! Handler for lines from the local Mouser; pass an empty function to clear.
  void setInboundHandler(InboundHandler handler);

  bool connected() const;
  Mode mode() const;
  std::string sessionId() const;
  const Stats &stats() const
  {
    return m_stats;
  }

private:
  struct Outbound
  {
    std::string bytes;
  };

  void linkLoop();
  void writerLoop();

  // lego backend
  bool runLego(const std::string &token);
  int connectLoopback(int port) const;
  enum class Hello
  {
    Accepted,
    ProtoMismatch,
    AuthRejected,
    Failed
  };
  Hello helloLego(int fd, const std::string &token);
  void readLoopLego(int fd, bool &byeReceived);
  void handleLegoInbound(const std::string &line, bool &byeReceived);
  void announceSession();

  // legacy backend
  void runLegacy();
  void runLegacyListener(int port, const std::string &token);
  void runLegacyConnector(int port, const std::string &token);
  bool legacyAuthenticate(int fd, const std::string &token, std::string &carry);
  void readLoopLegacy(int fd, bool synthesizeDisconnectOnClose);
  void deliverLegacy(const std::string &line);

  // shared plumbing
  void publishFd(int fd, Mode mode);
  void dropFd();
  void enqueue(std::string bytes, bool requireConnected = true);
  void enqueueJson(const std::string &line, bool requireConnected = true);
  bool sendNow(int fd, const std::string &bytes) const;
  bool readLine(int fd, std::string &carry, std::string &line, bool &timedOut) const;
  bool sleepInterruptible(std::chrono::milliseconds duration);
  std::string tokenFilePath() const;
  std::string readTokenFile() const;
  void dispatchInbound(const std::string &line);
  void resetBackoff();
  void escalateBackoff();

  Options m_options;
  Stats m_stats;
  const std::string m_sessionId;

  std::thread m_thread;
  std::thread m_writer;
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_started{false};

  // wake-ups for the interruptible sleeps (backoff, proto mismatch, idle)
  mutable std::mutex m_sleepMutex;
  std::condition_variable m_sleepCv;
  std::atomic<unsigned> m_wakeEpoch{0};

  // current session (guarded by m_fdMutex)
  mutable std::mutex m_fdMutex;
  int m_fd = -1;
  std::atomic<Mode> m_mode{Mode::Off};
  std::atomic<bool> m_connected{false};
  std::atomic<int> m_listenFd{-1};

  // outbound queue (guarded by m_queueMutex)
  std::mutex m_queueMutex;
  std::condition_variable m_queueCv;
  std::deque<Outbound> m_queue;

  // remembered state, replayed after every hello (guarded by m_stateMutex)
  mutable std::mutex m_stateMutex;
  Role m_role = Role::None;
  std::string m_focusScreen;
  bool m_focusHere = true;
  bool m_focusKnown = false;
  std::string m_lastConnectLine;
  InboundHandler m_inbound;
  std::atomic<bool> m_legacyAttached{false}; //!< legacy connector: did the old Mouser get a connect?

  std::chrono::milliseconds m_backoff;
  bool m_absentLogged = false;
  bool m_protoLogged = false;
  bool m_legacyLogged = false;
  Mode m_lastLoggedMode = Mode::Off;
};

} // namespace deskflow
