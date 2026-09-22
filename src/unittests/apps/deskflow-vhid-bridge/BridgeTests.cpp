/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "BridgeCore.h"

#include <QTest>

#include <chrono>
#include <cstring>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace vhid_bridge {

namespace {

//! IReportSink that records every report instead of posting it, with a
//! scripted caps-lock reader and no display/cursor.
class RecordingSink : public IReportSink
{
public:
  struct Kb
  {
    uint8_t mods;
    std::set<uint16_t> keys;
  };
  struct Pt
  {
    std::set<uint8_t> buttons;
    int8_t dx, dy, vw, hw;
  };
  std::vector<Kb> kb;
  std::vector<Pt> pt;
  int order = 0;      // total reports posted, for ordering assertions
  int lastKbOrder = 0;
  std::function<std::optional<bool>()> caps = [] { return std::optional<bool>{}; };
  int capsReads = 0;

  void post_keyboard(uint8_t modifier_bits, const std::set<uint16_t> &keys) override
  {
    kb.push_back({modifier_bits, keys});
    lastKbOrder = ++order;
  }
  void post_pointing(const std::set<uint8_t> &buttons, int8_t dx, int8_t dy, int8_t vw, int8_t hw) override
  {
    pt.push_back({buttons, dx, dy, vw, hw});
    ++order;
  }
  CapsTruth caps_lock_state() override
  {
    ++capsReads;
    return {caps(), "test-reader"};
  }
  std::optional<CursorPoint> cursor_position() override
  {
    return std::nullopt;
  }
  bool main_display(int16_t &, int16_t &, double &) override
  {
    return false;
  }

  int capsEdges() const
  {
    int n = 0;
    for (const auto &r : kb) {
      n += r.keys.contains(bridge_logic::kUsageCapsLock) ? 1 : 0;
    }
    return n;
  }
};

std::vector<uint8_t> key_body(const char (&code)[4], uint16_t id, uint16_t mask, uint16_t button)
{
  std::vector<uint8_t> b(code, code + 4);
  append_be16(b, static_cast<int16_t>(id));
  append_be16(b, static_cast<int16_t>(mask));
  append_be16(b, static_cast<int16_t>(button));
  return b;
}

std::vector<uint8_t> frame(const std::vector<uint8_t> &body)
{
  std::vector<uint8_t> f;
  append_be32(f, static_cast<uint32_t>(body.size()));
  f.insert(f.end(), body.begin(), body.end());
  return f;
}

//! Captures log_line output for the duration of the scope.
struct LogCapture
{
  std::vector<std::string> lines;
  LogCapture()
  {
    set_log_sink([this](const std::string &line) { lines.push_back(line); });
  }
  ~LogCapture()
  {
    set_log_sink(nullptr);
  }
  bool any(const std::string &needle) const
  {
    for (const auto &l : lines) {
      if (l.find(needle) != std::string::npos) {
        return true;
      }
    }
    return false;
  }
  // The message part (after the "[bridge] " prefix) of every line, joined.
  std::string messages() const
  {
    std::string out;
    for (const auto &l : lines) {
      const size_t at = l.find("[bridge] ");
      out += (at == std::string::npos ? l : l.substr(at + 9));
    }
    return out;
  }
};

constexpr uint16_t kUsageK = 0x0e, kUsage1 = 0x1e, kUsageEsc = 0x29, kUsageA = 0x04;
constexpr uint16_t kKeyIdShiftL = 0xEFE1, kKeyIdEscape = 0xEF1B;

} // namespace

//! Drives the real Bridge through a RecordingSink (K4 audit batch 1).
class BridgeTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void init();
  void rolloverUnderServerCapsDoesNotLeakDerivedShift();
  void shiftReleaseMidHoldDropsDerivedShift();
  void escapeBurstReleasesBeforeStop();
  void cheapSpecialKeysAreMapped();
  void unmappedKeyPostsNothing();

