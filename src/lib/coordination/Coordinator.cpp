/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/Coordinator.h"

#include "base/Event.h"
#include "base/EventQueue.h"
#include "base/EventTypes.h"
#include "base/Log.h"
#include "common/FleetCursor.h"
#include "coordination/CoordinationEvents.h"
#include "coordination/CoordinationProtocol.h"
#include "coordination/FleetStateMerge.h"
#include "coordination/KeyboardRescue.h"
#include "coordination/KeyboardRouter.h"
#include "coordination/RelayKeyEvent.h"
#include "coordination/WakeOnLan.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace deskflow::coordination {

using deskflow::common::cursorHostIsLocal;
using deskflow::common::namesEqual;

namespace {

const double kHeartbeatIntervalS = 3.0;
const double kDiscoveryWindowS = 30.0;
const double kWorkerTickS = 1.0;
const int kLanProbeTimeoutMs = 700;
const int kWedgeProbeTimeoutMs = 1000;
const int kWedgeProbeEveryTicks = 9;
const int kWedgeStrikesToRestart = 2;
const int kVersionProbeEveryTicks = 15;
//! How long a key forward may wait for a peer whose reachability is still
//! Unknown (first send only). Well inside the OS input-hook budget
//! (Windows LowLevelHooksTimeout 300 ms; macOS event-tap timeout).
const int kKeyForwardGraceMs = 50;

double monotonicSeconds()
{
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

RelayKeyEvent relayEventFromMessage(const Message &message)
{
  RelayKeyEvent event;
  event.phase = message.keyPhase;
  event.id = message.keyId;
  event.mask = message.keyMask;
  event.button = message.keyButton;
  event.lang = message.keyLang;
  event.from = message.name;
  return event;
}

} // namespace

Coordinator::Coordinator(CoordinatorConfig config)
    : m_config(std::move(config)),
      m_election(m_config.selfName, m_config.tuning, monotonicSeconds)
{
  m_fleetState.peers.reserve(m_config.peers.size());
  for (const auto &peer : m_config.peers) {
    m_fleetState.peers.push_back(FleetPeer{peer.name, peer.ip, peer.lan});
  }

  m_mesh = std::make_unique<CoordinationMesh>(
      m_config.meshPort, m_config.token,
      [this](const Message &message, const std::function<void(const std::string &)> &reply) {
        onMessage(message, reply);
      }
  );
  // The blocking connects live on the lanes' own threads; sendTo/query
  // keep their one-connect-per-line wire behavior.
  const auto transport = [mesh = m_mesh.get()](const std::string &host, const std::string &line, std::string *reply) {
    if (reply != nullptr) {
      *reply = mesh->query(host, line);
      return !reply->empty();
    }
    return mesh->sendTo(host, line);
  };
  for (const auto &peer : m_config.peers) {
    if (namesEqual(peer.name, m_config.selfName) || m_outboxes.contains(peer.name)) {
      continue;
    }
    m_outboxes.emplace(peer.name, std::make_unique<PeerOutbox>(peer.ip, peer.lan, transport, monotonicSeconds));
  }
  m_inputMonitor = createLocalInputMonitor();
  m_keyboardRelay = createKeyboardRelayMonitor();
}

Coordinator::~Coordinator()
{
  stop();
}

namespace {
//! Live coordinator for the process-wide fleet-rescue hook (KeyboardRescue
//! takes a plain function pointer). Single coordinator per process.
Coordinator *g_rescueCoordinator = nullptr;
void fleetRescueThunk()
{
  if (g_rescueCoordinator != nullptr) {
    g_rescueCoordinator->requestFleetRescue();
  }
}
} // namespace

bool Coordinator::start()
{
  if (!m_mesh->start()) {
    return false;
  }
  g_rescueCoordinator = this;
  setFleetRescueHandler(&fleetRescueThunk);
  for (auto &[name, outbox] : m_outboxes) {
    outbox->start();
  }
  m_inputMonitor->start([this] { onGenuineInput(); });
  m_startedAt = monotonicSeconds();
  m_workerStop = false;
  m_worker = std::thread([this] { workerLoop(); });
  // Settle every lane's reachability early so the first key forward does
  // not have to wait for it (and version mismatches surface at once).
  probePeerMeshVersions();
  LOG_INFO(
      "coordination: started as \"%s\" with %d peer(s)", m_config.selfName.c_str(),
      static_cast<int>(m_config.peers.size())
  );
  return true;
}

void Coordinator::stop()
{
  if (g_rescueCoordinator == this) {
    setFleetRescueHandler(nullptr);
    g_rescueCoordinator = nullptr;
  }
  {
    std::scoped_lock lock{m_mutex};
    m_workerStop = true;
    m_quit = true;
  }
  m_workerWake.notify_all();
  m_decisionReady.notify_all();
  if (m_worker.joinable()) {
    m_worker.join();
  }
  m_inputMonitor->stop();
  m_keyboardRelay->stop();
  for (auto &[name, outbox] : m_outboxes) {
    outbox->stop(); // before the mesh: lanes send through it
  }
  m_mesh->stop();
}

RoleDecision Coordinator::awaitRoleDecision()
{
  std::unique_lock lock{m_mutex};
  m_decisionReady.wait(lock, [this] { return m_hasDecision || m_quit; });
  if (m_quit) {
    return RoleDecision{Role::Init, {}, true};
  }
  m_hasDecision = false;
  return m_decision;
}

void Coordinator::setInterruptCallback(std::function<void()> interrupt)
{
  std::scoped_lock lock{m_mutex};
  m_interrupt = std::move(interrupt);
}

void Coordinator::notifyCursorHere(bool here)
{
  std::scoped_lock lock{m_mutex};
  m_election.setCursorHere(here);
}

void Coordinator::notifyEpochEnded()
{
  // The app exited without a new decision (screen error, transport
  // failure). Re-arm the current role so the epoch loop restarts it;
  // election events can still override at any time.
  std::scoped_lock lock{m_mutex};
  if (m_quit || m_hasDecision) {
    return;
  }
  if (m_election.role() != Role::Init) {
    m_decision = RoleDecision{m_election.role(), m_election.serverAddress(), false};
    m_hasDecision = true;
    m_decisionReady.notify_all();
  }
}

void Coordinator::requestQuit()
{
  std::function<void()> interrupt;
  {
    std::scoped_lock lock{m_mutex};
    m_quit = true;
    interrupt = m_interrupt;
  }
  m_decisionReady.notify_all();
  m_workerWake.notify_all();
  if (interrupt) {
    interrupt();
  }
}

bool Coordinator::hasPendingDecision()
{
  std::scoped_lock lock{m_mutex};
  return m_hasDecision || m_quit;
}

void Coordinator::setEventQueue(IEventQueue *events)
{
  std::scoped_lock lock{m_mutex};
  m_events = events;
}

FleetState Coordinator::fleetSnapshot() const
{
  std::scoped_lock lock{m_mutex};
  return m_fleetState;
}

void Coordinator::postFleetStateEvents(IEventQueue *events, const FleetMergeResult &merge)
{
  if (events == nullptr || !merge.changed) {
    return;
  }
  // Consumers register on the system target; a default-constructed Event
  // has a null target and exact-match dispatch would drop it silently.
  events->addEvent(Event(EventTypes::CoordinationFleetStateChanged, events->getSystemTarget()));
  if (merge.topologyBecameReady) {
    events->addEvent(Event(EventTypes::CoordinationTopologyReady, events->getSystemTarget()));
  }
}

std::vector<FleetPeer> Coordinator::buildFleetPeersLocked()
{
  if (!m_fleetState.peers.empty()) {
    return m_fleetState.peers;
  }
  std::vector<FleetPeer> peers;
  peers.reserve(m_config.peers.size());
  for (const auto &peer : m_config.peers) {
    peers.push_back(FleetPeer{peer.name, peer.ip, peer.lan});
  }
  return peers;
}

void Coordinator::sendLineToPeers(const std::string &line)
{
  for (auto &[name, outbox] : m_outboxes) {
    outbox->post(line);
  }
}

PeerOutbox *Coordinator::outboxByName(const std::string &name) const
{
  for (const auto &[peerName, outbox] : m_outboxes) {
    if (namesEqual(peerName, name)) {
      return outbox.get();
    }
  }
  return nullptr;
}

PeerOutbox *Coordinator::outboxForHostLocked(const std::string &hostName) const
{
  if (auto *direct = outboxByName(hostName); direct != nullptr) {
    return direct;
  }
  const auto &fleet = m_fleetState;
  if (!fleet.server.empty() && (namesEqual(hostName, fleet.server) || namesEqual(hostName, fleet.cursorScreen) ||
                                namesEqual(hostName, fleet.cursorHost))) {
    return outboxByName(fleet.server);
  }
  return nullptr;
}

bool Coordinator::mergeAndBroadcastFleetFragment(const FleetFragment &fragment, bool sendEvenIfUnchanged)
{
  IEventQueue *events = nullptr;
  FleetMergeResult merge;
  std::string line;
  {
    std::scoped_lock lock{m_mutex};
    if (m_election.role() != Role::Server) {
      return false;
    }
    if (!fragment.cursorHost.empty()) {
      m_fleetCursorHost = fragment.cursorHost;
    }
    events = m_events;
    merge = applyServerFragment(m_fleetState, fragment);
    if (sendEvenIfUnchanged || merge.changed) {
      line = protocol::encodeFleet(fragment, m_config.token);
    }
  }

  postFleetStateEvents(events, merge);

  if (!line.empty()) {
    // Callers include the server event loop (screen switch): posting is
    // non-blocking, and the heartbeat rebroadcast converges any client
    // that misses an intermediate fragment.
    sendLineToPeers(line);
  }
  return merge.changed;
}

void Coordinator::handleHelloMessage(const Message &message, const std::function<void(const std::string &)> &reply)
{
  const std::string peerName = message.name.empty() ? "unknown" : message.name;
  if (message.meshVersion < kMeshProtocolVersion) {
    LOG_WARN(
        "coordination: rejecting mesh v%d peer \"%s\" (local v%d)", message.meshVersion, peerName.c_str(),
        kMeshProtocolVersion
    );
    if (!message.name.empty()) {
      noteVersionMismatch(message.name);
    }
    return;
  }
  if (!message.name.empty()) {
    clearVersionMismatch(message.name);
  }
  LOG_DEBUG("coordination: mesh hello from \"%s\" (v=%d)", message.name.c_str(), message.meshVersion);
  reply(protocol::encodeHello(kMeshProtocolVersion, m_config.selfName, m_config.token));
}

void Coordinator::handleFleetMessage(const Message &message)
{
  IEventQueue *events = nullptr;
  FleetMergeResult merge;
  int64_t seq = 0;
  size_t linkCount = 0;
  {
    std::scoped_lock lock{m_mutex};
    if (m_election.role() == Role::Server) {
      return;
    }
    events = m_events;
    merge = applyServerFragment(m_fleetState, message.fleet);
    seq = m_fleetState.seq;
    linkCount = m_fleetState.links.size();
  }
  if (merge.changed) {
    LOG_DEBUG("coordination: fleet snapshot updated (seq=%lld links=%zu)", static_cast<long long>(seq), linkCount);
  }
  postFleetStateEvents(events, merge);
}

void Coordinator::updateCursorHost(const std::string &screenName)
{
  if (screenName.empty()) {
    return;
  }

  FleetFragment fragment;
  {
    std::scoped_lock lock{m_mutex};
    if (m_election.role() != Role::Server) {
      LOG_DEBUG("coordination: cursor host update \"%s\" ignored (not server)", screenName.c_str());
      return;
    }
    LOG_DEBUG("coordination: fleet cursor host -> \"%s\"", screenName.c_str());
    // Always author as self: after a server takeover m_fleetState.server
    // still names the previous server until our first publish lands.
    fragment.server = m_config.selfName;
    fragment.seq = ++m_fleetSeq;
    fragment.cursorHost = screenName;
    fragment.cursorScreen = screenName;
    fragment.links = m_fleetState.links;
    fragment.screens = m_fleetState.screens;
    fragment.peers = buildFleetPeersLocked();
  }

  mergeAndBroadcastFleetFragment(fragment, true);
}

void Coordinator::publishFleetTopology(std::vector<FleetLink> links, std::vector<FleetScreen> screens)
{
  if (screens.empty()) {
    return;
  }

  FleetFragment fragment;
  {
    std::scoped_lock lock{m_mutex};
    if (m_election.role() != Role::Server) {
      return;
    }

    fragment.server = m_config.selfName;
    fragment.seq = ++m_fleetSeq;
    fragment.cursorHost = m_fleetCursorHost.empty() ? m_config.selfName : m_fleetCursorHost;
    fragment.cursorScreen = fragment.cursorHost;
    fragment.links = std::move(links);
    fragment.screens = std::move(screens);
    fragment.peers = buildFleetPeersLocked();
  }

  const bool changed = mergeAndBroadcastFleetFragment(fragment, false);
  if (changed) {
    const auto snapshot = fleetSnapshot();
    LOG_DEBUG(
        "coordination: published fleet topology (seq=%lld links=%zu)", static_cast<long long>(snapshot.seq),
        snapshot.links.size()
    );
  }
}

void Coordinator::wakePeer(const std::string &name)
{
  Peer target;
  {
    std::scoped_lock lock{m_mutex};
    if (m_election.role() != Role::Server) {
      return;
    }
    const auto match = std::find_if(m_config.peers.begin(), m_config.peers.end(), [&name](const Peer &peer) {
      return namesEqual(peer.name, name);
    });
    if (match == m_config.peers.end() || (match->mac.empty() && match->wakeCommand.empty())) {
      return;
    }
    target = *match;
    const auto now = std::chrono::steady_clock::now();
    if (const auto lastWake = m_lastWakeAt.find(target.name);
        lastWake != m_lastWakeAt.end() && now - lastWake->second < std::chrono::seconds(30)) {
      return;
    }
    m_lastWakeAt[target.name] = now;
  }

  if (!target.mac.empty()) {
    sendWakeOnLan(target.mac);
  }
  if (!target.wakeCommand.empty()) {
    LOG_INFO("coordination: waking peer %s: %s", target.name.c_str(), target.wakeCommand.c_str());
    // Detached: wake commands (e.g. ssh to a hypervisor) can take seconds
    // and must never block the event loop or the coordination worker. The
    // exit report uses stderr, not LOG_WARN: a detached thread can outlive
    // main() and must not touch the Log singleton during static teardown.
    std::thread([command = target.wakeCommand, peerName = target.name] {
      const int status = std::system(command.c_str()); // NOSONAR -- operator-configured wake hook
      if (status != 0) {
        std::fprintf(stderr, "coordination: wake command for %s exited with status %d\n", peerName.c_str(), status);
      }
    }).detach();
  }
}

void Coordinator::updateKeyboardRelayForRole(Role role)
{
  if (!m_config.keyboardFollowCursor) {
    {
      std::scoped_lock lock{m_mutex};
      m_loggedKeyForward = false;
      m_loggedKeyForwardReceive = false;
    }
    LOG_DEBUG("coordination: keyboard follow-cursor disabled; relay not started");
    m_keyboardRelay->stop();
    return;
  }
  if (role == Role::Client) {
    {
      std::scoped_lock lock{m_mutex};
      m_loggedKeyForward = false;
      m_loggedKeyForwardReceive = false;
      // Do not reset EscTapRescue here: the tick reconciler restarts a dead
      // relay and must not wipe an in-progress 5× Esc sequence.
      // Epoch restart does not call becameClient(); clear stale screen sync.
      m_election.resetCursorScreen();
      // Also drop the PREVIOUS epoch's cursor host. A fresh relay reads this
      // immediately; if it still named a remote screen, every local keystroke
      // (including at a login prompt) was swallowed and forwarded until the
      // next fleet fragment arrived seconds later. Unknown host = keys stay
      // local, which is the safe default (see routeKeyboard).
      m_fleetState.cursorHost.clear();
    }
    // Routing follows the fleet cursor host; an unknown host always passes
    // keys locally (see routeKeyboard).
    m_keyboardRelay->start(
        [this] { return relayPassThroughLocal(); },
        [this](Message::KeyPhase phase, KeyID id, KeyModifierMask mask, KeyButton button, const std::string &lang) {
          return sendKeyForward(phase, id, mask, button, lang);
        }
    );
    LOG_INFO("coordination: keyboard relay started (client epoch)");
    return;
  }
  {
    std::scoped_lock lock{m_mutex};
    m_loggedKeyForward = false;
    m_loggedKeyForwardReceive = false;
    m_escTapRescue.reset();
  }
  // Server epoch: keyboard uses Server::onKeyDown → m_active (not key relay).
  m_keyboardRelay->stop();
}

void Coordinator::onMessage(const Message &message, const std::function<void(const std::string &)> &reply)
{
  switch (message.type) {
  case Message::Type::Claim: {
    ElectionState::ClaimAction action;
    {
      std::scoped_lock lock{m_mutex};
      action = m_election.onClaim(message.name, message.ip, message.lan, message.seq);
    }
    if (action == ElectionState::ClaimAction::FollowSender) {
      followSender(message);
    }
    break;
  }

  case Message::Type::Promote:
    LOG_INFO("coordination: manual promote received");
    promoteSelf("manual promote");
    break;

  case Message::Type::Rescue:
    // Never re-broadcast: the originator already fanned out to every peer,
    // so echoing would restart-storm the fleet.
    LOG_INFO("coordination: fleet keyboard rescue received -- restarting local core");
    requestLocalCoreRestart();
    break;

  case Message::Type::Status: {
    std::string snapshot;
    {
      std::scoped_lock lock{m_mutex};
      std::vector<std::string> mismatches(m_versionMismatchPeers.begin(), m_versionMismatchPeers.end());
      snapshot = protocol::encodeStatusReply(
          m_election.role(), m_election.serverAddress(), m_election.seq(), m_election.lastSwitchAt(), m_config.selfName,
          &m_fleetState, kMeshProtocolVersion, mismatches
      );
    }
    reply(snapshot);
    break;
  }

  case Message::Type::Key:
    handleKeyForwardMessage(message);
    break;

  case Message::Type::Hello:
    handleHelloMessage(message, reply);
    break;

  case Message::Type::Fleet:
    handleFleetMessage(message);
    break;

  default:
    break;
  }
}

void Coordinator::handleKeyForwardMessage(const Message &message)
{
  Role role;
  IEventQueue *events = nullptr;
  std::string selfName;
  std::string cursorHost;
  {
    std::scoped_lock lock{m_mutex};
    role = m_election.role();
    events = m_events;
    selfName = m_config.selfName;
    cursorHost = m_fleetState.cursorHost;
  }

  const bool serverEpoch = role == Role::Server;
  const bool clientCursorHost = role == Role::Client && cursorHostIsLocal(selfName, cursorHost);
  if (events == nullptr || (!serverEpoch && !clientCursorHost)) {
    return;
  }
  // Both paths inject keystrokes into the local OS; both require the sender
  // to be a configured peer (the shared token alone is not enough).
  if (!isKnownPeer(message.name)) {
    LOG_DEBUG("coordination: dropping relay key from unknown peer \"%s\"", message.name.c_str());
    return;
  }

  bool logFirst = false;
  {
    std::scoped_lock lock{m_mutex};
    if (!m_loggedKeyForwardReceive) {
      m_loggedKeyForwardReceive = true;
      logFirst = true;
    }
  }
  if (logFirst) {
    LOG_INFO("coordination: key from \"%s\" phase=%d", message.name.c_str(), static_cast<int>(message.keyPhase));
  } else {
    LOG_DEBUG("coordination: key from \"%s\" phase=%d", message.name.c_str(), static_cast<int>(message.keyPhase));
  }

  // EventData constructor (object slot, destructor runs on deleteData) and
  // queued dispatch: this runs on a mesh socket thread, and key injection
  // (screen switches, client screen access) must execute on the event loop.
  auto *info = new CoordinationKeyForwardInfo(relayEventFromMessage(message));
  events->addEvent(Event(EventTypes::CoordinationKeyForward, events->getSystemTarget(), info));
}

bool Coordinator::sendKeyForward(
    Message::KeyPhase phase, KeyID id, KeyModifierMask mask, KeyButton button, const std::string &lang
)
{
  // Always observe Downs (including when routing is Local) so 5× Esc still
  // works while the cursor is on this machine. True return = swallow Esc.
  if (phase == Message::KeyPhase::Down) {
    bool triggered = false;
    {
      std::scoped_lock lock{m_mutex};
      triggered = m_escTapRescue.noteEscDown(id, mask);
    }
    if (triggered) {
      // Fleet-wide: this path is what sees the taps at a login screen (the
      // elevated/secure-desktop core runs as a client epoch), and the wedged
      // machine is usually a different one.
      requestFleetRescue();
      return true;
    }
  }

  PeerOutbox *destination = nullptr;
  std::string line;
  bool logFirst = false;
  {
    std::scoped_lock lock{m_mutex};
    if (m_election.role() != Role::Client) {
      return false;
    }

    KeyboardRouteInput input;
    input.selfName = m_config.selfName;
    input.cursorHost = m_fleetState.cursorHost;
    input.cursorHostKnown = !m_fleetState.cursorHost.empty();

    const auto decision = routeKeyboard(input);
    if (decision.route == KeyboardRoute::Local) {
      return false;
    }

    destination = outboxForHostLocked(decision.forwardHost);
    if (destination == nullptr) {
      // Fall back to whichever configured peer we are following.
      const std::string serverAddress = m_election.serverAddress();
      for (const auto &peer : m_config.peers) {
        if (peer.hasAddress(serverAddress)) {
          destination = outboxByName(peer.name);
          break;
        }
      }
    }
    line = protocol::encodeKey(
        m_config.selfName, phase, static_cast<uint16_t>(id), static_cast<uint16_t>(mask), button, lang, m_config.token
    );

    if (!m_loggedKeyForward) {
      m_loggedKeyForward = true;
      logFirst = true;
    }
  }
  if (destination == nullptr) {
    return false;
  }
  const std::string address = destination->preferredAddress();
  if (logFirst) {
    LOG_INFO("coordination: forwarding keyboard to %s", address.c_str());
  } else {
    LOG_DEBUG("coordination: forwarding keyboard to %s", address.c_str());
  }
  // Runs inside the OS keyboard hook: enqueue and return. True (swallow
  // the key) only while the peer is known reachable; in backoff the key
  // stays local instead of being lost.
  return destination->forward(line, kKeyForwardGraceMs);
}

void Coordinator::requestLocalCoreRestart()
{
  if (m_localCoreRestartHook) {
    m_localCoreRestartHook();
    return;
  }
  deskflow::coordination::requestLocalCoreRestart();
}

void Coordinator::requestFleetRescue()
{
  LOG_INFO("coordination: fleet keyboard rescue -- restarting every peer");
  std::string line;
  {
    std::scoped_lock lock{m_mutex};
    if (m_quit) {
      return;
    }
    line = protocol::encodeRescue(m_config.token);
  }
  // Peers first: the lanes deliver to reachable peers within milliseconds,
  // well before the local restart IPC lands, and this may run inside the
  // keyboard hook (5x Esc) so nothing here may block.
  sendLineToPeers(line);
  requestLocalCoreRestart();
}

bool Coordinator::isKnownPeer(const std::string &name) const
{
  for (const auto &peer : m_config.peers) {
    if (namesEqual(peer.name, name)) {
      return true;
    }
  }
  return false;
}

bool Coordinator::relayPassThroughLocal()
{
  std::scoped_lock lock{m_mutex};

  KeyboardRouteInput input;
  input.selfName = m_config.selfName;
  input.cursorHost = m_fleetState.cursorHost;
  input.cursorHostKnown = !m_fleetState.cursorHost.empty();
  return routeKeyboard(input).route == KeyboardRoute::Local;
}

void Coordinator::onGenuineInput()
{
  bool promote = false;
  {
    std::scoped_lock lock{m_mutex};
    promote = m_election.onLocalInput();
  }
  if (promote) {
    promoteSelf("local input burst");
  }
}

void Coordinator::promoteSelf(const char *reason)
{
  {
    std::scoped_lock lock{m_mutex};
    if (m_quit) {
      return;
    }
    if (m_election.role() == Role::Server) {
      return; // already primary; heartbeats keep claiming
    }
    LOG_INFO("coordination: promoting to server (%s)", reason);
    // Hand the claim to the worker: it owns the heartbeat clock, and this
    // thread is often the input monitor's event-tap thread.
    m_broadcastPending = true;
  }
  decide(Role::Server, {});
  m_workerWake.notify_all();
}

void Coordinator::followSender(const Message &claim)
{
  // A claimant that cannot find itself in its own peer list (for example a
  // computerName/peers casing mismatch) broadcasts empty addresses; recover
  // by resolving the sender in *our* configured peer list by name.
  std::string stable = claim.ip;
  std::string lan = claim.lan;
  if (stable.empty() && lan.empty()) {
    for (const auto &peer : m_config.peers) {
      if (namesEqual(peer.name, claim.name)) {
        stable = peer.ip;
        lan = peer.lan;
        break;
      }
    }
  }
  if (stable.empty() && lan.empty()) {
    LOG_WARN("coordination: claim from \"%s\" has no address and is not a configured peer", claim.name.c_str());
    return;
  }

  // LAN-first: prefer the sender's LAN address when its coordination
  // port answers there; otherwise use the stable address.
  std::string address = stable.empty() ? lan : stable;
  if (!lan.empty() && lan != stable && m_mesh->probe(lan, kLanProbeTimeoutMs)) {
    address = lan;
  }
  LOG_INFO("coordination: following \"%s\" at %s", claim.name.c_str(), address.c_str());
  decide(Role::Client, address);
}

void Coordinator::decide(Role role, const std::string &serverAddress)
{
  std::function<void()> interrupt;
  {
    std::scoped_lock lock{m_mutex};
    if (m_quit) {
      return;
    }
    if (role == Role::Server) {
      m_election.becameServer();
      // Fragment sequence must stay monotonic across server changes: this
      // node may have merged the previous server's fragments up to
      // m_fleetState.seq, and publishing below that would be rejected as
      // stale by every peer (and by our own merge), freezing fleet state.
      m_fleetSeq = std::max(m_fleetSeq, m_fleetState.seq);
    } else {
      m_election.becameClient(serverAddress);
    }
    m_decision = RoleDecision{role, serverAddress, false};
    m_hasDecision = true;
    m_wedgeStrikes = 0;
    interrupt = m_interrupt;
  }
  m_decisionReady.notify_all();
  if (interrupt) {
    interrupt();
  }
}

void Coordinator::broadcastClaim()
{
  std::string line;
  {
    std::scoped_lock lock{m_mutex};
    std::string selfIp;
    std::string selfLan;
    for (const auto &peer : m_config.peers) {
      if (namesEqual(peer.name, m_config.selfName)) {
        selfIp = peer.ip;
        selfLan = peer.lan;
        break;
      }
    }
    line = protocol::encodeClaim(m_config.selfName, selfIp, selfLan, m_election.nextClaimSeq(), m_config.token);
  }
  sendLineToPeers(line);
}

void Coordinator::workerLoop()
{
  // Nothing in this loop performs peer I/O directly: every send is posted
  // to a PeerOutbox, so a sleeping peer can never stretch the 1 s tick.
  double lastHeartbeatAt = 0.0;
  int tick = 0;

  while (true) {
    bool broadcastNow = false;
    {
      std::unique_lock lock{m_mutex};
      m_workerWake.wait_for(lock, std::chrono::duration<double>(kWorkerTickS), [this] {
        return m_workerStop || m_broadcastPending;
      });
      if (m_workerStop) {
        return;
      }
      broadcastNow = m_broadcastPending;
      m_broadcastPending = false;
    }
    ++tick;
    const double now = monotonicSeconds();
    if (broadcastNow) {
      broadcastClaim();
      lastHeartbeatAt = now;
    }

    Role role;
    {
      std::scoped_lock lock{m_mutex};
      role = m_election.role();
    }

    if (role == Role::Server) {
      if (now - lastHeartbeatAt >= kHeartbeatIntervalS) {
        lastHeartbeatAt = now;
        broadcastClaim();
        // Rebroadcast the current fleet fragment so late-joining clients
        // converge without waiting for the next topology/cursor change.
        // Same seq: applyServerFragment treats equal seq as idempotent.
        // Only when we authored the snapshot: after a takeover the state
        // may still carry the previous server until our first publish.
        std::string line;
        {
          std::scoped_lock lock{m_mutex};
          if (namesEqual(m_fleetState.server, m_config.selfName) && !m_fleetState.screens.empty()) {
            line = protocol::encodeFleet(m_fleetState, m_config.token);
          }
        }
        if (!line.empty()) {
          sendLineToPeers(line);
        }
      }
      if (tick % kWedgeProbeEveryTicks == 0) {
        // Alive-but-not-accepting detection: the server process can wedge
        // while its accept loop is stuck; restart the epoch if the
        // transport port stops answering locally.
        if (m_mesh->probeDeskflowPort(m_config.deskflowPort, kWedgeProbeTimeoutMs)) {
          m_wedgeStrikes = 0;
        } else if (++m_wedgeStrikes >= kWedgeStrikesToRestart) {
          LOG_WARN("coordination: server transport wedged; restarting server epoch");
          m_wedgeStrikes = 0;
          decide(Role::Server, {});
        }
      }
    } else if (role == Role::Init && now - m_startedAt <= kDiscoveryWindowS) {
      discoverOnce();
    }

    // Relay-state reconciler: a client epoch must always have a live
    // keyboard relay monitor (its keys cannot reach other screens without
    // one), and a server epoch must never keep one (Server::onKeyDown owns
    // the keyboard). Heals epoch handoffs that missed the explicit
    // updateKeyboardRelayForRole call and taps/hooks that died silently
    // (permission loss, tap teardown).
    if (m_config.keyboardFollowCursor) {
      if (role == Role::Client && !m_keyboardRelay->running()) {
        LOG_WARN("coordination: keyboard relay not running in client epoch; restarting");
        updateKeyboardRelayForRole(Role::Client);
      } else if (role == Role::Server && m_keyboardRelay->running()) {
        LOG_WARN("coordination: keyboard relay still running in server epoch; stopping");
        m_keyboardRelay->stop();
      }
    }

    if (tick % kVersionProbeEveryTicks == 0) {
      probePeerMeshVersions();
    }
  }
}

void Coordinator::probePeerMeshVersions()
{
  const std::string hello = protocol::encodeHello(kMeshProtocolVersion, m_config.selfName, m_config.token);
  for (auto &[name, outbox] : m_outboxes) {
    // The reply handler runs on the lane thread; unreachable peers never
    // reply and keep whatever mismatch state they had.
    outbox->post(hello, [this, peerName = name](const std::string &replyLine) {
      const Message reply = protocol::decode(replyLine);
      if (reply.type != Message::Type::Hello || reply.meshVersion < kMeshProtocolVersion) {
        noteVersionMismatch(peerName);
      } else {
        clearVersionMismatch(peerName);
      }
    });
  }
}

void Coordinator::noteVersionMismatch(const std::string &peerName)
{
  if (peerName.empty()) {
    return;
  }
  std::scoped_lock lock{m_mutex};
  m_versionMismatchPeers.insert(peerName);
}

void Coordinator::clearVersionMismatch(const std::string &peerName)
{
  if (peerName.empty()) {
    return;
  }
  std::scoped_lock lock{m_mutex};
  m_versionMismatchPeers.erase(peerName);
}

void Coordinator::discoverOnce()
{
  // Best-effort: ask peers who is serving; the first server claim that
  // arrives via onMessage flips us. (Replies come back on the mesh as
  // unsolicited claims are not sent for status; legacy nodes reply with
  // a status object which we parse here.)
  const std::string line = protocol::encodeStatus(m_config.token);
  for (const auto &peer : m_config.peers) {
    auto *outbox = outboxByName(peer.name);
    if (outbox == nullptr || !outbox->idle()) {
      continue; // self, or the previous query is still in flight
    }
    outbox->post(line, [this, peer](const std::string &replyLine) {
      const auto reply = protocol::decodeStatusReply(replyLine);
      if (!reply.valid || reply.role != Role::Server) {
        return;
      }
      {
        // Replies arrive on lane threads: only the first server found
        // (or an inbound claim) may move us out of Init.
        std::scoped_lock lock{m_mutex};
        if (m_quit || m_election.role() != Role::Init) {
          return;
        }
      }
      LOG_INFO("coordination: discovered active server \"%s\"", peer.name.c_str());
      Message claim;
      claim.type = Message::Type::Claim;
      claim.name = peer.name;
      claim.ip = peer.ip;
      claim.lan = peer.lan;
      followSender(claim);
    });
  }
}

} // namespace deskflow::coordination
