/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ElectionStateTests.h"

#include "coordination/ElectionState.h"
#include <QTest>

using deskflow::coordination::ElectionState;
using deskflow::coordination::ElectionTuning;
using deskflow::coordination::Role;
using ClaimAction = ElectionState::ClaimAction;

namespace {

//! Test fixture wrapping ElectionState with a controllable clock.
struct Fixture
{
  double now = 1000.0;
  ElectionState state;

  explicit Fixture(const std::string &name = "self", ElectionTuning tuning = {})
      : state(name, tuning, [this] { return now; })
  {
  }

  //! Feed \p count input ticks \p spacing seconds apart; returns whether
  //! any tick completed a promotion burst.
  bool feedInput(int count, double spacing)
  {
    bool promoted = false;
    for (int i = 0; i < count; ++i) {
      now += spacing;
      promoted = state.onLocalInput() || promoted;
    }
    return promoted;
  }
};

} // namespace

void ElectionStateTests::initialRoleIsInit()
{
  Fixture f;
  QCOMPARE(f.state.role(), Role::Init);
  QCOMPARE(f.state.seq(), 0);
}

void ElectionStateTests::inputBurstPromotes()
{
  Fixture f;
  QVERIFY(f.feedInput(8, 0.05));
}

void ElectionStateTests::shortBurstNeverPromotes()
{
  // A bumped desk or a relayed wheel echo yields a handful of events; only
  // a sustained grab (8 within 0.5 s) may flip the fleet.
  Fixture f;
  QVERIFY(!f.feedInput(7, 0.05));
  f.now += 1.0;
  QVERIFY(!f.feedInput(8, 0.08)); // 8 events spread over 0.64 s
  f.now += 1.0;
  QVERIFY(f.feedInput(8, 0.05));
}

void ElectionStateTests::burstThresholdIsTunable()
{
  ElectionTuning tuning;
  tuning.burstCount = 3;
  tuning.burstWindowS = 0.2;
  Fixture f("self", tuning);
  QVERIFY(f.feedInput(3, 0.05));
}

void ElectionStateTests::sparsePhantomDriftNeverPromotes()
{
  Fixture f;
  // Idle optical-sensor drift: single events ~0.5s apart never fill the
  // 8-in-0.50s burst, no matter how long it goes on.
  QVERIFY(!f.feedInput(100, 0.5));
}

void ElectionStateTests::escalatingCooldownDampsFlapWar()
{
  Fixture f;
  // First flip: base cooldown applies.
  f.state.becameClient("10.0.0.2");
  f.now += 11.0; // past base 2.5s cooldown AND the 10s link-down grace (link never came up)
  QVERIFY(f.feedInput(8, 0.05));

  // War: two more flips inside the 30s flap window.
  f.state.becameServer();
  f.state.becameClient("10.0.0.2");

  // Escalated cooldown (3 flips -> 2.5 * 2^2 = 10s): input shortly after
  // the flip must NOT promote, however vigorous.
  f.now += 3.0;
  QVERIFY(!f.feedInput(8, 0.05));

  // After the escalated window passes, promotion works again.
  f.now += 9.0;
  QVERIFY(f.feedInput(8, 0.05));
}

void ElectionStateTests::slowInputNeverPromotes()
{
  Fixture f;
  // Each tick 0.5s apart: the 8-in-0.50s burst never fills.
  QVERIFY(!f.feedInput(50, 0.5));
}

void ElectionStateTests::selfCooldownBlocksPromotion()
{
  Fixture f;
  f.state.becameClient("10.0.0.2");

  // Within the 2.5s self-cooldown nothing promotes, however fast.
  QVERIFY(!f.feedInput(20, 0.01));

  // After the cooldown (and, for a client whose link never came up, the
  // 10s link-down grace) a burst promotes again.
  f.now += 11.0;
  QVERIFY(f.feedInput(8, 0.05));
}

void ElectionStateTests::serverNeverPromotesAgain()
{
  Fixture f;
  f.state.becameServer();
  f.now += 10.0; // far outside any cooldown
  QVERIFY(!f.feedInput(50, 0.01));
}

void ElectionStateTests::cursorHereRaisesThreshold()
{
  Fixture f;
  f.state.becameClient("10.0.0.2");
  f.now += 11.0; // link never came up: past the link-down grace, input may promote
  f.state.setCursorHere(true);

  // The normal threshold (8) must not promote while the cursor is here.
  QVERIFY(!f.feedInput(8, 0.05));
  // But a sustained 12-within-0.80s burst must.
  QVERIFY(f.feedInput(12, 0.05));
}