private:
  struct Fixture
  {
    RecordingSink sink;
    Bridge bridge;
    Clock::time_point now{std::chrono::seconds(1000)};
    Fixture() : bridge(sink, "test", 1920, 1080, 4.0, /*scale_fixed=*/true)
    {
      bridge.now_ = [this] { return now; };
      sink.kb.clear();
      sink.pt.clear();
      sink.order = 0;
    }
    bool keyDown(uint16_t id, uint16_t mask, uint16_t button)
    {
      return bridge.on_key_down(key_body(proto::kKeyDown, id, mask, button));
    }
    bool keyUp(uint16_t id, uint16_t mask, uint16_t button)
    {
      return bridge.on_key_up(key_body(proto::kKeyUp, id, mask, button));
    }
  };
};

void BridgeTests::init()
{
  g_debug_keys = false;
  g_stop.store(false);
}

// A-1: with the server's Caps on, `k` needs a derived Shift (caps+shift
// composes lowercase). That Shift belongs to the `k` report only: rolling
// over to `1` while `k` is still held must not type `!`.
void BridgeTests::rolloverUnderServerCapsDoesNotLeakDerivedShift()
{
  Fixture f;
  f.sink.caps = [] { return std::optional<bool>{true}; }; // local caps on, agrees with the server
  QVERIFY(f.keyDown('k', bridge_logic::kMaskCapsLock, 10));
  QVERIFY(!f.sink.kb.empty());
  QVERIFY((f.sink.kb.back().mods & bridge_logic::kHidLeftShift) != 0);
  QVERIFY(f.sink.kb.back().keys.contains(kUsageK));
  QCOMPARE(f.sink.capsEdges(), 0);

  QVERIFY(f.keyDown('1', bridge_logic::kMaskCapsLock, 11));
  const auto &r = f.sink.kb.back();
  QCOMPARE(int(r.mods & bridge_logic::kHidLeftShift), 0);
  QVERIFY(r.keys.contains(kUsageK));
  QVERIFY(r.keys.contains(kUsage1));

  // releasing `1` re-posts the ledger: still no Shift on the held `k`
  QVERIFY(f.keyUp('1', bridge_logic::kMaskCapsLock, 11));
  QCOMPARE(int(f.sink.kb.back().mods), 0);
  QCOMPARE(f.sink.kb.back().keys, std::set<uint16_t>{kUsageK});
  QVERIFY(f.keyUp('k', bridge_logic::kMaskCapsLock, 10));
  QCOMPARE(int(f.sink.kb.back().mods), 0);
  QVERIFY(f.sink.kb.back().keys.empty());
}

// A-6 (same root cause): the server releases its real Shift while `K` is
// still held (auto-repeat continues as `k` there). The next report must
// drop Shift so the repeated letter changes case with the server.
void BridgeTests::shiftReleaseMidHoldDropsDerivedShift()
{
  Fixture f;
  f.sink.caps = [] { return std::optional<bool>{false}; };
  QVERIFY(f.keyDown(kKeyIdShiftL, bridge_logic::kMaskShift, 20));
  QCOMPARE(int(f.sink.kb.back().mods), int(bridge_logic::kHidLeftShift));
  QVERIFY(f.keyDown('K', bridge_logic::kMaskShift, 10));
  QVERIFY((f.sink.kb.back().mods & bridge_logic::kHidLeftShift) != 0);
  QVERIFY(f.sink.kb.back().keys.contains(kUsageK));

  QVERIFY(f.keyUp(kKeyIdShiftL, 0, 20));
  QCOMPARE(int(f.sink.kb.back().mods & bridge_logic::kHidLeftShift), 0);
  QCOMPARE(f.sink.kb.back().keys, std::set<uint16_t>{kUsageK});

  // a repeat posts nothing; the key up empties the report
  const size_t before = f.sink.kb.size();
  QVERIFY(f.bridge.on_key_repeat(key_body(proto::kKeyRepeat, 'k', 0, 10)));
  QCOMPARE(f.sink.kb.size(), before);
  QVERIFY(f.keyUp('k', 0, 10));
  QVERIFY(f.sink.kb.back().keys.empty());
}

