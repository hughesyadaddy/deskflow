// SPDX-FileCopyrightText: (C) 2026 Deskflow Contributors
// SPDX-License-Identifier: MIT
//
// deskflow-vhid-bridge core (see BridgeCore.h). No Karabiner SDK, IOKit or
// CoreGraphics here: the process supplies those through IReportSink.

#include "BridgeCore.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace vhid_bridge {

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
namespace {
std::function<void(const std::string &)> g_log_sink;
}

// Local wall-clock prefix (ISO 8601 with offset). launchd's StandardErrorPath
// adds no timestamps of its own, and fleet-health measures the gap between
// the bridge's start and its "virtual HID ready" line from these.
std::string log_timestamp()
{
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  ::localtime_r(&now, &local);
  char buf[40];
  if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &local) == 0)
    return "0000-00-00T00:00:00+0000";
  return buf;
}

void log_line(const std::string &message)
{
  // stderr is captured by the LaunchDaemon log; stdout is reserved for none.
  std::string line = log_timestamp() + " [bridge] " + message + "\n";
  if (g_log_sink) {
    g_log_sink(line);
    return;
  }
  ::write(STDERR_FILENO, line.data(), line.size());
}

void set_log_sink(std::function<void(const std::string &line)> sink)
{
  g_log_sink = std::move(sink);
}

bool g_debug_keys = false;

void log_keys(const std::string &message)
{
  if (g_debug_keys)
    log_line(message);
}

// ---------------------------------------------------------------------------
// Process control
// ---------------------------------------------------------------------------
int g_wake_pipe[2] = {-1, -1};
std::atomic<bool> g_stop{false};
std::atomic<bool> g_stand_down{false};

void wake()
{
  const char byte = 1;
  (void)!::write(g_wake_pipe[1], &byte, 1);
}

void drain_wake_pipe()
{
  char buf[64];
  while (::read(g_wake_pipe[0], buf, sizeof(buf)) > 0) {
  }
}

bool must_pause()
{
  return g_stop.load() || g_stand_down.load();
}

bool wait_or_wake(int timeout_ms)
{
  pollfd pfd{};
  pfd.fd = g_wake_pipe[0];
  pfd.events = POLLIN;
  if (::poll(&pfd, 1, timeout_ms) > 0) {
    drain_wake_pipe();
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Byte (de)serialization
// ---------------------------------------------------------------------------
bool ByteReader::skip(size_t n)
{
  if (remaining() < n) {
    ok_ = false;
    return false;
  }
  pos_ += n;
  return true;
}

std::optional<uint8_t> ByteReader::u8()
{
  if (remaining() < 1) {
    ok_ = false;
    return std::nullopt;
  }
  return data_[pos_++];
}

std::optional<int16_t> ByteReader::i16()
{
  if (remaining() < 2) {
    ok_ = false;
    return std::nullopt;
  }
  int16_t v = static_cast<int16_t>((data_[pos_] << 8) | data_[pos_ + 1]);
  pos_ += 2;
  return v;
}

void append_be16(std::vector<uint8_t> &out, int16_t value)
{
  auto u = static_cast<uint16_t>(value);
  out.push_back(static_cast<uint8_t>(u >> 8));
  out.push_back(static_cast<uint8_t>(u & 0xff));
}

void append_be32(std::vector<uint8_t> &out, uint32_t value)
{
  out.push_back(static_cast<uint8_t>(value >> 24));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value & 0xff));
}

void append_bytes(std::vector<uint8_t> &out, const char *bytes, size_t n)
{
  out.insert(out.end(), bytes, bytes + n);
}

bool body_has_code(const std::vector<uint8_t> &body, const char (&code)[4])
{
  return body.size() >= 4 && std::memcmp(body.data(), code, 4) == 0;
}

// ---------------------------------------------------------------------------
// Framed socket I/O
// ---------------------------------------------------------------------------
void FramedSocket::close()
{
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

std::optional<std::vector<uint8_t>> FramedSocket::read_message()
{
  std::array<uint8_t, 4> header{};
  if (!read_exact(header.data(), header.size()))
    return std::nullopt;
  uint32_t length = (static_cast<uint32_t>(header[0]) << 24) | (static_cast<uint32_t>(header[1]) << 16) |
                    (static_cast<uint32_t>(header[2]) << 8) | static_cast<uint32_t>(header[3]);
  if (length == 0 || length > proto::kMaxMessageBytes) {
    log_line("rejecting framed length " + std::to_string(length));
    return std::nullopt;
  }
  std::vector<uint8_t> body(length);
  if (!read_exact(body.data(), body.size()))
    return std::nullopt;
  return body;
}

bool FramedSocket::write_message(const std::vector<uint8_t> &body)
{
  std::vector<uint8_t> frame;
  frame.reserve(4 + body.size());
  append_be32(frame, static_cast<uint32_t>(body.size()));
  frame.insert(frame.end(), body.begin(), body.end());
  return write_all(frame.data(), frame.size());
}

bool FramedSocket::read_exact(uint8_t *buffer, size_t n)
{
  size_t got = 0;
  while (got < n) {
    pollfd pfds[2] = {};
    pfds[0].fd = fd_;
    pfds[0].events = POLLIN;
    pfds[1].fd = g_wake_pipe[0]; // -1 (no pipe, tests) is ignored by poll
    pfds[1].events = POLLIN;
    const int ready = ::poll(pfds, 2, kIoTimeoutSeconds * 1000);
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready <= 0 || (pfds[1].revents & POLLIN) != 0)
      return false; // idle timeout, or woken to stop / stand down
    ssize_t r = ::recv(fd_, buffer + got, n - got, 0);
    if (r == 0)
      return false; // peer closed
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    got += static_cast<size_t>(r);
  }
  return true;
}

