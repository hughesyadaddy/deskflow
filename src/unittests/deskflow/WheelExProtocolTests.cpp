/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "WheelExProtocolTests.h"

#include "arch/Arch.h"
#include "base/Log.h"
#include "common/Settings.h"
#include "deskflow/ISecondaryScreen.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "io/IStream.h"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <cstring>
#include <memory>
#include <vector>

namespace {

class MemoryStream : public deskflow::IStream
{
public:
  void close() override
  {
  }
  uint32_t read(void *buffer, uint32_t n) override
  {
    const uint32_t avail = static_cast<uint32_t>(m_data.size() - m_readPos);
    const uint32_t count = n < avail ? n : avail;
    memcpy(buffer, m_data.data() + m_readPos, count);
    m_readPos += count;
    return count;
  }
  void write(const void *buffer, uint32_t n) override
  {
    const auto *bytes = static_cast<const uint8_t *>(buffer);
    m_data.insert(m_data.end(), bytes, bytes + n);
  }
  void flush() override
  {
  }
  void shutdownInput() override
  {
  }
  void shutdownOutput() override
  {
  }
  void *getEventTarget() const override
  {
    return const_cast<MemoryStream *>(this);
  }
  bool isReady() const override
  {
    return m_readPos < m_data.size();
  }
  uint32_t getSize() const override
  {
    return static_cast<uint32_t>(m_data.size() - m_readPos);
  }

  std::vector<uint8_t> m_data;
  size_t m_readPos = 0;
};

class BankingSecondary : public ISecondaryScreen
{
public:
  void fakeMouseButton(ButtonID, bool) override
  {
  }
  void fakeMouseMove(int32_t, int32_t) override
  {
  }
  void fakeMouseRelativeMove(int32_t, int32_t) const override
  {
  }
  void fakeMouseWheel(ScrollDelta delta) const override
  {
    m_wheels.push_back(delta);
  }
  using ISecondaryScreen::applyScrollModifierFixed;

  mutable std::vector<ScrollDelta> m_wheels;
};

std::unique_ptr<Arch> g_arch;
Log g_log;
std::unique_ptr<QCoreApplication> g_app;
std::unique_ptr<QTemporaryDir> g_settingsDir;

WheelEx lines(double x, double y)
{
  WheelEx ex;
  ex.xDelta = static_cast<int32_t>(x * kScrollFixedOne);
  ex.yDelta = static_cast<int32_t>(y * kScrollFixedOne);
  return ex;
}

} // namespace

void WheelExProtocolTests::initTestCase()
{
  static int argc = 1;
  static char arg0[] = "WheelExProtocolTests";
  static char *argv[] = {arg0, nullptr};
  g_app = std::make_unique<QCoreApplication>(argc, argv);
  g_arch = std::make_unique<Arch>();
  g_log.setFilter(LogLevel::Level::Error);
  // ISecondaryScreen reads the scroll prefs; keep that off the real config
  g_settingsDir = std::make_unique<QTemporaryDir>();
  QVERIFY(g_settingsDir->isValid());
  Settings::setSettingsFile(g_settingsDir->filePath(QStringLiteral("Deskflow.conf")));
  QVERIFY(Settings::settingsFile().startsWith(g_settingsDir->path()));
}

void WheelExProtocolTests::cleanupTestCase()
{
  g_app.reset();
  g_arch.reset();
  g_settingsDir.reset();
}

void WheelExProtocolTests::negotiatedMinor_clampsToServer()
{
  QCOMPARE(kProtocolMinorVersion, 9);
  QCOMPARE(negotiatedProtocolMinor(1, 8), 8);
  QCOMPARE(negotiatedProtocolMinor(1, 9), 9);
  QCOMPARE(negotiatedProtocolMinor(1, 12), 9);
  QCOMPARE(negotiatedProtocolMinor(1, 0), 0);
  QCOMPARE(negotiatedProtocolMinor(2, 3), 9);
}

void WheelExProtocolTests::fixedToNotchUnits_roundsToNearest()
{
  QCOMPARE(WheelEx::fixedToNotchUnits(kScrollFixedOne, false), 120);
  QCOMPARE(WheelEx::fixedToNotchUnits(-kScrollFixedOne, false), -120);
  QCOMPARE(WheelEx::fixedToNotchUnits(kScrollFixedOne / 2, false), 60);
  QCOMPARE(WheelEx::fixedToNotchUnits(6554, false), 12);
  // 273/65536 lines = 0.49988 units -> 0, 274 = 0.5017 -> 1, symmetric for negatives
  QCOMPARE(WheelEx::fixedToNotchUnits(273, false), 0);
  QCOMPARE(WheelEx::fixedToNotchUnits(274, false), 1);
  QCOMPARE(WheelEx::fixedToNotchUnits(-273, false), 0);
  QCOMPARE(WheelEx::fixedToNotchUnits(-274, false), -1);
  QCOMPARE(WheelEx::fixedToNotchUnits(INT32_MAX, false), 3932160);
  QCOMPARE(WheelEx::fixedToNotchUnits(INT32_MIN, false), -3932160);
}