// A-4: the 4x Esc rescue must not exit the process before the release
// report is posted. run() ends the connection, posts the empty keyboard
// and pointing reports, and returns so main can unwind (sink destroyed
// after the release was queued).
void BridgeTests::escapeBurstReleasesBeforeStop()
{
  Fixture f;
  int fds[2];
  QCOMPARE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  // server hello + four Esc downs, all already in the buffer
  std::vector<uint8_t> hello(proto::kGreeting, proto::kGreeting + sizeof(proto::kGreeting));
  append_be16(hello, proto::kMajorVersion);
  append_be16(hello, proto::kMinorVersion);
  std::vector<uint8_t> stream = frame(hello);
  for (int i = 0; i < 4; ++i) {
    const auto body = frame(key_body(proto::kKeyDown, kKeyIdEscape, 0, 53));
    stream.insert(stream.end(), body.begin(), body.end());
  }
  QCOMPARE(::write(fds[1], stream.data(), stream.size()), ssize_t(stream.size()));

  FramedSocket socket(fds[0]);
  f.bridge.run(socket); // returns instead of std::exit()
  ::close(fds[1]);

  QVERIFY(g_stop.load());
  // the 4th Esc down was posted, then the release (empty) report
  size_t escAt = f.sink.kb.size();
  for (size_t i = 0; i < f.sink.kb.size(); ++i) {
    if (f.sink.kb[i].keys.contains(kUsageEsc)) {
      escAt = i;
    }
  }
  QVERIFY(escAt < f.sink.kb.size());
  QVERIFY(escAt + 1 < f.sink.kb.size());
  QCOMPARE(int(f.sink.kb.back().mods), 0);
  QVERIFY(f.sink.kb.back().keys.empty());
  QVERIFY(!f.sink.pt.empty());
  QVERIFY(f.sink.pt.back().buttons.empty());
  QCOMPARE(int(f.sink.pt.back().dx), 0);
  QVERIFY(f.bridge.held_keys_.empty());
  g_stop.store(false);
}

// A-7: the cheap extra mappings.
void BridgeTests::cheapSpecialKeysAreMapped()
{
  QCOMPARE(special_keyid_to_usage(0xEF8D), std::optional<uint16_t>{0x28}); // KP_Enter -> Return
  QCOMPARE(special_keyid_to_usage(0xEFB0), std::optional<uint16_t>{0x27}); // KP_0 -> '0'
  QCOMPARE(special_keyid_to_usage(0xEFB1), std::optional<uint16_t>{0x1e}); // KP_1 -> '1'
  QCOMPARE(special_keyid_to_usage(0xEFB9), std::optional<uint16_t>{0x26}); // KP_9 -> '9'
  QCOMPARE(special_keyid_to_usage(0xEFAE), std::optional<uint16_t>{0x37}); // KP_Decimal -> '.'
  QCOMPARE(special_keyid_to_usage(0xEFBE), std::optional<uint16_t>{0x3a}); // F1
  QCOMPARE(special_keyid_to_usage(0xEFC9), std::optional<uint16_t>{0x45}); // F12
  QCOMPARE(special_keyid_to_usage(0xEFCA), std::optional<uint16_t>{0x68}); // F13
  QCOMPARE(special_keyid_to_usage(0xEFD5), std::optional<uint16_t>{0x73}); // F24
  QCOMPARE(special_keyid_to_usage(0xEF63), std::optional<uint16_t>{0x49}); // Insert
  QCOMPARE(special_keyid_to_usage(0xEF08), std::optional<uint16_t>{0x2a}); // BackSpace (unchanged)
  QVERIFY(!special_keyid_to_usage(0xEFAB).has_value());                     // KP_Add: documented gap
}

// A-7: non-ASCII / dead keys are the documented gap: nothing is posted and
// nothing about the key is logged at the default level.
void BridgeTests::unmappedKeyPostsNothing()
{
  Fixture f;
  LogCapture log;
  QVERIFY(f.keyDown(0x00E9, 0, 30)); // 'é'
  QVERIFY(f.keyDown(0x0300, 0, 31)); // combining grave (dead key)
  QVERIFY(f.sink.kb.empty());
  QVERIFY(f.bridge.held_keys_.empty());
  QVERIFY(log.lines.empty());
  g_debug_keys = true;
  QVERIFY(f.keyDown(0x00E9, 0, 30));
  QVERIFY(log.any("unmapped key"));
  QVERIFY(f.sink.kb.empty());
  g_debug_keys = false;
}

} // namespace vhid_bridge

QTEST_MAIN(vhid_bridge::BridgeTests)

#include "BridgeTests.moc"