bool FramedSocket::write_all(const uint8_t *buffer, size_t n)
{
  size_t sent = 0;
  while (sent < n) {
    ssize_t w = ::send(fd_, buffer + sent, n - sent, 0);
    if (w <= 0) {
      if (w < 0 && errno == EINTR)
        continue;
      return false;
    }
    sent += static_cast<size_t>(w);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Key translation
// ---------------------------------------------------------------------------
std::optional<uint16_t> ascii_to_physical_usage(char c)
{
  if (c >= 'a' && c <= 'z')
    return static_cast<uint16_t>(0x04 + (c - 'a'));
  if (c >= 'A' && c <= 'Z')
    return static_cast<uint16_t>(0x04 + (c - 'A'));
  switch (c) {
  case '1':
  case '!':
    return 0x1e;
  case '2':
  case '@':
    return 0x1f;
  case '3':
  case '#':
    return 0x20;
  case '4':
  case '$':
    return 0x21;
  case '5':
  case '%':
    return 0x22;
  case '6':
  case '^':
    return 0x23;
  case '7':
  case '&':
    return 0x24;
  case '8':
  case '*':
    return 0x25;
  case '9':
  case '(':
    return 0x26;
  case '0':
  case ')':
    return 0x27;
  case '-':
  case '_':
    return 0x2d;
  case '=':
  case '+':
    return 0x2e;
  case '[':
  case '{':
    return 0x2f;
  case ']':
  case '}':
    return 0x30;
  case '\\':
  case '|':
    return 0x31;
  case ';':
  case ':':
    return 0x33;
  case '\'':
  case '"':
    return 0x34;
  case '`':
  case '~':
    return 0x35;
  case ',':
  case '<':
    return 0x36;
  case '.':
  case '>':
    return 0x37;
  case '/':
  case '?':
    return 0x38;
  case ' ':
    return 0x2c;
  default:
    return std::nullopt;
  }
}

std::optional<uint16_t> special_keyid_to_usage(uint16_t key_id)
{
  // Function keys: kKeyF1..F12 = 0xEFBE..0xEFC9 -> usages 0x3a..0x45,
  // kKeyF13..F24 = 0xEFCA..0xEFD5 -> 0x68..0x73.
  if (key_id >= 0xEFBE && key_id <= 0xEFC9)
    return static_cast<uint16_t>(0x3a + (key_id - 0xEFBE));
  if (key_id >= 0xEFCA && key_id <= 0xEFD5)
    return static_cast<uint16_t>(0x68 + (key_id - 0xEFCA));
  // Keypad digits kKeyKP_0..9 = 0xEFB0..0xEFB9 -> the main-row digit keys
  // (the login window has no Num Lock to honour; a digit is a digit).
  if (key_id == 0xEFB0)
    return 0x27; // '0'
  if (key_id >= 0xEFB1 && key_id <= 0xEFB9)
    return static_cast<uint16_t>(0x1e + (key_id - 0xEFB1));
  switch (key_id) {
  case 0xEF08:
    return 0x2a; // BackSpace
  case 0xEF09:
    return 0x2b; // Tab
  case 0xEF0D:
    return 0x28; // Return
  case 0xEF8D:
    return 0x28; // KP_Enter -> Return
  case 0xEF1B:
    return 0x29; // Escape
  case 0xEFE5:
    return 0x39; // CapsLock (toggle handled by the target OS)
  case 0xEFFF:
    return 0x4c; // Delete (forward)
  case 0xEF63:
    return 0x49; // Insert
  case 0xEF50:
    return 0x4a; // Home
  case 0xEF51:
    return 0x50; // Left
  case 0xEF52:
    return 0x52; // Up
  case 0xEF53:
    return 0x4f; // Right
  case 0xEF54:
    return 0x51; // Down
  case 0xEF55:
    return 0x4b; // PageUp
  case 0xEF56:
    return 0x4e; // PageDown
  case 0xEF57:
    return 0x4d; // End
  case 0xEFAE:
    return 0x37; // KP_Decimal -> '.'
  default:
    return std::nullopt;
  }
}

uint8_t modifier_keyid_to_bit(uint16_t key_id)
{
  switch (key_id) {
  case 0xEFE1:
    return bridge_logic::kHidLeftShift;
  case 0xEFE2:
    return bridge_logic::kHidRightShift;
  case 0xEFE3:
    return bridge_logic::kHidLeftControl;
  case 0xEFE4:
    return bridge_logic::kHidRightControl;
  case 0xEFE9:
    return bridge_logic::kHidLeftOption;
  case 0xEFEA:
    return bridge_logic::kHidRightOption;
  case 0xEFE7:
  case 0xEFEB:
    return bridge_logic::kHidLeftCommand;
  case 0xEFE8:
  case 0xEFEC:
    return bridge_logic::kHidRightCommand;
  default:
    return 0;
  }
}

// ---------------------------------------------------------------------------
// Bridge
// ---------------------------------------------------------------------------
Bridge::Bridge(
    IReportSink &sink, std::string client_name, int16_t fallback_w, int16_t fallback_h, double scale_factor,
    bool scale_fixed
)
    : sink_(sink),
      client_name_(std::move(client_name)),
      screen_w_(fallback_w),
      screen_h_(fallback_h),
      scale_factor_(scale_factor),
      scale_fixed_(scale_fixed)
{
  // main_display overwrites these if the live display is readable; if it
  // isn't (can happen at the login window), the caller-supplied fallback — the
  // machine's real size from config — is kept instead of a wrong hardcoded guess.
  // It also reports the display's backing scale, from which we derive the SEED
  // motion scale (1x->4, 2x->8, 3x->12). Unless --scale-fixed, calibrate()
  // replaces the seed with a measured counts-per-point.
  double backing_scale = 2.0; // sane default (these Macs are 2x Retina) if the query fails
  if (!sink_.main_display(screen_w_, screen_h_, backing_scale)) {
    log_line(
        "display NOT detected (CG returned 0) -> fallback " + std::to_string(screen_w_) + "x" +
        std::to_string(screen_h_) + " (cursor will be confined if this is wrong)"
    );
  }
  motion_scale_ = backing_scale * scale_factor_;
  log_line(
      std::string(scale_fixed_ ? "motion scale FIXED " : "motion scale seed ") + std::to_string(motion_scale_) +
      " (backing " + std::to_string(backing_scale) + " x factor " + std::to_string(scale_factor_) + ")"
  );
}

bool Bridge::calibrate()
{
  if (scale_fixed_ || calibrated_)
    return calibrated_;
  if (++calibration_attempts_ > kMaxCalibrationAttempts) {
    return false;
  }
  constexpr int kSlam = 1 << 15;
  emit_slam(-kSlam, -kSlam);
  std::this_thread::sleep_for(milliseconds(150));
  const auto p0 = sink_.cursor_position();
  if (!p0) {
    log_line("calibrate: cursor unreadable (WindowServer not up?) — keeping seed scale, will retry on Enter");
    return false;
  }
  constexpr int kProbeX = 400, kProbeY = 300; // counts
  emit_counts(kProbeX, kProbeY);
  std::this_thread::sleep_for(milliseconds(150));
  const auto p1 = sink_.cursor_position();
  if (!p1) {
    log_line("calibrate: cursor unreadable after probe — keeping seed scale");
    return false;
  }
  const auto sx = bridge_logic::counts_per_point(kProbeX, p1->x - p0->x);
  const auto sy = bridge_logic::counts_per_point(kProbeY, p1->y - p0->y);
  const auto scale = bridge_logic::combine_axis_scales(sx, sy);
  log_line(
      "calibrate: probe +" + std::to_string(kProbeX) + "," + std::to_string(kProbeY) + " counts moved cursor from (" +
      std::to_string(static_cast<int>(p0->x)) + "," + std::to_string(static_cast<int>(p0->y)) + ") to (" +
      std::to_string(static_cast<int>(p1->x)) + "," + std::to_string(static_cast<int>(p1->y)) + ") -> x " +
      (sx ? std::to_string(*sx) : std::string("n/a")) + " y " + (sy ? std::to_string(*sy) : std::string("n/a")) +
      " counts/point"
  );
  if (!scale) {
    log_line("calibrate: measurement unusable — keeping seed scale " + std::to_string(motion_scale_));
    return false;
  }
  motion_scale_ = *scale;
  calibrated_ = true;
  log_line("calibrate: motion scale set to " + std::to_string(motion_scale_) + " counts/point (closed loop active)");
  return true;
}

void Bridge::run(FramedSocket &socket)
{
  if (!handshake(socket))
    return;
  letters_shifted_ = letters_unshifted_ = caps_edges_ = 0;
  release_all();
  while (!must_pause()) {
    std::optional<std::vector<uint8_t>> message = socket.read_message();
    if (!message)
      break;
    if (!dispatch(socket, *message))
      break;
    warn_stuck_keys();
  }
  // Also the keyboard-rescue path (A-4): on_key_down returned false with
  // g_stop set, and THIS release is the empty report that must reach the
  // daemon before main unwinds and destroys the sink.
  release_all();
  log_line("disconnect; " + keys_summary());
}

// Logs a WARNING (once per entry) for any key held longer than
// kStuckKeyWarning: a missed key-up or a latched entry shows up here
// instead of only as "everything types wrong until Leave".
void Bridge::warn_stuck_keys()
{
  if (held_keys_.empty())
    return;
  const auto now = now_();
  bool any = false;
  std::string summary;
  for (auto &[button, held] : held_keys_) {
    if (now - held.since < kStuckKeyWarning)
      continue;
    if (!held.warned) {
      held.warned = true;
      any = true;
    }
    summary += " {btn=" + std::to_string(button) +
               " held=" + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now - held.since).count()) +
               "s}";
  }
  if (any) {
    log_line(
        "WARNING: " + std::to_string(held_keys_.size()) + " key(s) in held_keys_, some > 10s:" + summary +
        " (Leave/close will release); " + keys_summary()
    );
  }
}