void ElectionStateTests::connectedClientNeverPromotesOnLocalInput()
{
  // 2026-10-08: seven role flips a day from the MacBook's own trackpad and
  // an Easy-Switched mouse while the hackintosh was serving fine. While the
  // link to the server is up, local input is just local input.
  Fixture f;
  f.state.becameClient("10.0.0.2");
  f.now += 1.0;
  f.state.setServerLinkUp(true);
  f.now += 60.0;
  QVERIFY(!f.feedInput(8, 0.05));
  QVERIFY(!f.feedInput(40, 0.01));
  // The link drops: not yet -- the server may just be restarting.
  f.state.setServerLinkUp(false);
  f.now += 2.0;
  QVERIFY(!f.feedInput(8, 0.05));
  // Past the grace the server is genuinely gone: a sustained grab promotes.
  f.now += ElectionTuning{}.linkDownGraceS;
  QVERIFY(f.feedInput(8, 0.05));
}

void ElectionStateTests::freshClientEpochCountsAsLinkDown()
{
  // A client epoch starts disconnected; it has the grace to connect before
  // local input may promote, and a connect inside the grace closes the door.
  Fixture f;
  f.state.becameClient("10.0.0.2");
  f.now += 5.0;
  QVERIFY(!f.feedInput(8, 0.05));
  f.state.setServerLinkUp(true);
  f.now += 30.0;
  QVERIFY(!f.feedInput(8, 0.05));
  // Init (first boot, nobody serving yet) is unaffected by the gate.
  Fixture g;
  QVERIFY(g.feedInput(8, 0.05));
}

void ElectionStateTests::flappingLinkDoesNotExtendGrace()
{
  // A crash-looping server connects for a few seconds at a time. Only a
  // link that was STABLE for the grace restarts the grace clock, so this
  // seat becomes promotable 10 s after the first loss, not never.
  Fixture f;
  f.state.becameClient("10.0.0.2");
  f.now += 1.0;
  f.state.setServerLinkUp(true);
  f.now += 3.0;
  f.state.setServerLinkUp(false); // up for 3 s: a flap, not a stable link
  f.now += 2.0;
  f.state.setServerLinkUp(true);
  f.now += 3.0;
  f.state.setServerLinkUp(false);
  f.now += 2.0; // 11 s after becameClient, link down
  QVERIFY(f.feedInput(8, 0.05));

  // A link that WAS stable restarts the clock on its loss.
  Fixture g;
  g.state.becameClient("10.0.0.2");
  g.state.setServerLinkUp(true);
  g.now += 60.0;
  g.state.setServerLinkUp(false);
  g.now += 5.0;
  QVERIFY(!g.feedInput(8, 0.05));
  g.now += 6.0;
  QVERIFY(g.feedInput(8, 0.05));
}

void ElectionStateTests::cursorToggleClearsBurst()
{
  Fixture f;
  // 7 fast ticks, then the cursor regime changes: stale ticks must not
  // count toward the next threshold.
  QVERIFY(!f.feedInput(7, 0.01));
  f.state.setCursorHere(true);
  QVERIFY(!f.feedInput(11, 0.01));
}

void ElectionStateTests::claimFromSelfIgnored()
{
  Fixture f("alpha");
  QCOMPARE(f.state.onClaim("alpha", "10.0.0.1", "alpha.local", 5), ClaimAction::Ignore);
}

void ElectionStateTests::claimDuringServerCooldownIgnored()
{
  Fixture f;
  f.state.becameServer();
  f.now += 1.0; // inside the 1.5s claim cooldown
  QCOMPARE(f.state.onClaim("beta", "10.0.0.2", "beta.local", 1), ClaimAction::Ignore);
}

void ElectionStateTests::claimAfterServerCooldownFollowed()
{
  Fixture f;
  f.state.becameServer();
  f.now += 2.0; // outside the cooldown
  QCOMPARE(f.state.onClaim("beta", "10.0.0.2", "beta.local", 1), ClaimAction::FollowSender);
}

