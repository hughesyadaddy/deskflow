// SPDX-FileCopyrightText: (C) 2026 Deskflow Contributors
// SPDX-License-Identifier: MIT
//
// deskflow-vhid-bridge core: the Deskflow protocol reader, the input ledger
// and the report translation, split from the process (main, the Karabiner
// client, IOKit/CoreGraphics readers) so `Bridge` can be driven in a unit
// test through an IReportSink that records reports instead of posting them.
//
// Nothing here touches the Karabiner SDK, IOKit or CoreGraphics: everything
// the bridge needs from the machine (report posting, the caps-lock truth,
// the cursor position, the main display) comes through IReportSink.

#pragma once

#include "BridgeCalibration.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace vhid_bridge {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// ---------------------------------------------------------------------------
// Logging. Lines go to stderr (the LaunchDaemon log) with a local wall-clock
// prefix; set_log_sink() diverts them (tests capture what would be logged).
// ---------------------------------------------------------------------------
std::string log_timestamp();
void log_line(const std::string &message);
// nullptr restores stderr.
void set_log_sink(std::function<void(const std::string &line)> sink);

// --debug-keys: per-key held counts and the per-session case counters.
extern bool g_debug_keys;
void log_keys(const std::string &message);

// ---------------------------------------------------------------------------
// Process control shared by main, the socket reader and the signal handler.
// Self-pipe: the signal handler and the console-user watcher write a byte so
// a blocking read/poll/sleep wakes immediately instead of at the next message.
// ---------------------------------------------------------------------------
extern int g_wake_pipe[2];
extern std::atomic<bool> g_stop;
// True while a user session owns the console (not loginwindow): inject nothing.
extern std::atomic<bool> g_stand_down;

void wake();
void drain_wake_pipe();
bool must_pause();
// Sleeps up to timeout_ms; returns early (true) when woken by the self-pipe.
bool wait_or_wake(int timeout_ms);

// ---------------------------------------------------------------------------
// Deskflow/Barrier protocol constants (verified against deskflow ProtocolTypes).
// ---------------------------------------------------------------------------
namespace proto {
constexpr char kGreeting[7] = {'B', 'a', 'r', 'r', 'i', 'e', 'r'};
constexpr int16_t kMajorVersion = 1;
constexpr int16_t kMinorVersion = 8;
constexpr uint32_t kMaxMessageBytes = 1u << 20; // reject absurd framed lengths
constexpr uint16_t kDefaultPort = 24800;

// Codes the bridge acts on. Other messages (CIAK/CROP/DSOP/CSEC/CCLP/…) carry
// no input and fall through to a no-op in dispatch(), so they need no constant.
constexpr char kQueryInfo[4] = {'Q', 'I', 'N', 'F'};
constexpr char kKeepAlive[4] = {'C', 'A', 'L', 'V'};
constexpr char kNoop[4] = {'C', 'N', 'O', 'P'};
constexpr char kClose[4] = {'C', 'B', 'Y', 'E'};
constexpr char kEnter[4] = {'C', 'I', 'N', 'N'};
constexpr char kLeave[4] = {'C', 'O', 'U', 'T'};
constexpr char kInfo[4] = {'D', 'I', 'N', 'F'};
constexpr char kMouseMove[4] = {'D', 'M', 'M', 'V'};
constexpr char kMouseRelMove[4] = {'D', 'M', 'R', 'M'};
constexpr char kMouseDown[4] = {'D', 'M', 'D', 'N'};
constexpr char kMouseUp[4] = {'D', 'M', 'U', 'P'};
constexpr char kMouseWheel[4] = {'D', 'M', 'W', 'M'};
constexpr char kKeyDown[4] = {'D', 'K', 'D', 'N'};
constexpr char kKeyDownLang[4] = {'D', 'K', 'D', 'L'}; // v1.8 key-down w/ language
constexpr char kKeyUp[4] = {'D', 'K', 'U', 'P'};
constexpr char kKeyRepeat[4] = {'D', 'K', 'R', 'P'};

// Deskflow key-modifier mask bits (KeyTypes.h).
constexpr uint32_t kMaskShift = 0x0001;
constexpr uint32_t kMaskControl = 0x0002;
constexpr uint32_t kMaskAlt = 0x0004;
constexpr uint32_t kMaskMeta = 0x0008;
constexpr uint32_t kMaskSuper = 0x0010;
} // namespace proto

// ---------------------------------------------------------------------------
// Byte (de)serialization — bounds-checked, big-endian (network order).
// ---------------------------------------------------------------------------
class ByteReader
{
public:
  explicit ByteReader(const std::vector<uint8_t> &data) : data_(data)
  {
  }

