/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "VirtualHostTrackerTests.h"

#include "server/VirtualHostTracker.h"

#include <QTest>

#include <functional>
#include <string>
#include <vector>

namespace {

struct SentLine
{
  void *client = nullptr;
  std::string line;
};

using SendFn = std::function<void(BaseClientProxy *, const std::string &)>;

SendFn captureSend(std::vector<SentLine> &sent)
{
  return [&sent](BaseClientProxy *client, const std::string &line) { sent.push_back({client, line}); };
}

} // namespace

void VirtualHostTrackerTests::connectsOnRemoteFocus()
{
  VirtualHostTracker tracker;
  char primary{};
  char remote{};

  std::vector<SentLine> sent;
  tracker.setConnectLine(R"({"type":"connect","device":"mouse"})");
  tracker.onFocusChange(
      reinterpret_cast<BaseClientProxy *>(&remote), reinterpret_cast<BaseClientProxy *>(&primary), "remote", captureSend(sent)
  );

  QCOMPARE(sent.size(), static_cast<size_t>(2));
  QCOMPARE(sent[0].client, &remote);
  QCOMPARE(sent[0].line, R"({"type":"connect","device":"mouse"})");
  QCOMPARE(sent[1].client, &remote);
  QCOMPARE(sent[1].line, VirtualHostTracker::focusLine("remote", true));
  QCOMPARE(tracker.host(), reinterpret_cast<BaseClientProxy *>(&remote));
}

void VirtualHostTrackerTests::movesVirtualHostWhenFocusChanges()
{
  VirtualHostTracker tracker;
  char primary{};
  char remoteA{};
  char remoteB{};

  std::vector<SentLine> sent;
  tracker.setConnectLine(R"({"type":"connect"})");
  tracker.onFocusChange(
      reinterpret_cast<BaseClientProxy *>(&remoteA), reinterpret_cast<BaseClientProxy *>(&primary), "remote", captureSend(sent)
  );
  tracker.onFocusChange(
      reinterpret_cast<BaseClientProxy *>(&remoteB), reinterpret_cast<BaseClientProxy *>(&primary), "remote", captureSend(sent)
  );

  // A: connect + focus(here); then on the switch: A gets focus(away),
  // B gets connect (first time) + focus(here). Never a disconnect.
  QCOMPARE(sent.size(), static_cast<size_t>(5));
  QCOMPARE(sent[2].client, &remoteA);
  QCOMPARE(sent[2].line, VirtualHostTracker::focusLine("remote", false));
  QCOMPARE(sent[3].client, &remoteB);
  QCOMPARE(sent[3].line, R"({"type":"connect"})");
  QCOMPARE(sent[4].client, &remoteB);
  QCOMPARE(sent[4].line, VirtualHostTracker::focusLine("remote", true));
  QCOMPARE(tracker.host(), reinterpret_cast<BaseClientProxy *>(&remoteB));

  // Back to A: it is already attached, so only the focus notice travels.
  tracker.onFocusChange(
      reinterpret_cast<BaseClientProxy *>(&remoteA), reinterpret_cast<BaseClientProxy *>(&primary), "remote",
      captureSend(sent)
  );
  QCOMPARE(sent.size(), static_cast<size_t>(7));
  QCOMPARE(sent[5].client, &remoteB);
  QCOMPARE(sent[5].line, VirtualHostTracker::focusLine("remote", false));
  QCOMPARE(sent[6].client, &remoteA);
  QCOMPARE(sent[6].line, VirtualHostTracker::focusLine("remote", true));
  for (const auto &entry : sent) {
    QVERIFY(entry.line != VirtualHostTracker::kDefaultDisconnect);
  }
}

void VirtualHostTrackerTests::skipsConnectOnPrimaryFocus()
{
  VirtualHostTracker tracker;
  char primary{};

  std::vector<SentLine> sent;
  tracker.setConnectLine(R"({"type":"connect"})");
  tracker.onFocusChange(
      reinterpret_cast<BaseClientProxy *>(&primary), reinterpret_cast<BaseClientProxy *>(&primary), "remote", captureSend(sent)
  );

  QVERIFY(sent.empty());
  QCOMPARE(tracker.host(), nullptr);
}

void VirtualHostTrackerTests::clearHostIfClearsMatchingHost()
{
  VirtualHostTracker tracker;
  char primary{};
  char remote{};

  std::vector<SentLine> sent;
  tracker.setConnectLine(R"({"type":"connect"})");
  tracker.onFocusChange(
      reinterpret_cast<BaseClientProxy *>(&remote), reinterpret_cast<BaseClientProxy *>(&primary), "remote", captureSend(sent)
  );
  tracker.clearHostIf(reinterpret_cast<BaseClientProxy *>(&remote));
  QCOMPARE(tracker.host(), nullptr);
}

