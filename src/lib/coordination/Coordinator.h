/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "coordination/CoordinationMesh.h"
#include "coordination/ElectionState.h"
#include "coordination/FleetState.h"
#include "coordination/FleetStateMerge.h"
#include "coordination/KeyboardRelayMonitor.h"
#include "coordination/KeyboardRescue.h"
#include "coordination/LocalInputMonitor.h"
#include "coordination/Peer.h"
#include "coordination/PeerAddressAllowlist.h"
#include "coordination/WedgeDetector.h"
#include "deskflow/KeyTypes.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

class IEventQueue;

class CoordinatorFleetPublishTests;
class CoordinatorTests;
class HealthReportTests;

namespace deskflow::coordination {

//! Mesh protocol version spoken by this build. Peers announcing a lower
//! version in their hello are rejected; there is no v1 compatibility mode.
inline constexpr int kMeshProtocolVersion = 2;

//! Grace for the core event loop to acknowledge an off-loop 5x Esc restart
//! before the process hard-exits for a supervisor relaunch.
inline constexpr std::chrono::milliseconds kRescueAckTimeout{3000};
//! Two counters (off-loop monitor, on-loop Server) see the same taps: a
//! second fleet rescue request inside this window is a duplicate.
inline constexpr double kFleetRescueDedupeS = 5.0;

//! Coordinator configuration (from Settings; see design.md).
struct CoordinatorConfig
{
  std::string selfName;
  int meshPort = 24851;
  int deskflowPort = 24800;
  //! `core/interface`: what the server binds (empty = IPv4 any). The wedge
  //! probe targets the loopback of the same family/address.
  std::string deskflowInterface;
  std::string token;
  PeerList peers;
  ElectionTuning tuning;
  bool keyboardFollowCursor = true;
};

//! What the epoch loop should run next.
struct RoleDecision
{
  Role role = Role::Init;
  std::string serverAddress; // when role == Client
  bool quit = false;
  //! Rebuild the app even when role/address match the running epoch
  //! (server transport wedged; the epoch loop must not "keep" it).
  bool restart = false;
};

//! Orchestrates election, mesh, input detection, and the reconciler.
/*!
Owns its threads and outlives the per-role App epochs. The epoch loop in
deskflow-core blocks in awaitRoleDecision(); when the election decides a
flip, the coordinator records the new decision, wakes the epoch loop, and
interrupts the currently running app via the registered callback.
*/
class Coordinator
{
  friend class ::CoordinatorFleetPublishTests;
  friend class ::CoordinatorTests;
  friend class ::HealthReportTests;

public:
  explicit Coordinator(CoordinatorConfig config);
  Coordinator(const Coordinator &) = delete;
  Coordinator &operator=(const Coordinator &) = delete;
  ~Coordinator();

  bool start();
  void stop();

  //! Block until a (new) role decision or quit is available.
  RoleDecision awaitRoleDecision();

  //! Register how to interrupt the currently running app epoch.
  /*!
  Called (from coordinator threads) whenever a decision is made while an
  epoch runs; typically posts EventTypes::Quit to the app event queue.
  */
  void setInterruptCallback(std::function<void()> interrupt);

  //! The running client observed the shared cursor entering/leaving us.
  void notifyCursorHere(bool here);

  //! The running epoch ended on its own (app error); re-arm the same role.
  void notifyEpochEnded();

  //! Request a graceful process shutdown.
  void requestQuit();

  //! True when a decision (or quit) is waiting to be consumed.
  /*!
  The epoch loop re-checks this right after starting an app so a decision
  made in the gap between awaitRoleDecision() and the app's event loop
  cannot be missed.
  */
  bool hasPendingDecision();

  //! Main-thread event queue for cross-thread coordination events.
  void setEventQueue(IEventQueue *events);