  bool ok() const
  {
    return ok_;
  }
  size_t remaining() const
  {
    return data_.size() - pos_;
  }

  bool skip(size_t n);
  std::optional<uint8_t> u8();
  std::optional<int16_t> i16();

private:
  const std::vector<uint8_t> &data_;
  size_t pos_ = 0;
  bool ok_ = true;
};

void append_be16(std::vector<uint8_t> &out, int16_t value);
void append_be32(std::vector<uint8_t> &out, uint32_t value);
void append_bytes(std::vector<uint8_t> &out, const char *bytes, size_t n);
bool body_has_code(const std::vector<uint8_t> &body, const char (&code)[4]);

// A blocking connect() to a black-holed host (firewall drop, sleeping machine)
// stalls ~75s, defeating the reconnect backoff; a healthy link must also notice a
// silently-dead host (no RST) rather than blocking in recv() forever. Bound both.
constexpr int kConnectTimeoutMs = 4000;
constexpr int kIoTimeoutSeconds = 10; // > deskflow's 5s CALV keep-alive interval

// ---------------------------------------------------------------------------
// Framed socket I/O. Every message is a 4-byte big-endian length + payload.
// ---------------------------------------------------------------------------
class FramedSocket
{
public:
  explicit FramedSocket(int fd) : fd_(fd)
  {
  }
  FramedSocket(const FramedSocket &) = delete;
  FramedSocket &operator=(const FramedSocket &) = delete;
  ~FramedSocket()
  {
    close();
  }

  void close();
  int fd() const
  {
    return fd_;
  }

  // Reads a complete framed message. Returns std::nullopt on EOF or any error.
  std::optional<std::vector<uint8_t>> read_message();
  bool write_message(const std::vector<uint8_t> &body);

private:
  bool read_exact(uint8_t *buffer, size_t n);
  bool write_all(const uint8_t *buffer, size_t n);
  int fd_ = -1;
};

// ---------------------------------------------------------------------------
// What the bridge needs from the machine. VirtualHidSink (the Karabiner
// client + IOKit/CG readers) implements it in the process; tests implement it
// with a recorder.
// ---------------------------------------------------------------------------
struct CapsTruth
{
  std::optional<bool> state;
  const char *source = "none";
};

struct CursorPoint
{
  double x = 0;
  double y = 0;
};

class IReportSink
{
public:
  virtual ~IReportSink() = default;

  // HID boot-keyboard report: modifier byte + the set of held usages.
  virtual void post_keyboard(uint8_t modifier_bits, const std::set<uint16_t> &keys) = 0;
  // HID pointing report: held buttons, relative motion, wheels.
  virtual void post_pointing(
      const std::set<uint8_t> &buttons, int8_t dx, int8_t dy, int8_t vertical_wheel, int8_t horizontal_wheel
  ) = 0;
  // This machine's Caps Lock state (the login window's composition truth).
  virtual CapsTruth caps_lock_state() = 0;
  // The cursor's current position in points, if WindowServer answers.
  virtual std::optional<CursorPoint> cursor_position() = 0;
  // The main display in points + its backing scale; false leaves the
  // arguments untouched (the caller keeps its fallback).
  virtual bool main_display(int16_t &width, int16_t &height, double &backing_scale) = 0;
  // Blocks until every report posted so far has left this process's
  // queues (bounded); false when that could not be confirmed in time. The
  // bridge calls it after the release report on Leave, CBYE/disconnect and
  // the keyboard rescue (A-4).
  virtual bool flush(milliseconds bound) = 0;
};

// ---------------------------------------------------------------------------
// Key translation: Deskflow KeyID + modifier mask -> HID usage / modifier bits.
// ---------------------------------------------------------------------------
// Physical US-keyboard HID usage for an ASCII character, ignoring shift (shift
// is taken from the protocol mask). Both members of a shifted pair map here.
std::optional<uint16_t> ascii_to_physical_usage(char c);
// Deskflow special KeyIDs (0xEFxx) -> HID usage. See the "unmapped keys"
// note in deskflow-vhid-bridge.cpp for what is deliberately absent.
std::optional<uint16_t> special_keyid_to_usage(uint16_t key_id);
// Modifier KeyIDs map to a single HID modifier bit; non-modifier keys return 0.
uint8_t modifier_keyid_to_bit(uint16_t key_id);

// ---------------------------------------------------------------------------
// Bridge — owns input state and translates one host connection.
// ---------------------------------------------------------------------------
class Bridge
{
public:
  // Tests reach the ledger, the caps assumption and the clock.
  friend class BridgeTests;

  Bridge(
      IReportSink &sink, std::string client_name, int16_t fallback_w, int16_t fallback_h, double scale_factor,
      bool scale_fixed
  );