bool Bridge::handshake(FramedSocket &socket)
{
  std::optional<std::vector<uint8_t>> hello = socket.read_message();
  if (!hello || hello->size() < 11 || std::memcmp(hello->data(), proto::kGreeting, sizeof(proto::kGreeting)) != 0) {
    log_line("bad or missing server hello");
    return false;
  }
  ByteReader reader(*hello);
  reader.skip(sizeof(proto::kGreeting));
  std::optional<int16_t> major = reader.i16();
  std::optional<int16_t> minor = reader.i16();
  if (!reader.ok() || !major || !minor) {
    log_line("malformed server hello");
    return false;
  }
  if (*major != proto::kMajorVersion) {
    log_line("incompatible server major version " + std::to_string(*major));
    return false;
  }
  std::vector<uint8_t> reply;
  append_bytes(reply, proto::kGreeting, sizeof(proto::kGreeting));
  append_be16(reply, proto::kMajorVersion);
  append_be16(reply, proto::kMinorVersion);
  append_be32(reply, static_cast<uint32_t>(client_name_.size()));
  append_bytes(reply, client_name_.data(), client_name_.size());
  if (!socket.write_message(reply)) {
    log_line("failed to send hello-back");
    return false;
  }
  log_line(
      "handshake complete as \"" + client_name_ + "\" (server v" + std::to_string(*major) + "." +
      std::to_string(*minor) + ")"
  );
  return true;
}