  //! Role of the app epoch that is actually running (Init between epochs).
  /*!
  The election role (m_election) flips the moment decide() runs, but the
  app follows only when the epoch loop's dwell gate lets it (up to the
  dwell later). Anything that must agree with the *running* app -- the
  keyboard relay reconciler, key forwarding -- keys off this instead, so
  an outgoing server never starts a relay while its ServerApp still owns
  the keyboard. AutoModeRunner sets it around each epoch's event loop.
  */
  void setRunningRole(Role role);
  Role runningRole() const;

  //! Server epoch: its client listener is bound and accepting (true) or
  //! gone (false). Gates the wedge probe; see WedgeDetector.
  void notifyServerListening(bool listening);

  //! Server epoch: update cursor host/screen in fleet state.
  //! \p screenName is the active screen name (deskflow screen names identify cursor host).
  void updateCursorHost(const std::string &screenName);

  //! Server epoch (mesh v2): publish screen topology to fleet peers.
  void publishFleetTopology(std::vector<FleetLink> links, std::vector<FleetScreen> screens);

  //! Server epoch: fire the wake action for a configured peer (asleep).
  /*!
  Sends a Wake-on-LAN magic packet when the peer has a \c mac and spawns
  its \c wakeCommand (detached) when set. Rate-limited to one wake per
  peer per 30 seconds; no-op for clients and peers without wake hints.
  */
  void wakePeer(const std::string &name);

  //! Start/stop the keyboard relay monitor for the current role epoch.
  void updateKeyboardRelayForRole(Role role);

  //! Mutex-guarded copy of the merged fleet snapshot (mesh v2).
  FleetState fleetSnapshot() const;

  //! Counters behind the periodic `health:` line (deskflow-core HealthReport).
  struct HealthStats
  {
    int peersReachable = 0;
    int peersTotal = 0;
    int links = 0;
    uint64_t meshRx = 0;
    uint64_t meshDup = 0;
    int flipsLastHour = 0;
    int rescuesLastHour = 0;
    bool relayRunning = false;
  };
  HealthStats healthStats() const;

  //! Fleet-wide keyboard rescue: restart the local core AND tell every peer
  //! to restart theirs. The 5-Esc gesture means "the fleet's input is
  //! wedged" -- restarting only the machine that happened to see the taps
  //! left the actually-broken peer stuck.
  void requestFleetRescue();

  //! Fleet-wide stop-all (10x Esc): tell every peer to stop every Deskflow
  //! instance and service on its seat, then do the same here. Once per
  //! process: a seat that is already stopping ignores repeats.
  void requestFleetStopAll();

