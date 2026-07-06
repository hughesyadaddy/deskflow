/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "WakeOnLanTests.h"

#include "arch/Arch.h"
#include "base/Log.h"
#include "coordination/WakeOnLan.h"

#include <QTest>

#include <memory>

using deskflow::coordination::buildMagicPacket;
using deskflow::coordination::MacAddress;
using deskflow::coordination::parseMacAddress;

namespace {
// sendWakeOnLan logs; the Log/Arch singletons must exist.
Log g_log;
std::unique_ptr<Arch> g_arch;
} // namespace

void WakeOnLanTests::initTestCase()
{
  g_arch = std::make_unique<Arch>();
  g_log.setFilter(LogLevel::Level::Error);
}

void WakeOnLanTests::cleanupTestCase()
{
  g_arch.reset();
}

void WakeOnLanTests::parseMac_colonSeparated()
{
  const auto mac = parseMacAddress("bc:24:11:aA:Bb:0f");
  QVERIFY(mac.has_value());
  const MacAddress expected{0xBC, 0x24, 0x11, 0xAA, 0xBB, 0x0F};
  QCOMPARE(*mac, expected);
}

void WakeOnLanTests::parseMac_dashSeparated()
{
  const auto mac = parseMacAddress("00-11-22-33-44-55");
  QVERIFY(mac.has_value());
  const MacAddress expected{0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
  QCOMPARE(*mac, expected);
}

void WakeOnLanTests::parseMac_rejectsMalformed()
{
  QVERIFY(!parseMacAddress("").has_value());
  QVERIFY(!parseMacAddress("bc:24:11:aa:bb").has_value());       // five octets
  QVERIFY(!parseMacAddress("bc:24:11:aa:bb:cc:dd").has_value()); // seven octets
  QVERIFY(!parseMacAddress("bc.24.11.aa.bb.cc").has_value());    // wrong separator
  QVERIFY(!parseMacAddress("bc:24:11:aa:bb:zz").has_value());    // non-hex
  QVERIFY(!parseMacAddress("bc:24:11:aa:bb:c").has_value());     // short octet
  QVERIFY(!parseMacAddress("ssh proxmox wake").has_value());     // arbitrary text
}

void WakeOnLanTests::magicPacket_layout()
{
  const MacAddress mac{0xBC, 0x24, 0x11, 0xAA, 0xBB, 0x0F};
  const auto packet = buildMagicPacket(mac);

  QCOMPARE(packet.size(), static_cast<size_t>(102));
  for (size_t i = 0; i < 6; ++i) {
    QCOMPARE(packet[i], static_cast<uint8_t>(0xFF));
  }
  for (size_t repeat = 0; repeat < 16; ++repeat) {
    for (size_t octet = 0; octet < mac.size(); ++octet) {
      QCOMPARE(packet[6 + repeat * 6 + octet], mac[octet]);
    }
  }
}

void WakeOnLanTests::sendWakeOnLan_rejectsInvalidMac()
{
  // The only network-free branch: an unparseable MAC fails fast.
  QVERIFY(!deskflow::coordination::sendWakeOnLan("invalid-mac"));
}

QTEST_MAIN(WakeOnLanTests)