void ElectionStateTests::clientSelfCooldownBlocksForeignClaim()
{
  // Regression: a client that just switched must NOT immediately follow a
  // claim from a different peer, or two machines ping-pong leadership and
  // churn epochs (which raced socket teardown into a use-after-free).
  Fixture f;
  f.state.becameClient("10.0.0.2"); // lastSwitchAt = now

  // Within the 2.5s self-cooldown a foreign peer's claim is ignored...
  f.now += 1.0;
  QCOMPARE(f.state.onClaim("other", "10.0.0.3", "other.local", 5), ClaimAction::Ignore);

  // ...but once the cooldown elapses the same claim is followed.
  f.now += 2.0; // 3.0s since the switch, past the cooldown
  QCOMPARE(f.state.onClaim("other", "10.0.0.3", "other.local", 6), ClaimAction::FollowSender);
}

void ElectionStateTests::sameHostHeartbeatIsNoOp()
{
  Fixture f;
  f.state.becameClient("10.0.0.2");
  f.now += 10.0;

  // Heartbeats from the host we already follow are strict no-ops,
  // whether they match by ip or by lan address.
  QCOMPARE(f.state.onClaim("beta", "10.0.0.2", "beta.local", 2), ClaimAction::Ignore);
  f.state.becameClient("beta.local");
  f.now += 10.0;
  QCOMPARE(f.state.onClaim("beta", "10.0.0.2", "beta.local", 3), ClaimAction::Ignore);

  // A different host's claim is followed.
  QCOMPARE(f.state.onClaim("gamma", "10.0.0.3", "gamma.local", 4), ClaimAction::FollowSender);
}

void ElectionStateTests::claimMergesSequenceNumbers()
{
  Fixture f;
  f.state.onClaim("beta", "10.0.0.2", "beta.local", 41);
  QCOMPARE(f.state.seq(), 41);
  // Lower sequence numbers never regress the gossip value.
  f.state.onClaim("gamma", "10.0.0.3", "gamma.local", 7);
  QCOMPARE(f.state.seq(), 41);
  QCOMPARE(f.state.nextClaimSeq(), 42);
}

void ElectionStateTests::transitionsResetBurstAndCursor()
{
  Fixture f;
  f.state.setCursorHere(true);
  f.feedInput(3, 0.01);

  f.state.becameServer();
  QCOMPARE(f.state.role(), Role::Server);
  QVERIFY(!f.state.cursorHere());

  f.state.becameClient("10.0.0.2");
  QCOMPARE(f.state.role(), Role::Client);
  QCOMPARE(f.state.serverAddress(), std::string("10.0.0.2"));
  f.state.resetCursorScreen();
  QVERIFY(!f.state.cursorHere());
}

void ElectionStateTests::cursorHereTracksScreenPresenceForRelay()
{
  Fixture f;
  f.state.becameClient("10.0.0.2");
  f.state.resetCursorScreen();
  QVERIFY(!f.state.cursorScreenKnown());
  QVERIFY(!f.state.cursorHere());

  f.state.setCursorHere(true);
  QVERIFY(f.state.cursorScreenKnown());
  QVERIFY(f.state.cursorHere());

  f.state.setCursorHere(false);
  QVERIFY(f.state.cursorScreenKnown());
  QVERIFY(!f.state.cursorHere());
}

void ElectionStateTests::relayStartClearsStaleCursorScreenKnown()
{
  Fixture f;
  f.state.setCursorHere(true);
  f.state.becameClient("10.0.0.2");
  QVERIFY(f.state.cursorScreenKnown());
  f.state.resetCursorScreen();
  QVERIFY(!f.state.cursorScreenKnown());
  QVERIFY(!f.state.cursorHere());
}

void ElectionStateTests::becameServerClearsCursorScreenKnown()
{
  Fixture f;
  f.state.setCursorHere(true);
  f.state.becameServer();
  QVERIFY(!f.state.cursorScreenKnown());
  QVERIFY(!f.state.cursorHere());
}

void ElectionStateTests::resetCursorScreenClearsInputBurst()
{
  Fixture f;
  f.state.becameClient("10.0.0.2");
  f.now += 11.0; // past the link-down grace: input may count
  f.feedInput(7, 0.01);
  f.state.resetCursorScreen();
  f.now += 3.0;
  QVERIFY(!f.feedInput(7, 0.01));
  QVERIFY(f.feedInput(1, 0.05));
}

void ElectionStateTests::resetCursorScreenClearsKnownState()
{
  Fixture f;
  f.state.setCursorHere(true);
  f.state.resetCursorScreen();
  QVERIFY(!f.state.cursorScreenKnown());
  QVERIFY(!f.state.cursorHere());
}

QTEST_MAIN(ElectionStateTests)