void WheelExProtocolTests::fixedToNotchUnits_continuousUsesPixelsPerLine()
{
  QCOMPARE(WheelEx::fixedToNotchUnits(10 * kScrollFixedOne, true), 120);
  QCOMPARE(WheelEx::fixedToNotchUnits(kScrollFixedOne, true), 12);
  QCOMPARE(WheelEx::fixedToNotchUnits(-3 * kScrollFixedOne, true), -36);
}

void WheelExProtocolTests::fromNotches_roundTripsLegacyDeltas()
{
  const WheelEx ex = WheelEx::fromNotches(120, -240);
  QCOMPARE(ex.xDelta, kScrollFixedOne);
  QCOMPARE(ex.yDelta, -2 * kScrollFixedOne);
  QVERIFY(!ex.continuous);
  QCOMPARE(ex.phase, ScrollPhase::None);
  QCOMPARE(ex.momentum, MomentumPhase::None);
  for (int32_t units = -32768; units <= 32767; ++units) {
    if (WheelEx::fixedToNotchUnits(WheelEx::notchUnitsToFixed(units), false) != units) {
      QFAIL(qPrintable(QStringLiteral("legacy delta %1 does not survive DMWX").arg(units)));
    }
  }
}

void WheelExProtocolTests::isEmpty_phaseMarkersAreNotEmpty()
{
  WheelEx ex;
  QVERIFY(ex.isEmpty());
  ex.continuous = true;
  ex.timestampMs = 42;
  QVERIFY(ex.isEmpty());
  ex.phase = ScrollPhase::Ended;
  QVERIFY(!ex.isEmpty());
  ex.phase = ScrollPhase::None;
  ex.momentum = MomentumPhase::Ended;
  QVERIFY(!ex.isEmpty());
  ex.momentum = MomentumPhase::None;
  ex.yDelta = 1;
  QVERIFY(!ex.isEmpty());
}

void WheelExProtocolTests::takeWholeNotches_keepsRemainder()
{
  int32_t bank = 119;
  QCOMPARE(WheelEx::takeWholeNotches(bank), 0);
  QCOMPARE(bank, 119);
  bank = 130;
  QCOMPARE(WheelEx::takeWholeNotches(bank), 120);
  QCOMPARE(bank, 10);
  bank = 250;
  QCOMPARE(WheelEx::takeWholeNotches(bank), 240);
  QCOMPARE(bank, 10);
  bank = -144;
  QCOMPARE(WheelEx::takeWholeNotches(bank), -120);
  QCOMPARE(bank, -24);
  bank = -119;
  QCOMPARE(WheelEx::takeWholeNotches(bank), 0);
  QCOMPARE(bank, -119);
}

void WheelExProtocolTests::fromWire_sanitizesPhaseBytes()
{
  const WheelEx good = WheelEx::fromWire(1, -2, 1, 5, 3, 77);
  QCOMPARE(good.xDelta, 1);
  QCOMPARE(good.yDelta, -2);
  QVERIFY(good.continuous);
  QCOMPARE(good.phase, ScrollPhase::MayBegin);
  QCOMPARE(good.momentum, MomentumPhase::Ended);
  QCOMPARE(good.timestampMs, 77u);

  const WheelEx junk = WheelEx::fromWire(0, 0, 7, 6, 4, 0);
  QVERIFY(junk.continuous);
  QCOMPARE(junk.phase, ScrollPhase::None);
  QCOMPARE(junk.momentum, MomentumPhase::None);
  QVERIFY(junk.isEmpty());
  QCOMPARE(WheelEx::fromWire(0, 0, 0, 255, 255, 0).phase, ScrollPhase::None);
  QCOMPARE(WheelEx::fromWire(0, 0, 0, 255, 255, 0).momentum, MomentumPhase::None);
}