  // Self-calibration: slam the cursor to the top-left corner (OS clamps it
  // there), emit a known delta in <=8-count reports, read the cursor back and
  // derive counts_per_point. Runs at startup and, if that attempt could not
  // read the cursor (WindowServer not up yet), again after the next Enter's
  // corner slam. Returns true once calibrated.
  bool calibrate();

  // Runs one connection to completion (returns on disconnect/error/close,
  // and on the keyboard rescue, which stops the process via g_stop).
  void run(FramedSocket &socket);

private:
  // One held key/modifier, keyed by the Deskflow physical button id so that
  // key-up matches key-down even if the reported KeyID changed meanwhile.
  // modifier_bits are the REAL modifiers only (the mask's ctrl/alt/cmd, or
  // the bit of a modifier key): a derived Shift never lives here (A-1).
  struct HeldKey
  {
    std::optional<uint16_t> usage; // none for pure modifier keys
    uint8_t modifier_bits = 0;
    uint16_t key_id = 0;
    Clock::time_point since{};
    bool warned = false;
  };

  static constexpr int kMaxCalibrationAttempts = 5;
  static constexpr auto kStuckKeyWarning = std::chrono::seconds(10);

  void warn_stuck_keys();
  bool handshake(FramedSocket &socket);
  // Returns false to terminate the connection.
  bool dispatch(FramedSocket &socket, const std::vector<uint8_t> &body);
  bool send_screen_info(FramedSocket &socket);
  void warp_to(int x, int y);
  bool on_enter(const std::vector<uint8_t> &body);
  std::string keys_summary() const;
  bool on_mouse_abs(const std::vector<uint8_t> &body);
  bool on_mouse_rel(const std::vector<uint8_t> &body);
  bool on_mouse_button(const std::vector<uint8_t> &body, bool down);
  bool on_mouse_wheel(const std::vector<uint8_t> &body);
  // Returns true when the escape burst fired (the caller must stop).
  bool note_escape_down();
  bool on_key_down(const std::vector<uint8_t> &body);
  void sync_caps_lock(uint16_t key_id, uint32_t mask);
  CapsTruth read_caps_truth();
  void sync_caps_lock(uint16_t key_id, uint32_t mask, const CapsTruth &truth);
  bool on_key_up(const std::vector<uint8_t> &body);
  bool on_key_repeat(const std::vector<uint8_t> &body);
  bool parse_key(const std::vector<uint8_t> &body, int16_t &key_id, int16_t &mask, int16_t &button);
  std::optional<uint16_t> translate_key(uint16_t key_id);
  void collect_held(uint8_t &modifiers, std::set<uint16_t> &keys) const;
  // Posts the ledger; extra_modifier_bits ride on THIS report only (A-1).
  void emit_keyboard(uint8_t extra_modifier_bits = 0);
  void emit_counts(int dx, int dy);
  void emit_slam(int dx, int dy);
  void emit_relative(int dx, int dy);
  // Clears the ledger and posts the empty reports; with `flush` also waits
  // (bounded) until they have left the process's queues.
  void release_all(bool flush);

  static int8_t clamp_to_i8(int v);
  static uint8_t synergy_button_to_hid(uint8_t synergy_button);

  IReportSink &sink_;
  std::string client_name_;
  std::function<Clock::time_point()> now_ = [] { return Clock::now(); };
  std::map<int16_t, HeldKey> held_keys_;
  std::set<uint8_t> mouse_buttons_;
  int last_abs_x_ = 0;
  int last_abs_y_ = 0;
  bool have_last_abs_ = false;
  int16_t screen_w_ = 1920;
  int16_t screen_h_ = 1080;
  double scale_factor_ = 4.0; // seed knob (counts/point = backing x this); from arg
  bool scale_fixed_ = false;  // --scale-fixed: no calibration, no closed loop
  double motion_scale_ = 8.0; // host-point -> HID-count scale (seed, then calibrated)
  double frac_x_ = 0.0;       // sub-count carry so rounding never accumulates
  double frac_y_ = 0.0;
  bool calibrated_ = false;
  int calibration_attempts_ = 0;
  bool closed_loop_disabled_ = false;
  int saturated_residuals_ = 0;
  int escape_taps_ = 0;
  Clock::time_point last_escape_{};
  // Session counters (reset per connection), printed by keys_summary().
  unsigned long letters_shifted_ = 0;
  unsigned long letters_unshifted_ = 0;
  unsigned long caps_edges_ = 0;
  // Lock state assumed right after an edge we emitted, until the OS reader
  // agrees or kCapsAssumeMaxMs passes (read_caps_truth, A-5).
  std::optional<bool> assumed_caps_;
  Clock::time_point assumed_caps_at_{};
};

} // namespace vhid_bridge