bool Bridge::dispatch(FramedSocket &socket, const std::vector<uint8_t> &body)
{
  if (body_has_code(body, proto::kKeepAlive))
    return socket.write_message(body); // echo CALV
  if (body_has_code(body, proto::kNoop))
    return true;
  if (body_has_code(body, proto::kClose))
    return false;
  if (body_has_code(body, proto::kQueryInfo))
    return send_screen_info(socket);
  if (body_has_code(body, proto::kEnter))
    return on_enter(body);
  if (body_has_code(body, proto::kLeave)) {
    release_all();
    log_line("leave; " + keys_summary());
    return true;
  }
  if (body_has_code(body, proto::kMouseMove))
    return on_mouse_abs(body);
  if (body_has_code(body, proto::kMouseRelMove))
    return on_mouse_rel(body);
  if (body_has_code(body, proto::kMouseDown))
    return on_mouse_button(body, true);
  if (body_has_code(body, proto::kMouseUp))
    return on_mouse_button(body, false);
  if (body_has_code(body, proto::kMouseWheel))
    return on_mouse_wheel(body);
  if (body_has_code(body, proto::kKeyDown))
    return on_key_down(body);
  if (body_has_code(body, proto::kKeyDownLang))
    return on_key_down(body); // v1.8 sends DKDL
  if (body_has_code(body, proto::kKeyUp))
    return on_key_up(body);
  if (body_has_code(body, proto::kKeyRepeat))
    return on_key_repeat(body);
  // CIAK / CROP / DSOP / CSEC / CCLP and anything else: no input action.
  return true;
}

bool Bridge::send_screen_info(FramedSocket &socket)
{
  std::vector<uint8_t> info;
  append_bytes(info, proto::kInfo, sizeof(proto::kInfo));
  append_be16(info, 0);                                   // screen origin x
  append_be16(info, 0);                                   // screen origin y
  append_be16(info, screen_w_);                           // width
  append_be16(info, screen_h_);                           // height
  append_be16(info, 0);                                   // obsolete warp-zone
  append_be16(info, static_cast<int16_t>(screen_w_ / 2)); // initial cursor x
  append_be16(info, static_cast<int16_t>(screen_h_ / 2)); // initial cursor y
  return socket.write_message(info);
}