  //! Receiver side of the KeyClearAll boundary resync (mesh v2).
  /*!
  A peer that forwarded keys to this machine lost the ability to release
  them (its lane to us failed, its relay stopped, a rescue fired) and asks
  us to release every key we hold on its behalf. The handler receives the
  sender's peer name and must release the keys THAT sender relayed (Server:
  its ledger for the sender; Client: the client screen). Invoked on a MESH
  HANDLER THREAD after the same gating as relayed keys (running role, known
  peer): the handler must marshal onto the app event loop itself (post an
  event), never touch screens directly. Wired by deskflow-core next to the
  CoordinationKeyForward handler.
  */
  void setKeyClearAllHandler(std::function<void(const std::string &sender)> handler);

private:
  //! A lane failed while it was believed reachable: keys forwarded on it
  //! may be held on the peer with their Up now undeliverable. Re-labels
  //! every forwarded hold Local (its Up goes to the local OS, a no-op) and
  //! posts a sticky KeyClearAll so the peer releases them once it answers.
  void onPeerLaneFailed(const std::string &peerName);
  //! Relay stop: post Ups for \p buttons still held on the key destination
  //! (regular class: kept across backoff; a late Up is idempotent).
  void postForwardedReleases(const std::vector<KeyButton> &buttons);
  void onMessage(const Message &message, const std::function<void(const std::string &)> &reply);
  void onGenuineInput();
  void handleHelloMessage(const Message &message, const std::function<void(const std::string &)> &reply);
  void handleFleetMessage(const Message &message);
  void postFleetStateEvents(IEventQueue *events, const FleetMergeResult &merge);
  std::vector<FleetPeer> buildFleetPeersLocked();
  //! Queue \p line on every peer's outbox (never blocks; see PeerOutbox).
  void sendLineToPeers(const std::string &line);
  PeerOutbox *outboxByName(const std::string &name) const;
  //! Resolve a fleet host name (peer, server, or cursor screen) to its outbox.
  PeerOutbox *outboxForHostLocked(const std::string &hostName) const;
  bool mergeAndBroadcastFleetFragment(const FleetFragment &fragment, bool sendEvenIfUnchanged);
  void handleKeyForwardMessage(const Message &message);
  void handleKeyClearAllMessage(const Message &message);
  //! Whether a relayed key from \p message may be injected here (running
  //! role owns the keyboard, sender is a configured peer).
  bool acceptsRelayedKeys(const Message &message) const;
  KeyForwardResult
  sendKeyForward(Message::KeyPhase phase, KeyID id, KeyModifierMask mask, KeyButton button, const std::string &lang);
  void requestLocalCoreRestart();
  //! Stop this seat (m_localStopAllHook in tests, else the process-wide
  //! executor); guarded so it runs at most once per process. The guard is
  //! released when no executor could be started, so a later burst retries.
  void requestLocalStopAll();
  bool runLocalStopAll();
  //! A genuine (non-repeat) key down seen by the local input monitor, on
  //! its thread, in EVERY role: the one Esc-burst counter of this process.
  //! Never touches the core event loop (a wedged loop is what the gesture
  //! is for).
  void onLocalKeyDown(KeyID id, KeyModifierMask mask);
  //! Settle poll for the Esc burst (RescueSettleTimer thread, or tests
  //! with an injected clock): decides and fires the burst's action.
  void settleEscBurst(EscTapRescue::Clock::time_point now = EscTapRescue::Clock::now());
  void fireRescueAction(RescueAction action);
  //! Off-loop restart: post a liveness probe to the core event loop and
  //! hard-exit (supervisor relaunch) unless it is dispatched in time.
  void armRescueAckWatchdog();
  void onRescueProbeProcessed();
  void exitProcess(int code, const std::string &reason);
  //! Fleet-wide commands are accepted only from a configured peer's
  //! address (see PeerAddressAllowlist); drops are logged once per line.
  bool sourceAllowed(const Message &message, const char *kind);
  static std::vector<std::string> peerAddressEntries(const PeerList &peers);

  bool isKnownPeer(const std::string &name) const;
  bool relayPassThroughLocal();
  void promoteSelf(const char *reason);
  void followSender(const Message &claim);
  void decide(Role role, const std::string &serverAddress, bool restart = false);
  void broadcastClaim();
  void workerLoop();
  void discoverOnce();
  void probePeerMeshVersions();
  void noteVersionMismatch(const std::string &peerName);
  void clearVersionMismatch(const std::string &peerName);

  CoordinatorConfig m_config;
  //! Addresses the configured peers resolve to (rescue/stop-all gating).
  PeerAddressAllowlist m_peerAllowlist;
  std::unique_ptr<CoordinationMesh> m_mesh;
  //! One outbound lane per configured peer (self excluded), keyed by peer
  //! name. Declared after m_mesh: the lanes send through it and must be
  //! destroyed first.
  std::map<std::string, std::unique_ptr<PeerOutbox>> m_outboxes;
  std::unique_ptr<ILocalInputMonitor> m_inputMonitor;
  std::unique_ptr<IKeyboardRelayMonitor> m_keyboardRelay;

  IEventQueue *m_events = nullptr;
  //! Set by the epoch loop; read on the worker tick and inside the OS
  //! keyboard hook (no lock: see runningRole()).
  std::atomic<Role> m_runningRole{Role::Init};
  std::string m_fleetCursorHost;
  //! Monotonic fleet fragment sequence (authoritative).
  int64_t m_fleetSeq = 0;
  FleetState m_fleetState;

