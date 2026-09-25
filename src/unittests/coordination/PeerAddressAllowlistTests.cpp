/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "PeerAddressAllowlistTests.h"

#include "arch/Arch.h"
#include "base/Log.h"
#include "coordination/PeerAddressAllowlist.h"

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using deskflow::coordination::PeerAddressAllowlist;

namespace {

Log g_log;
std::unique_ptr<Arch> g_arch;

//! Scripted resolver: records every lookup, answers from a table.
struct FakeResolver
{
  std::mutex mutex;
  std::map<std::string, std::vector<std::string>> table;
  std::vector<std::string> lookups;

  PeerAddressAllowlist::Resolver fn()
  {
    return [this](const std::string &host) {
      std::scoped_lock lock{mutex};
      lookups.push_back(host);
      const auto it = table.find(host);
      return it == table.end() ? std::vector<std::string>{} : it->second;
    };
  }

  size_t lookupCount()
  {
    std::scoped_lock lock{mutex};
    return lookups.size();
  }
};

template <typename Condition> bool waitFor(Condition condition, int timeoutMs)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (!condition()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

} // namespace

void PeerAddressAllowlistTests::initTestCase()
{
  g_arch = std::make_unique<Arch>();
  g_log.setFilter(LogLevel::Level::Error);
}

void PeerAddressAllowlistTests::cleanupTestCase()
{
  g_arch.reset();
}

void PeerAddressAllowlistTests::normalize_canonicalizesLiterals()
{
  QCOMPARE(PeerAddressAllowlist::normalize("10.0.0.7"), std::string("10.0.0.7"));
  QCOMPARE(PeerAddressAllowlist::normalize("::ffff:10.0.0.7"), std::string("10.0.0.7"));
  QCOMPARE(PeerAddressAllowlist::normalize("[fe80::1%en0]"), std::string("fe80::1"));
  QCOMPARE(PeerAddressAllowlist::normalize("FE80:0:0:0:0:0:0:1"), std::string("fe80::1"));
  QVERIFY(PeerAddressAllowlist::normalize("hackintosh.lan").empty());
  QVERIFY(PeerAddressAllowlist::normalize("").empty());
  QVERIFY(PeerAddressAllowlist::normalize("10.0.0").empty());
}

void PeerAddressAllowlistTests::literals_allowedImmediately()
{
  FakeResolver resolver;
  // Literal entries need no resolver call at all.
  PeerAddressAllowlist allowlist({"10.0.0.7", "fd7a:115c::7", "", "10.0.0.7"}, resolver.fn());
  QVERIFY(allowlist.allows("10.0.0.7"));
  QVERIFY(allowlist.allows("::ffff:10.0.0.7"));
  QVERIFY(allowlist.allows("fd7a:115c::7"));
  QVERIFY(allowlist.allows("fd7a:115c:0:0:0:0:0:7"));
  QVERIFY(allowlist.hosts().empty());
  allowlist.start(0.0);
  QCOMPARE(resolver.lookupCount(), static_cast<size_t>(0));
}

void PeerAddressAllowlistTests::unknown_denied()
{
  FakeResolver resolver;
  PeerAddressAllowlist allowlist({"10.0.0.7"}, resolver.fn());
  QVERIFY(!allowlist.allows("10.0.0.8"));
  QVERIFY(!allowlist.allows(""));
  QVERIFY(!allowlist.allows("garbage"));
  QVERIFY(!allowlist.allows("::1"));
}

void PeerAddressAllowlistTests::names_resolvedOnRefresh()
{
  FakeResolver resolver;
  resolver.table["hackintosh.lan"] = {"10.0.0.7"};
  resolver.table["hackintosh.tail1234.ts.net"] = {"100.64.0.7", "fd7a:115c:a1e0::7"};
  PeerAddressAllowlist allowlist({"hackintosh.lan", "hackintosh.tail1234.ts.net", "10.0.0.9"}, resolver.fn());
  QCOMPARE(allowlist.hosts().size(), static_cast<size_t>(2));
  // Names are unknown until resolved; the literal is known at once.
  QVERIFY(!allowlist.allows("10.0.0.7"));
  QVERIFY(allowlist.allows("10.0.0.9"));

  allowlist.refreshNow(0.0);
  QVERIFY(allowlist.allows("10.0.0.7"));
  QVERIFY(allowlist.allows("100.64.0.7"));
  QVERIFY(allowlist.allows("fd7a:115c:a1e0::7"));
  QVERIFY(allowlist.allows("::ffff:100.64.0.7"));
  QVERIFY(!allowlist.allows("10.0.0.8"));
  QCOMPARE(allowlist.snapshot().size(), static_cast<size_t>(4));

  // The background path: start() resolves on its own thread.
  FakeResolver background;
  background.table["tiny11.lan"] = {"10.0.0.11"};
  PeerAddressAllowlist async({"tiny11.lan"}, background.fn());
  QVERIFY(!async.allows("10.0.0.11"));
  async.start(0.0);
  QVERIFY(waitFor([&async] { return async.allows("10.0.0.11"); }, 2000));
  async.stop();
}

void PeerAddressAllowlistTests::miss_schedulesEarlyRefreshRateLimited()
{
  FakeResolver resolver;
  PeerAddressAllowlist allowlist({"hackintosh.lan"}, resolver.fn());
  allowlist.refreshNow(0.0);
  QCOMPARE(resolver.lookupCount(), static_cast<size_t>(1));

  // Not due yet: the periodic refresh is every kRefreshS.
  allowlist.refreshIfDue(1.0);
  QCOMPARE(resolver.lookupCount(), static_cast<size_t>(1));

  // A miss right after a refresh is rate-limited (kMissRefreshMinGapS)...
  allowlist.noteMiss(1.0);
  allowlist.refreshIfDue(1.0);
  QCOMPARE(resolver.lookupCount(), static_cast<size_t>(1));

  // ...a miss later triggers the early re-resolve on the next tick, and the
  // peer's new address is then accepted.
  resolver.table["hackintosh.lan"] = {"10.0.0.77"};
  allowlist.noteMiss(PeerAddressAllowlist::kMissRefreshMinGapS + 1.0);
  allowlist.refreshIfDue(PeerAddressAllowlist::kMissRefreshMinGapS + 1.0);
  QVERIFY(waitFor([&allowlist] { return allowlist.allows("10.0.0.77"); }, 2000));
  QCOMPARE(resolver.lookupCount(), static_cast<size_t>(2));

  // The periodic refresh fires once kRefreshS has elapsed.
  allowlist.refreshIfDue(PeerAddressAllowlist::kMissRefreshMinGapS + 1.0 + PeerAddressAllowlist::kRefreshS);
  QVERIFY(waitFor([&resolver] { return resolver.lookupCount() == 3; }, 2000));
  allowlist.stop();
}

void PeerAddressAllowlistTests::emptyResolveKeepsLastGoodSet()
{
  FakeResolver resolver;
  resolver.table["hackintosh.lan"] = {"10.0.0.7"};
  PeerAddressAllowlist allowlist({"hackintosh.lan"}, resolver.fn());
  allowlist.refreshNow(0.0);
  QVERIFY(allowlist.allows("10.0.0.7"));
  // A DNS blip must not lock every peer out until the next refresh.
  resolver.table.clear();
  allowlist.refreshNow(PeerAddressAllowlist::kRefreshS);
  QVERIFY(allowlist.allows("10.0.0.7"));
}

QTEST_MAIN(PeerAddressAllowlistTests)