// A relative HID device has no absolute-position command, so establish a known
// origin by slamming to the screen corner *nearest* the target (the OS clamps
// the cursor there), then move to the target. Slamming to the nearest corner
// minimizes visible travel, so the cursor lands cleanly at the host's reported
// crossing point regardless of which edge the host sits on.
void Bridge::warp_to(int x, int y)
{
  constexpr int kSlamDistance = 1 << 15; // exceeds any display dimension
  int corner_x = (2 * x < screen_w_) ? 0 : screen_w_;
  int corner_y = (2 * y < screen_h_) ? 0 : screen_h_;
  emit_slam(corner_x == 0 ? -kSlamDistance : kSlamDistance, corner_y == 0 ? -kSlamDistance : kSlamDistance);
  if (!calibrated_ && !scale_fixed_) {
    // Startup calibration could not read the cursor; retry now that the
    // host is driving us (WindowServer is certainly up by this point).
    // calibrate() slams to the top-left corner itself, so re-slam after.
    if (calibrate()) {
      emit_slam(corner_x == 0 ? -kSlamDistance : kSlamDistance, corner_y == 0 ? -kSlamDistance : kSlamDistance);
    }
  }
  frac_x_ = frac_y_ = 0.0;
  emit_relative(x - corner_x, y - corner_y);
  last_abs_x_ = x;
  last_abs_y_ = y;
  have_last_abs_ = true;
}

bool Bridge::on_enter(const std::vector<uint8_t> &body)
{
  ByteReader r(body);
  r.skip(4);
  std::optional<int16_t> x = r.i16();
  std::optional<int16_t> y = r.i16();
  if (!r.ok() || !x || !y)
    return true;
  // Logs the host's crossing point against our reported size: if the host ever
  // drives near a boundary the bridge can't reach, the mismatch shows up here.
  // CINN = x, y, seq (4 bytes), modifier mask: the server's toggle state at
  // the crossing. Sync this machine's Caps Lock to it now so the first
  // letter is composed against the right lock state, not whatever a
  // previous session (or the user at the keyboard) left behind.
  r.skip(4);
  std::optional<int16_t> mask = r.i16();
  log_line(
      "enter " + std::to_string(*x) + "," + std::to_string(*y) + " of " + std::to_string(screen_w_) + "x" +
      std::to_string(screen_h_) + "; " + keys_summary()
  );
  warp_to(*x, *y);
  if (r.ok() && mask)
    sync_caps_lock(0, static_cast<uint32_t>(static_cast<uint16_t>(*mask)));
  return true;
}

// Per-session observability without keystrokes: how letters were composed
// and how often the target's Caps Lock had to be toggled.
std::string Bridge::keys_summary() const
{
  return "[keys] session letters shifted=" + std::to_string(letters_shifted_) +
         " unshifted=" + std::to_string(letters_unshifted_) + " caps-edges=" + std::to_string(caps_edges_);
}

bool Bridge::on_mouse_abs(const std::vector<uint8_t> &body)
{
  ByteReader r(body);
  r.skip(4);
  std::optional<int16_t> x = r.i16();
  std::optional<int16_t> y = r.i16();
  if (!r.ok() || !x || !y)
    return true;
  if (have_last_abs_) {
    int dx = static_cast<int>(*x) - last_abs_x_;
    int dy = static_cast<int>(*y) - last_abs_y_;
    // Closed loop: the previous move's reports have landed by now, so the
    // difference between where the host wanted the cursor and where it
    // actually is (in points) is carried into this delta -- bounded, so a
    // bad read can never turn into a sweep. Skipped under --scale-fixed.
    if (calibrated_ && !closed_loop_disabled_) {
      if (const auto here = sink_.cursor_position()) {
        const int rx = bridge_logic::bounded_residual(last_abs_x_, static_cast<int>(std::lround(here->x)));
        const int ry = bridge_logic::bounded_residual(last_abs_y_, static_cast<int>(std::lround(here->y)));
        dx += rx;
        dy += ry;
        // A residual pinned at the bound move after move means the readback
        // and the host disagree about coordinates (not a scale error) --
        // keep correcting and we would drift 48 points per move. Give up.
        const bool saturated =
            std::abs(rx) >= bridge_logic::kMaxResidualPoints || std::abs(ry) >= bridge_logic::kMaxResidualPoints;
        saturated_residuals_ = saturated ? saturated_residuals_ + 1 : 0;
        if (saturated_residuals_ >= 20) {
          closed_loop_disabled_ = true;
          log_line(
              "WARNING: closed-loop residual saturated for 20 moves (cursor at " +
              std::to_string(static_cast<int>(here->x)) + "," + std::to_string(static_cast<int>(here->y)) +
              " vs host " + std::to_string(last_abs_x_) + "," + std::to_string(last_abs_y_) +
              ") — disabling closed loop, keeping calibrated scale"
          );
        }
      }
    }
    emit_relative(dx, dy);
  }
  last_abs_x_ = *x;
  last_abs_y_ = *y;
  have_last_abs_ = true;
  return true;
}