void WheelExProtocolTests::dmwx_packsAndUnpacks()
{
  MemoryStream stream;
  const int32_t x = -123456789;
  const int32_t y = 98765;
  const uint32_t ts = 0xFFFFFFFFu;
  ProtocolUtil::writef(
      &stream, kMsgDMouseWheelEx, x, y, static_cast<uint32_t>(1), static_cast<uint32_t>(ScrollPhase::Changed),
      static_cast<uint32_t>(MomentumPhase::Ended), ts
  );

  QCOMPARE(stream.getSize(), 19u);
  char code[4];
  QCOMPARE(stream.read(code, 4), 4u);
  QCOMPARE(memcmp(code, kMsgDMouseWheelEx, 4), 0);

  int32_t rx = 0;
  int32_t ry = 0;
  uint8_t continuous = 0;
  uint8_t phase = 0;
  uint8_t momentum = 0;
  uint32_t rts = 0;
  ProtocolUtil::readf(&stream, kMsgDMouseWheelEx + 4, &rx, &ry, &continuous, &phase, &momentum, &rts);
  QCOMPARE(rx, x);
  QCOMPARE(ry, y);
  QCOMPARE(continuous, 1);
  QCOMPARE(static_cast<ScrollPhase>(phase), ScrollPhase::Changed);
  QCOMPARE(static_cast<MomentumPhase>(momentum), MomentumPhase::Ended);
  QCOMPARE(rts, ts);
  QCOMPARE(stream.getSize(), 0u);
}

void WheelExProtocolTests::dmwx_wireLengthIsFixed()
{
  QCOMPARE(QByteArray(kMsgDMouseWheelEx), QByteArray("DMWX%4i%4i%1i%1i%1i%4i"));
  QCOMPARE(QByteArray(kMsgDMouseWheel), QByteArray("DMWM%2i%2i"));
}

void WheelExProtocolTests::secondaryDefault_banksToWholeNotches()
{
  BankingSecondary screen;
  for (int i = 0; i < 3; ++i) {
    screen.fakeMouseWheelEx(lines(0, 0.25));
    QVERIFY(screen.m_wheels.empty());
  }
  screen.fakeMouseWheelEx(lines(0, 0.25));
  QCOMPARE(screen.m_wheels.size(), 1u);
  QCOMPARE(screen.m_wheels[0].x, 0);
  QCOMPARE(screen.m_wheels[0].y, 120);

  // only whole notches leave; the half notch waits for its other half
  screen.m_wheels.clear();
  screen.fakeMouseWheelEx(lines(1.5, 0));
  QCOMPARE(screen.m_wheels.size(), 1u);
  QCOMPARE(screen.m_wheels[0].x, 120);
  QCOMPARE(screen.m_wheels[0].y, 0);
  screen.fakeMouseWheelEx(lines(0.5, 0));
  QCOMPARE(screen.m_wheels.size(), 2u);
  QCOMPARE(screen.m_wheels[1].x, 120);

  screen.m_wheels.clear();
  WheelEx pixels = lines(0, 7);
  pixels.continuous = true;
  screen.fakeMouseWheelEx(pixels);
  QVERIFY(screen.m_wheels.empty());
  screen.fakeMouseWheelEx(pixels);
  QCOMPARE(screen.m_wheels.size(), 1u);
  QCOMPARE(screen.m_wheels[0].y, 120);
  screen.fakeMouseWheelEx(pixels);
  QCOMPARE(screen.m_wheels.size(), 2u);
  QCOMPARE(screen.m_wheels[1].y, 120);
}

void WheelExProtocolTests::secondaryDefault_dropsPhaseOnly()
{
  BankingSecondary screen;
  WheelEx marker;
  marker.continuous = true;
  marker.phase = ScrollPhase::Began;
  screen.fakeMouseWheelEx(marker);
  marker.phase = ScrollPhase::None;
  marker.momentum = MomentumPhase::Ended;
  screen.fakeMouseWheelEx(marker);
  QVERIFY(screen.m_wheels.empty());
}

void WheelExProtocolTests::secondaryDefault_negativeBankFlushesNegative()
{
  BankingSecondary screen;
  screen.fakeMouseWheelEx(lines(0, -0.5));
  screen.fakeMouseWheelEx(lines(0, 0.25));
  screen.fakeMouseWheelEx(lines(0, -0.75));
  QCOMPARE(screen.m_wheels.size(), 1u);
  QCOMPARE(screen.m_wheels[0].y, -120);
}

void WheelExProtocolTests::applyScrollModifierFixed_scalesWithoutTruncation()
{
  Settings::setValue(Settings::Client::YScrollScale, 0.5);
  Settings::setValue(Settings::Client::InvertXScroll, true);
  BankingSecondary screen;
  WheelEx ex = lines(0.25, 0.25);
  screen.applyScrollModifierFixed(ex);
  QCOMPARE(ex.yDelta, kScrollFixedOne / 8);
  QCOMPARE(ex.xDelta, -kScrollFixedOne / 4);
  Settings::setValue(Settings::Client::YScrollScale, 1.0);
  Settings::setValue(Settings::Client::InvertXScroll, false);
}

QTEST_MAIN(WheelExProtocolTests)