void VirtualHostTrackerTests::detachSendsDisconnectAndClearsHost()
{
  VirtualHostTracker tracker;
  char primary{};
  char remote{};

  std::vector<SentLine> sent;
  tracker.setConnectLine(R"({"type":"connect"})");
  tracker.onFocusChange(
      reinterpret_cast<BaseClientProxy *>(&remote), reinterpret_cast<BaseClientProxy *>(&primary), "remote", captureSend(sent)
  );
  tracker.detach(captureSend(sent), R"({"type":"custom-disconnect"})");

  QCOMPARE(sent.size(), static_cast<size_t>(3));
  QCOMPARE(sent[2].line, R"({"type":"custom-disconnect"})");
  QCOMPARE(sent[2].client, &remote);
  QCOMPARE(tracker.host(), nullptr);
  QVERIFY(!tracker.isAttached(reinterpret_cast<BaseClientProxy *>(&remote)));
}

void VirtualHostTrackerTests::hostsActiveClientMatchesRelayTarget()
{
  VirtualHostTracker tracker;
  char primary{};
  char remote{};

  std::vector<SentLine> sent;
  tracker.setConnectLine(R"({"type":"connect"})");
  tracker.onFocusChange(
      reinterpret_cast<BaseClientProxy *>(&remote), reinterpret_cast<BaseClientProxy *>(&primary), "remote", captureSend(sent)
  );

  QVERIFY(tracker.hostsActiveClient(reinterpret_cast<BaseClientProxy *>(&remote)));
  QVERIFY(!tracker.hostsActiveClient(reinterpret_cast<BaseClientProxy *>(&primary)));
}

void VirtualHostTrackerTests::connectPayloadOverrideBypassesCachedLine()
{
  VirtualHostTracker tracker;
  char primary{};
  char remote{};

  std::vector<SentLine> sent;
  tracker.setConnectLine(R"({"type":"connect","cached":true})");
  tracker.onFocusChange(
      reinterpret_cast<BaseClientProxy *>(&remote), reinterpret_cast<BaseClientProxy *>(&primary), "remote", captureSend(sent),
      R"({"type":"connect","override":true})"
  );

  QCOMPARE(sent.size(), static_cast<size_t>(2));
  QCOMPARE(sent[0].line, R"({"type":"connect","override":true})");
}

void VirtualHostTrackerTests::primaryFocusSendsFocusAwayNotDisconnect()
{
  VirtualHostTracker tracker;
  char primary{};
  char remote{};

  std::vector<SentLine> sent;
  tracker.setConnectLine(R"({"type":"connect"})");
  tracker.onFocusChange(
      reinterpret_cast<BaseClientProxy *>(&remote), reinterpret_cast<BaseClientProxy *>(&primary), "remote",
      captureSend(sent)
  );
  tracker.onFocusChange(
      reinterpret_cast<BaseClientProxy *>(&primary), reinterpret_cast<BaseClientProxy *>(&primary), "primary",
      captureSend(sent)
  );

  QCOMPARE(sent.size(), static_cast<size_t>(3));
  QCOMPARE(sent[2].client, &remote);
  QCOMPARE(sent[2].line, VirtualHostTracker::focusLine("primary", false));
  QCOMPARE(tracker.host(), nullptr);
  QVERIFY(tracker.isAttached(reinterpret_cast<BaseClientProxy *>(&remote)));
}

void VirtualHostTrackerTests::changedConnectLineIsReannounced()
{
  VirtualHostTracker tracker;
  char primary{};
  char remote{};

  std::vector<SentLine> sent;
  tracker.setConnectLine(R"({"type":"connect","device":"a"})");
  tracker.onFocusChange(
      reinterpret_cast<BaseClientProxy *>(&remote), reinterpret_cast<BaseClientProxy *>(&primary), "remote",
      captureSend(sent)
  );
  tracker.setConnectLine(R"({"type":"connect","device":"b"})");
  tracker.onFocusChange(
      reinterpret_cast<BaseClientProxy *>(&remote), reinterpret_cast<BaseClientProxy *>(&primary), "remote",
      captureSend(sent)
  );

  QCOMPARE(sent.size(), static_cast<size_t>(4));
  QCOMPARE(sent[2].line, R"({"type":"connect","device":"b"})");
  QCOMPARE(sent[3].line, VirtualHostTracker::focusLine("remote", true));
}

QTEST_MAIN(VirtualHostTrackerTests)