bool Bridge::on_mouse_rel(const std::vector<uint8_t> &body)
{
  ByteReader r(body);
  r.skip(4);
  std::optional<int16_t> dx = r.i16();
  std::optional<int16_t> dy = r.i16();
  if (!r.ok() || !dx || !dy)
    return true;
  emit_relative(*dx, *dy);
  return true;
}

bool Bridge::on_mouse_button(const std::vector<uint8_t> &body, bool down)
{
  ByteReader r(body);
  r.skip(4);
  std::optional<uint8_t> synergy_button = r.u8();
  if (!r.ok() || !synergy_button)
    return true;
  uint8_t hid_button = synergy_button_to_hid(*synergy_button);
  if (hid_button == 0)
    return true;
  if (down)
    mouse_buttons_.insert(hid_button);
  else
    mouse_buttons_.erase(hid_button);
  sink_.post_pointing(mouse_buttons_, 0, 0, 0, 0);
  return true;
}

bool Bridge::on_mouse_wheel(const std::vector<uint8_t> &body)
{
  ByteReader r(body);
  r.skip(4);
  std::optional<int16_t> x = r.i16();
  std::optional<int16_t> y = r.i16();
  if (!r.ok() || !x || !y)
    return true;
  int8_t vertical = clamp_to_i8(*y / 120);
  int8_t horizontal = clamp_to_i8(*x / 120);
  if (vertical == 0 && *y != 0)
    vertical = (*y > 0) ? 1 : -1;
  if (horizontal == 0 && *x != 0)
    horizontal = (*x > 0) ? 1 : -1;
  sink_.post_pointing(mouse_buttons_, 0, 0, vertical, horizontal);
  return true;
}

// 5x Esc keyboard rescue, bridge edition.
/*!
At a login window the bridge is the ONLY Deskflow process on this machine,
so the fleet rescue (which restarts cores) cannot fix a wedged bridge. Count
relayed Esc presses and stop -- launchd's KeepAlive restarts us with a fresh
virtual HID device and empty held-key state. The threshold is 4, not 5,
because the server swallows the final tap of its own rescue gesture: the
same five presses therefore rescue the cores AND the bridge.

Returns true when the burst fired. The caller ends the connection; run()
then posts the release reports and main unwinds normally, so the sink is
destroyed (and the dispatcher flushed) AFTER the empty report is queued.
std::exit() here used to race that flush (A-4): the process could be gone
before the daemon ever saw the release, leaving the last key held on a
virtual keyboard nobody owned any more.
*/
bool Bridge::note_escape_down()
{
  const auto now = now_();
  if (now - last_escape_ > std::chrono::seconds(2)) {
    escape_taps_ = 0;
  }
  last_escape_ = now;
  if (++escape_taps_ < 4) {
    return false;
  }
  log_line("keyboard rescue: escape burst -- releasing input and stopping (launchd restarts the bridge)");
  g_stop.store(true);
  wake();
  return true;
}