  mutable std::mutex m_mutex; // guards election state + decision + interrupt + fleet
  ElectionState m_election;
  std::function<void()> m_interrupt;
  RoleDecision m_decision;
  bool m_hasDecision = false;
  bool m_quit = false;
  std::condition_variable m_decisionReady;

  std::thread m_worker;
  std::condition_variable m_workerWake;
  bool m_workerStop = false;
  bool m_broadcastPending = false;
  double m_startedAt = 0.0;
  WedgeDetector m_wedge; //!< guarded by m_mutex
  bool m_loggedKeyForward = false;
  bool m_loggedKeyForwardReceive = false;
  //! Lane the last key was forwarded on (guarded by m_mutex; lanes live as
  //! long as *this). Boundary releases (stop flush) go there.
  PeerOutbox *m_lastKeyDestination = nullptr;
  //! Our own outgoing key sequence (guarded by m_mutex).
  int64_t m_keySeq = 0;
  //! Highest key seq accepted per sender (guarded by m_mutex): a Down or
  //! Repeat at or below it is a duplicate/reordered delivery and is dropped.
  //! Forgotten on the sender's hello (its counter restarted with it).
  std::map<std::string, int64_t> m_lastKeySeqBySender;
  //! Last claim seq seen per sender (guarded by m_mutex): the same line
  //! delivered twice (older peers send to both ip and lan) is dropped.
  std::map<std::string, int64_t> m_lastClaimSeqBySender;
  //! When the last fleet rescue was accepted (guarded by m_mutex).
  double m_lastRescueAt = -1.0e9;
  //! When this seat last REQUESTED a fleet rescue (guarded by m_mutex):
  //! the off-loop counter and the Server's on-loop counter see the same
  //! taps, so the second request inside kFleetRescueDedupeS is dropped.
  double m_lastFleetRescueAt = -1.0e9;
  std::function<void(const std::string &)> m_keyClearAllHandler; //!< guarded by m_mutex
  //! Esc burst counter (guarded by m_mutex; fed from the keyboard hook).
  EscTapRescue m_escTapRescue;
  //! When set (unit tests), used instead of ipcRequestLocalCoreRestart().
  std::function<void()> m_localCoreRestartHook;
  //! When set (unit tests), used instead of the process-wide stop-all.
  std::function<void()> m_localStopAllHook;
  //! Set once a stop-all was requested or received (guarded by m_mutex).
  bool m_stopAllTriggered = false;
  //! Wakes settleEscBurst() once the burst has been silent for kSettleMs.
  RescueSettleTimer m_escSettleTimer;
  //! Hard-exits the process when the event loop never acknowledges an
  //! off-loop 5x Esc restart (CoordinationRescueProbe).
  ExitWatchdog m_rescueWatchdog;
  //! How long the loop gets to dispatch the probe (tests shrink it).
  std::chrono::milliseconds m_rescueAckTimeout{kRescueAckTimeout};
  //! When set (unit tests), used instead of hardExit().
  std::function<void(int)> m_exitProcessHook;
  bool m_probeHandlerInstalled = false; //!< guarded by m_mutex
  std::set<std::string> m_versionMismatchPeers;
  //! Last wake action per peer (rate limit; guarded by m_mutex).
  std::map<std::string, std::chrono::steady_clock::time_point> m_lastWakeAt;
  std::atomic<uint64_t> m_meshRx{0};
  std::atomic<uint64_t> m_meshDup{0};
  //! Role flips / fleet rescues in the last hour (guarded by m_mutex;
  //! mutable so the const health read can drop expired entries).
  mutable std::deque<std::chrono::steady_clock::time_point> m_flipTimes;
  mutable std::deque<std::chrono::steady_clock::time_point> m_rescueTimes;
};

} // namespace deskflow::coordination