bool Bridge::on_key_down(const std::vector<uint8_t> &body)
{
  int16_t key_id = 0, mask = 0, button = 0;
  if (!parse_key(body, key_id, mask, button))
    return true;
  const auto id16 = static_cast<uint16_t>(key_id);
  const auto mask32 = static_cast<uint32_t>(static_cast<uint16_t>(mask));
  if (id16 == 0xEF1B) { // Escape
    if (note_escape_down())
      return false; // run() releases everything and returns
  }
  // Caps Lock is an EDGE, never a held key. A relayed Caps Down used to be
  // stored in held_keys_ with the mask's modifiers, so Shift held at caps
  // time latched onto it until Leave, and a second caps Down overwrote the
  // same button with an identical entry -> no HID edge -> no toggle. Also
  // handle a mask-only event (id 0 with the caps bit): same sync logic.
  if (id16 == bridge_logic::kKeyIdCapsLock || (id16 == 0 && (mask32 & bridge_logic::kMaskCapsLock))) {
    sync_caps_lock(id16, mask32);
    return true;
  }
  HeldKey entry;
  entry.key_id = id16;
  entry.since = now_();
  uint8_t report_bits = 0; // this report only (A-1): the derived Shift
  uint8_t modifier_bit = modifier_keyid_to_bit(id16);
  if (modifier_bit != 0) {
    entry.modifier_bits = modifier_bit;
  } else {
    std::optional<uint16_t> usage = translate_key(id16);
    if (!usage) {
      log_keys("unmapped key");
      return true;
    }
    entry.usage = usage;
    // The KeyID already names the character the server wants typed; the
    // usage is always the unshifted key, so case lives in the modifier
    // byte and the target's Caps Lock. Letters are caps-sensitive (macOS
    // composes caps+shift as LOWERCASE): bridge_logic::decide_letter_modifiers
    // derives Shift from the KeyID, the server's Shift/Caps bits and this
    // machine's caps truth, and asks for a caps edge first whenever the
    // two lock states disagree (at most one edge per key-down). Non-letters
    // keep the mask's modifiers and gain Shift for shifted symbols.
    //
    // The DERIVED Shift is posted with this key-down only; the ledger entry
    // keeps the real modifiers (heldBits). Storing the derived Shift on the
    // entry leaked it into every later report while the key stayed held:
    // with the server's Caps on, rolling over `k` then `1` typed `k!`, and a
    // Shift released mid-repeat left the repeated letter in the wrong case.
    const bool is_letter = bridge_logic::keyid_is_letter(id16);
    CapsTruth truth;
    if (is_letter)
      truth = read_caps_truth();
    const bridge_logic::LetterDecision decision =
        bridge_logic::decide_letter_modifiers(id16, mask32, is_letter ? truth.state : std::nullopt);
    if (decision.capsEdge)
      sync_caps_lock(0, mask32, truth);
    entry.modifier_bits = decision.heldBits;
    report_bits = static_cast<uint8_t>(decision.modifierBits & ~decision.heldBits);
    if (is_letter) {
      if (decision.modifierBits & bridge_logic::kHidLeftShift)
        ++letters_shifted_;
      else
        ++letters_unshifted_;
    }
  }
  held_keys_[button] = entry;
  // Diagnostic only (--debug-keys): held count and nothing else. Not the
  // key id, usage or button (the typed text), and not the mask or modifier
  // byte either -- per-key Shift is the case pattern of a password. Case
  // composition is observable only through the session counters.
  log_keys("key down held=" + std::to_string(held_keys_.size()));
  emit_keyboard(report_bits);
  return true;
}

// Caps Lock: compare the server's desired lock state (the mask's caps bit)
// with this machine's truth and emit ONE press+release edge (usage 0x39,
// no modifiers, the currently held keys untouched) only when they differ.
// When the truth is unreadable, emit the edge unconditionally -- one edge
// per press is the best approximation of a real keyboard.
void Bridge::sync_caps_lock(uint16_t key_id, uint32_t mask)
{
  sync_caps_lock(key_id, mask, read_caps_truth());
}

// This machine's caps truth: the state we just set, for kCapsAssumeMs
// after an edge we emitted; otherwise the OS (IReportSink::caps_lock_state).
CapsTruth Bridge::read_caps_truth()
{
  if (assumed_caps_) {
    const auto now_ms = std::chrono::duration_cast<milliseconds>(now_().time_since_epoch()).count();
    const auto edge_ms = std::chrono::duration_cast<milliseconds>(assumed_caps_at_.time_since_epoch()).count();
    if (bridge_logic::caps_assumption_valid(now_ms, edge_ms))
      return {assumed_caps_, "assumed-after-edge"};
    assumed_caps_.reset();
  }
  return sink_.caps_lock_state();
}

// Same, with a truth the caller already read (one read per key-down).
// key_id == kKeyIdCapsLock is a real caps press: with the truth unknown
// one edge per press is the best approximation of a keyboard. key_id == 0
// is a SYNC (Enter, mask-only event, pre-letter): with the truth unknown
// it must do nothing -- a blind edge would toggle the real lock and invert
// every following letter.
void Bridge::sync_caps_lock(uint16_t key_id, uint32_t mask, const CapsTruth &truth)
{
  const bool desired = bridge_logic::desired_caps_from_mask(mask);
  const bool is_press = key_id == bridge_logic::kKeyIdCapsLock;
  const bool emit = is_press ? bridge_logic::caps_edge_needed(truth.state, desired)
                             : bridge_logic::caps_sync_edge_needed(truth.state, desired);
  log_keys(
      "caps " + std::string(key_id == 0 ? "sync" : "down") + " desired=" + (desired ? "on" : "off") +
      " truth=" + (truth.state ? (*truth.state ? "on" : "off") : "unknown") + " (" + truth.source + ") -> " +
      (emit ? "EDGE" : "skip")
  );
  if (!emit)
    return;
  ++caps_edges_;
  // The OS readers lag the toggle; for the next kCapsAssumeMs the lock IS
  // what we just set (see read_caps_truth), so a letter burst in the same
  // TCP read cannot edge twice.
  assumed_caps_ = desired;
  assumed_caps_at_ = now_();
  // Edge = held report + caps, then the held report without it. Modifiers of
  // the held keys stay as they are; the caps usage itself carries none.
  uint8_t modifiers = 0;
  std::set<uint16_t> keys;
  collect_held(modifiers, keys);
  std::set<uint16_t> with_caps = keys;
  with_caps.insert(bridge_logic::kUsageCapsLock);
  sink_.post_keyboard(modifiers, with_caps);
  sink_.post_keyboard(modifiers, keys);
}

bool Bridge::on_key_up(const std::vector<uint8_t> &body)
{
  int16_t key_id = 0, mask = 0, button = 0;
  if (!parse_key(body, key_id, mask, button))
    return true;
  const auto id16 = static_cast<uint16_t>(key_id);
  auto it = held_keys_.find(button);
  const bool was_held = it != held_keys_.end();
  if (was_held)
    held_keys_.erase(it);
  log_keys(std::string("key up") + (was_held ? "" : " (not held)") + " held=" + std::to_string(held_keys_.size()));
  if (id16 == bridge_logic::kKeyIdCapsLock)
    return true; // edge already emitted on the down
  emit_keyboard();
  return true;
}

// Auto-repeat: the held key is already down, so no report change is required.
bool Bridge::on_key_repeat(const std::vector<uint8_t> &)
{
  return true;
}

bool Bridge::parse_key(const std::vector<uint8_t> &body, int16_t &key_id, int16_t &mask, int16_t &button)
{
  ByteReader r(body);
  r.skip(4);
  std::optional<int16_t> id = r.i16();
  std::optional<int16_t> m = r.i16();
  if (!r.ok() || !id || !m)
    return false;
  std::optional<int16_t> b = r.i16(); // absent in 1.0 variant; default to KeyID
  key_id = *id;
  mask = *m;
  button = b.value_or(*id);
  return true;
}

std::optional<uint16_t> Bridge::translate_key(uint16_t key_id)
{
  if (std::optional<uint16_t> special = special_keyid_to_usage(key_id))
    return special;
  if (key_id >= 0x20 && key_id <= 0x7e)
    return ascii_to_physical_usage(static_cast<char>(key_id));
  return std::nullopt;
}

void Bridge::collect_held(uint8_t &modifiers, std::set<uint16_t> &keys) const
{
  for (const auto &[button, held] : held_keys_) {
    modifiers |= held.modifier_bits;
    if (held.usage)
      keys.insert(*held.usage);
  }
}

void Bridge::emit_keyboard(uint8_t extra_modifier_bits)
{
  uint8_t modifiers = extra_modifier_bits;
  std::set<uint16_t> keys;
  collect_held(modifiers, keys);
  sink_.post_keyboard(modifiers, keys);
}

// Karabiner's pointer is RELATIVE-only (no absolute mode); one HID count is
// a fixed fraction of a screen point (~1/8 on 2x Retina with acceleration
// off). Host-space deltas are scaled by motion_scale_ (counts per point:
// measured by calibrate(), or backing x --scale under --scale-fixed) with a
// fractional carry so rounding never accumulates.
//
// emit_counts is the unscaled stepper. Every report carries at most
// bridge_logic::kMaxChunk (8) counts per axis so motion stays inside the
// linear part of any acceleration curve the OS still applies (the accel
// disable is best-effort at the login window). The corner slam is the one
// exception: it only needs to overshoot the edge, and acceleration can
// only help it, so it uses full 127-count reports (4096 x fewer reports).
void Bridge::emit_counts(int dx, int dy)
{
  for (const bridge_logic::Step s : bridge_logic::chunk_delta_xy(dx, dy))
    sink_.post_pointing(mouse_buttons_, s.dx, s.dy, 0, 0);
}

void Bridge::emit_slam(int dx, int dy)
{
  for (const bridge_logic::Step s : bridge_logic::chunk_delta_xy(dx, dy, 127))
    sink_.post_pointing(mouse_buttons_, s.dx, s.dy, 0, 0);
}

void Bridge::emit_relative(int dx, int dy)
{
  const double fx = dx * motion_scale_ + frac_x_;
  const double fy = dy * motion_scale_ + frac_y_;
  const int cx = static_cast<int>(std::lround(fx));
  const int cy = static_cast<int>(std::lround(fy));
  frac_x_ = fx - cx;
  frac_y_ = fy - cy;
  emit_counts(cx, cy);
}

void Bridge::release_all()
{
  held_keys_.clear();
  mouse_buttons_.clear();
  have_last_abs_ = false;
  sink_.post_keyboard(0, {});
  sink_.post_pointing({}, 0, 0, 0, 0);
}

int8_t Bridge::clamp_to_i8(int v)
{
  return static_cast<int8_t>(std::clamp(v, -127, 127));
}

uint8_t Bridge::synergy_button_to_hid(uint8_t synergy_button)
{
  switch (synergy_button) {
  case 1:
    return 1; // left
  case 2:
    return 3; // middle
  case 3:
    return 2; // right
  default:
    return (synergy_button >= 1 && synergy_button <= 32) ? synergy_button : 0;
  }
}

} // namespace vhid_bridge
