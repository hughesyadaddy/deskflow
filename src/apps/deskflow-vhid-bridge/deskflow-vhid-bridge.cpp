// deskflow-vhid-bridge — replays a Deskflow host's mouse & keyboard stream
// onto this Mac through a Karabiner DriverKit virtual HID device, so the
// host can drive the machine at the login window where CGEventPost is blocked.
//
// Usage: deskflow-vhid-bridge <server_hosts> <client_screen_name>
//          [port [width height [scale_factor]]]
//          [--size=WxH] [--scale=S] [--scale-fixed] [--calibrate] [--coord-port=N]
//
// Pointer scale: by default the bridge self-calibrates (slam to a corner, emit
// a known delta, read the cursor back -> counts per point) and then runs every
// absolute move closed-loop against the real cursor position. --scale=S is only
// a seed unless --scale-fixed is also passed, which disables calibration and
// closed-loop correction and uses backing_scale x S verbatim (the pre-2026-09
// behaviour). --calibrate is accepted for explicitness; it is the default.
//
// When --coord-port is set, the bridge polls the local coordination mesh on
// 127.0.0.1:N before each reconnect pass and refreshes server candidates from
// the live fleet snapshot (GUI plist generation uses the same snapshot).
//
// <server_hosts> is a comma-separated candidate list. In auto-switch mode any
// peer can be the elected server, and only the elected server listens on the
// Deskflow port -- the bridge cycles the list until one accepts, and returns
// to cycling when that connection drops (role flip).
//
// Scope: TLS-disabled protocol only (the KVM runs inside Tailscale). It handles
// the handshake, keep-alives, screen-info query, and mouse/key data messages;
// non-input messages (clipboard, options, file transfer) are acknowledged or
// ignored. Mouse position is relayed as relative motion; the bridge disables
// OS pointer acceleration on its virtual device, calibrates counts-per-point,
// and corrects each absolute move against the real cursor (see above).

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDEventServiceKeys.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDProperties.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDParameter.h>

#include <pqrs/karabiner/driverkit/virtual_hid_device_driver.hpp>
#include <pqrs/karabiner/driverkit/virtual_hid_device_service.hpp>

#include "BridgeCalibration.h"

// IOHIDEventSystemClient — the modern (per-service) HID property API. It is
// exported by IOKit.framework but has no public header; these are the
// signatures Karabiner-Elements and Apple's own hidutil use. Unlike the legacy
// IOHIDSystem kIOHIDParamConnectType user client it does NOT require an
// exclusive event-system connection, which is exactly what fails at the
// LoginWindow session on macOS 26 (kr=-536870195 == kIOReturnExclusiveAccess).
extern "C"
{
  typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
  typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;
  IOHIDEventSystemClientRef IOHIDEventSystemClientCreateSimpleClient(CFAllocatorRef allocator);
  CFArrayRef IOHIDEventSystemClientCopyServices(IOHIDEventSystemClientRef client);
  CFTypeRef IOHIDServiceClientCopyProperty(IOHIDServiceClientRef service, CFStringRef key);
  Boolean IOHIDServiceClientSetProperty(IOHIDServiceClientRef service, CFStringRef key, CFTypeRef property);
}

namespace {

namespace hr = pqrs::karabiner::driverkit::virtual_hid_device_driver::hid_report;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

void log_line(const std::string &message)
{
  // stderr is captured by the LaunchDaemon log; stdout is reserved for none.
  std::string line = "[bridge] " + message + "\n";
  ::write(STDERR_FILENO, line.data(), line.size());
}

std::string hex_str(unsigned v)
{
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04x", v);
  return buf;
}

// Karabiner DriverKit virtual device identity (parameters.hpp / DriverKit sources).
constexpr int kKarabinerVendorId = 0x16c0;
constexpr int kKarabinerPointingProductId = 0x27da;
constexpr int kKarabinerKeyboardProductId = 0x27db;

struct CFReleaser
{
  void operator()(const void *ref) const
  {
    if (ref)
      CFRelease(ref);
  }
};
template <typename T> using cf_ptr = std::unique_ptr<std::remove_pointer_t<T>, CFReleaser>;

std::optional<int> service_int_property(IOHIDServiceClientRef service, CFStringRef key)
{
  cf_ptr<CFTypeRef> value(IOHIDServiceClientCopyProperty(service, key));
  if (!value || CFGetTypeID(value.get()) != CFNumberGetTypeID())
    return std::nullopt;
  int out = 0;
  if (!CFNumberGetValue(static_cast<CFNumberRef>(value.get()), kCFNumberIntType, &out))
    return std::nullopt;
  return out;
}

// Finds the Karabiner virtual HID service with the given product id (or any
// Karabiner service matching usage page/usage when the product id is absent).
// Returns the service and the owning client, which must outlive the service.
struct HidServiceHandle
{
  cf_ptr<IOHIDEventSystemClientRef> client;
  cf_ptr<CFArrayRef> services;
  IOHIDServiceClientRef service = nullptr; // borrowed from `services`
};

HidServiceHandle
find_karabiner_service(int product_id, int usage_page, int usage, const char *what, bool verbose = true)
{
  HidServiceHandle h;
  h.client.reset(IOHIDEventSystemClientCreateSimpleClient(kCFAllocatorDefault));
  if (!h.client) {
    if (verbose)
      log_line(std::string("hid: IOHIDEventSystemClientCreateSimpleClient failed (") + what + ")");
    return h;
  }
  h.services.reset(IOHIDEventSystemClientCopyServices(h.client.get()));
  if (!h.services) {
    if (verbose)
      log_line(std::string("hid: IOHIDEventSystemClientCopyServices returned null (") + what + ")");
    return h;
  }
  const CFIndex n = CFArrayGetCount(h.services.get());
  for (CFIndex i = 0; i < n; ++i) {
    auto service = static_cast<IOHIDServiceClientRef>(const_cast<void *>(CFArrayGetValueAtIndex(h.services.get(), i)));
    const auto vendor = service_int_property(service, CFSTR(kIOHIDVendorIDKey));
    if (!vendor || *vendor != kKarabinerVendorId)
      continue;
    const auto product = service_int_property(service, CFSTR(kIOHIDProductIDKey));
    const auto page = service_int_property(service, CFSTR(kIOHIDPrimaryUsagePageKey));
    const auto use = service_int_property(service, CFSTR(kIOHIDPrimaryUsageKey));
    const bool product_ok = product && *product == product_id;
    const bool usage_ok = page && use && *page == usage_page && *use == usage;
    if (product_ok || usage_ok) {
      log_line(
          std::string("hid: found Karabiner ") + what + " service (product=0x" +
          hex_str(static_cast<unsigned>(product.value_or(0))) + " page=" + std::to_string(page.value_or(0)) +
          " usage=" + std::to_string(use.value_or(0)) + ") among " + std::to_string(n) + " services"
      );
      h.service = service;
      return h;
    }
  }
  if (verbose) {
    log_line(
        std::string("hid: no Karabiner ") + what + " service among " + std::to_string(n) +
        " HID services (vendor 0x16c0 not present yet?)"
    );
  }
  return h;
}

// We inject RELATIVE pointer motion; under macOS pointer acceleration a delta does
// not map 1:1 to a screen point, so the cursor drifts from the host's absolute
// position. Force linear (no-accel) on the VIRTUAL POINTING SERVICE itself via
// the per-service property API (value -1 == acceleration off, the same thing
// `hidutil property --set '{"HIDPointerAcceleration":-1}'` does).
//
// The legacy IOHIDSystem kIOHIDParamConnectType route this replaced failed on
// every LoginWindow run on macOS 26 (only the first-ever run got the exclusive
// connection) -- so acceleration was silently live while the bridge multiplied
// deltas by 8 and chopped them into int8 reports: fast, nonlinear cursor.
//
// Returns true when both keys were accepted. Runs on a detached thread so a
// hung HID call can never block the bridge; calibration + closed-loop
// correction cover the case where this fails.
bool disable_pointer_acceleration()
{
  HidServiceHandle h =
      find_karabiner_service(kKarabinerPointingProductId, kHIDPage_GenericDesktop, kHIDUsage_GD_Mouse, "pointing");
  if (!h.service) {
    log_line("accel: FAILED — virtual pointing service not found; relying on calibration + closed loop");
    return false;
  }
  int minus_one = -1;
  cf_ptr<CFNumberRef> value(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &minus_one));
  const Boolean rm = IOHIDServiceClientSetProperty(h.service, CFSTR(kIOHIDMouseAccelerationType), value.get());
  const Boolean rp = IOHIDServiceClientSetProperty(h.service, CFSTR(kIOHIDPointerAccelerationKey), value.get());
  const auto readback = service_int_property(h.service, CFSTR(kIOHIDPointerAccelerationKey));
  log_line(
      std::string("accel: ") + ((rm && rp) ? "DISABLED" : "FAILED") +
      " on virtual pointing service (HIDMouseAcceleration=" + (rm ? "ok" : "rejected") + " HIDPointerAcceleration=" +
      (rp ? "ok" : "rejected") + " readback=" + (readback ? std::to_string(*readback) : std::string("n/a")) + ")"
  );
  return rm && rp;
}

// Reads the cursor's current position in points. Works at the login window
// because the bridge already reaches WindowServer (CGMainDisplayID succeeds).
std::optional<CGPoint> read_cursor_position()
{
  cf_ptr<CGEventRef> event(CGEventCreate(nullptr));
  if (!event)
    return std::nullopt;
  CGPoint p = CGEventGetLocation(event.get());
  if (!std::isfinite(p.x) || !std::isfinite(p.y))
    return std::nullopt;
  return p;
}

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

  bool skip(size_t n)
  {
    if (remaining() < n) {
      ok_ = false;
      return false;
    }
    pos_ += n;
    return true;
  }
  std::optional<uint8_t> u8()
  {
    if (remaining() < 1) {
      ok_ = false;
      return std::nullopt;
    }
    return data_[pos_++];
  }
  std::optional<int16_t> i16()
  {
    if (remaining() < 2) {
      ok_ = false;
      return std::nullopt;
    }
    int16_t v = static_cast<int16_t>((data_[pos_] << 8) | data_[pos_ + 1]);
    pos_ += 2;
    return v;
  }

private:
  const std::vector<uint8_t> &data_;
  size_t pos_ = 0;
  bool ok_ = true;
};

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

  void close()
  {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }
  int fd() const
  {
    return fd_;
  }

  // Reads a complete framed message. Returns std::nullopt on EOF or any error.
  std::optional<std::vector<uint8_t>> read_message()
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

  bool write_message(const std::vector<uint8_t> &body)
  {
    std::vector<uint8_t> frame;
    frame.reserve(4 + body.size());
    append_be32(frame, static_cast<uint32_t>(body.size()));
    frame.insert(frame.end(), body.begin(), body.end());
    return write_all(frame.data(), frame.size());
  }

private:
  bool read_exact(uint8_t *buffer, size_t n)
  {
    size_t got = 0;
    while (got < n) {
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
  bool write_all(const uint8_t *buffer, size_t n)
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
  int fd_ = -1;
};

// A blocking connect() to a black-holed host (firewall drop, sleeping machine)
// stalls ~75s, defeating the reconnect backoff; a healthy link must also notice a
// silently-dead host (no RST) rather than blocking in recv() forever. Bound both.
constexpr int kConnectTimeoutMs = 4000;
constexpr int kIoTimeoutSeconds = 10; // > deskflow's 5s CALV keep-alive interval

void set_io_timeouts(int fd)
{
  timeval tv{};
  tv.tv_sec = kIoTimeoutSeconds;
  tv.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

bool connect_with_timeout(int fd, const sockaddr *addr, socklen_t len, int timeout_ms)
{
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    return false;
  bool connected = false;
  if (::connect(fd, addr, len) == 0) {
    connected = true;
  } else if (errno == EINPROGRESS) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    if (::poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLOUT)) {
      int err = 0;
      socklen_t err_len = sizeof(err);
      if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len) == 0 && err == 0)
        connected = true;
    }
  }
  ::fcntl(fd, F_SETFL, flags); // restore blocking for read_exact/write_all
  return connected;
}

int connect_tcp(const std::string &host, uint16_t port)
{
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *result = nullptr;
  std::string port_str = std::to_string(port);
  int rc = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
  if (rc != 0) {
    log_line(std::string("getaddrinfo: ") + gai_strerror(rc));
    return -1;
  }
  int fd = -1;
  for (addrinfo *ai = result; ai != nullptr; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0)
      continue;
    if (connect_with_timeout(fd, ai->ai_addr, ai->ai_addrlen, kConnectTimeoutMs)) {
      set_io_timeouts(fd);
      break;
    }
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(result);
  return fd;
}

std::optional<std::string> json_quoted_value(const std::string &json, const std::string &key, size_t from = 0)
{
  const std::string needle = "\"" + key + "\":\"";
  const size_t pos = json.find(needle, from);
  if (pos == std::string::npos) {
    return std::nullopt;
  }
  const size_t start = pos + needle.size();
  const size_t end = json.find('"', start);
  if (end == std::string::npos) {
    return std::nullopt;
  }
  return json.substr(start, end - start);
}

void merge_unique_host(std::vector<std::string> &hosts, const std::string &host)
{
  if (host.empty()) {
    return;
  }
  if (std::ranges::find(hosts, host) == hosts.end()) {
    hosts.push_back(host);
  }
}

bool refresh_hosts_from_coord_snapshot(
    uint16_t coord_port, const std::string &self_name, std::vector<std::string> &hosts
)
{
  int fd = connect_tcp("127.0.0.1", coord_port);
  if (fd < 0) {
    return false;
  }
  static constexpr char kStatusRequest[] = "{\"t\":\"status\"}\n";
  if (::send(fd, kStatusRequest, std::strlen(kStatusRequest), 0) < 0) {
    ::close(fd);
    return false;
  }

  std::string line;
  char buffer[512];
  while (line.find('\n') == std::string::npos) {
    const ssize_t received = ::recv(fd, buffer, sizeof(buffer) - 1, 0);
    if (received <= 0) {
      ::close(fd);
      return false;
    }
    buffer[received] = '\0';
    line.append(buffer, static_cast<size_t>(received));
    if (line.size() > 65536) {
      ::close(fd);
      return false;
    }
  }
  ::close(fd);
  const size_t newline = line.find('\n');
  if (newline != std::string::npos) {
    line.resize(newline);
  }

  std::vector<std::string> fresh;
  if (const auto server_ip = json_quoted_value(line, "server_ip")) {
    merge_unique_host(fresh, *server_ip);
  }

  size_t search_from = 0;
  while (true) {
    const size_t name_pos = line.find("\"name\":\"", search_from);
    if (name_pos == std::string::npos) {
      break;
    }
    const size_t name_start = name_pos + 8;
    const size_t name_end = line.find('"', name_start);
    if (name_end == std::string::npos) {
      break;
    }
    const std::string name = line.substr(name_start, name_end - name_start);
    search_from = name_end + 1;
    if (name == self_name) {
      continue;
    }
    const size_t object_end = line.find('}', name_end);
    if (object_end == std::string::npos) {
      break;
    }
    const std::string peer_object = line.substr(name_pos, object_end - name_pos);
    if (const auto ip = json_quoted_value(peer_object, "ip")) {
      merge_unique_host(fresh, *ip);
    }
    if (const auto lan = json_quoted_value(peer_object, "lan")) {
      merge_unique_host(fresh, *lan);
    }
    merge_unique_host(fresh, name);
  }

  if (fresh.empty()) {
    return false;
  }
  hosts = std::move(fresh);
  return true;
}

// ---------------------------------------------------------------------------
// Virtual HID sink — owns the pqrs client and emits HID reports.
// ---------------------------------------------------------------------------
constexpr std::array<hr::modifier, 8> kAllModifiers = {hr::modifier::left_control,  hr::modifier::left_shift,
                                                       hr::modifier::left_option,   hr::modifier::left_command,
                                                       hr::modifier::right_control, hr::modifier::right_shift,
                                                       hr::modifier::right_option,  hr::modifier::right_command};

class VirtualHidSink
{
public:
  VirtualHidSink()
  {
    pqrs::dispatcher::extra::initialize_shared_dispatcher();
    client_ = std::make_unique<pqrs::karabiner::driverkit::virtual_hid_device_service::client>();
    client_->connected.connect([this] {
      pqrs::karabiner::driverkit::virtual_hid_device_service::virtual_hid_keyboard_parameters p;
      p.set_country_code(pqrs::hid::country_code::us);
      client_->async_virtual_hid_keyboard_initialize(p);
      client_->async_virtual_hid_pointing_initialize();
    });
    client_->connect_failed.connect([](auto &&ec) { log_line("vhid connect_failed: " + std::to_string(ec.value())); });
    client_->virtual_hid_keyboard_ready.connect([this](bool r) { keyboard_ready_ = r; });
    client_->virtual_hid_pointing_ready.connect([this](bool r) { pointing_ready_ = r; });
  }
  VirtualHidSink(const VirtualHidSink &) = delete;
  VirtualHidSink &operator=(const VirtualHidSink &) = delete;
  ~VirtualHidSink()
  {
    if (client_)
      client_->async_stop();
    pqrs::dispatcher::extra::terminate_shared_dispatcher();
  }

  void start()
  {
    client_->async_start();
  }

  bool wait_ready(milliseconds timeout)
  {
    Clock::time_point deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
      if (keyboard_ready_ && pointing_ready_)
        return true;
      std::this_thread::sleep_for(milliseconds(20));
    }
    return keyboard_ready_ && pointing_ready_;
  }

  void post_keyboard(uint8_t modifier_bits, const std::set<uint16_t> &keys)
  {
    hr::keyboard_input report;
    for (hr::modifier m : kAllModifiers) {
      if (modifier_bits & static_cast<uint8_t>(m))
        report.modifiers.insert(m);
    }
    for (uint16_t usage : keys)
      report.keys.insert(usage);
    client_->async_post_report(report);
  }

  void
  post_pointing(const std::set<uint8_t> &buttons, int8_t dx, int8_t dy, int8_t vertical_wheel, int8_t horizontal_wheel)
  {
    hr::pointing_input report;
    for (uint8_t b : buttons)
      report.buttons.insert(b);
    report.x = static_cast<uint8_t>(dx);
    report.y = static_cast<uint8_t>(dy);
    report.vertical_wheel = static_cast<uint8_t>(vertical_wheel);
    report.horizontal_wheel = static_cast<uint8_t>(horizontal_wheel);
    client_->async_post_report(report);
  }

private:
  std::unique_ptr<pqrs::karabiner::driverkit::virtual_hid_device_service::client> client_;
  std::atomic<bool> keyboard_ready_{false};
  std::atomic<bool> pointing_ready_{false};
};

// ---------------------------------------------------------------------------
// Key translation: Deskflow KeyID + modifier mask -> HID usage / modifier bits.
// ---------------------------------------------------------------------------
// Physical US-keyboard HID usage for an ASCII character, ignoring shift (shift
// is taken from the protocol mask). Both members of a shifted pair map here.
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
  switch (key_id) {
  case 0xEF08:
    return 0x2a; // BackSpace
  case 0xEF09:
    return 0x2b; // Tab
  case 0xEF0D:
    return 0x28; // Return
  case 0xEF1B:
    return 0x29; // Escape
  case 0xEFE5:
    return 0x39; // CapsLock (toggle handled by the target OS)
  case 0xEFFF:
    return 0x4c; // Delete (forward)
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
  default:
    return std::nullopt;
  }
}

// True for KeyIDs that name a character reachable only with shift on the US
// layout: uppercase letters and the shifted symbol row/pairs. The KeyID is
// the character the server wants typed, so shift is implied even when the
// protocol modifier mask lacks it (e.g. uppercase composed via caps lock).
bool keyid_requires_shift(uint16_t key_id)
{
  if (key_id >= 'A' && key_id <= 'Z')
    return true;
  switch (key_id) {
  case '!':
  case '@':
  case '#':
  case '$':
  case '%':
  case '^':
  case '&':
  case '*':
  case '(':
  case ')':
  case '_':
  case '+':
  case '{':
  case '}':
  case '|':
  case ':':
  case '"':
  case '~':
  case '<':
  case '>':
  case '?':
    return true;
  default:
    return false;
  }
}

// True for KeyIDs naming an alphabetic character. Caps Lock affects ONLY
// these on the US layout, so only these need caps-aware shift handling.
bool keyid_is_letter(uint16_t key_id)
{
  return (key_id >= 'A' && key_id <= 'Z') || (key_id >= 'a' && key_id <= 'z');
}

// Live Caps Lock state of THIS machine.
/*!
The bridge injects raw HID reports, and macOS composes caps+shift as
LOWERCASE, so the shift decision for letters depends on the target's caps
state -- and a relayed caps press must only emit a toggle edge when the
target's lock state differs from what the server wants. Three sources, in
order:
  1. IOHIDServiceClient "HIDCapsLockState" on the Karabiner virtual keyboard
     (modern per-service API; needs no exclusive connection).
  2. A fresh legacy IOHIDSystem kIOHIDParamConnectType open +
     IOHIDGetModifierLockState. Fails with kIOReturnExclusiveAccess on every
     LoginWindow run after the first on macOS 26, but is cheap to try.
  3. CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState) & AlphaShift --
     WindowServer's view, reachable from the bridge at the login window.
state is nullopt only when none of them can answer.
*/
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
std::optional<bool> caps_state_legacy_iohidsystem()
{
  io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(kIOHIDSystemClass));
  if (service == IO_OBJECT_NULL)
    return std::nullopt;
  io_connect_t connect = MACH_PORT_NULL;
  std::optional<bool> state;
  if (IOServiceOpen(service, mach_task_self(), kIOHIDParamConnectType, &connect) == KERN_SUCCESS) {
    bool value = false;
    if (IOHIDGetModifierLockState(connect, kIOHIDCapsLockState, &value) == KERN_SUCCESS)
      state = value;
    IOServiceClose(connect);
  }
  IOObjectRelease(service);
  return state;
}
#pragma clang diagnostic pop

std::optional<bool> caps_state_virtual_keyboard_service()
{
  // Cached: the virtual keyboard outlives the bridge process, and this is
  // consulted on every relayed letter. Re-resolved only while unresolved.
  static HidServiceHandle h;
  static bool logged_miss = false;
  if (!h.service) {
    h = find_karabiner_service(
        kKarabinerKeyboardProductId, kHIDPage_GenericDesktop, kHIDUsage_GD_Keyboard, "keyboard", !logged_miss
    );
    logged_miss = true;
  }
  if (!h.service)
    return std::nullopt;
  cf_ptr<CFTypeRef> value(IOHIDServiceClientCopyProperty(h.service, CFSTR(kIOHIDServiceCapsLockStateKey)));
  if (!value)
    return std::nullopt;
  if (CFGetTypeID(value.get()) == CFBooleanGetTypeID())
    return CFBooleanGetValue(static_cast<CFBooleanRef>(value.get()));
  if (CFGetTypeID(value.get()) == CFNumberGetTypeID()) {
    int v = 0;
    if (CFNumberGetValue(static_cast<CFNumberRef>(value.get()), kCFNumberIntType, &v))
      return v != 0;
  }
  return std::nullopt;
}

std::optional<bool> caps_state_cg_flags()
{
  const CGEventFlags flags = CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState);
  // CG has no "unknown"; 0 is a legitimate answer, so only distrust it when
  // WindowServer is unreachable altogether (the cursor read fails too).
  if (flags == 0 && !read_cursor_position())
    return std::nullopt;
  return (flags & kCGEventFlagMaskAlphaShift) != 0;
}

struct CapsTruth
{
  std::optional<bool> state;
  const char *source = "none";
};

CapsTruth target_caps_lock_state()
{
  if (auto s = caps_state_virtual_keyboard_service())
    return {s, "vkbd-service"};
  if (auto s = caps_state_legacy_iohidsystem())
    return {s, "iohidsystem"};
  if (auto s = caps_state_cg_flags())
    return {s, "cg-flags"};
  return {};
}

// Modifier KeyIDs map to a single HID modifier bit; non-modifier keys return 0.
uint8_t modifier_keyid_to_bit(uint16_t key_id)
{
  switch (key_id) {
  case 0xEFE1:
    return static_cast<uint8_t>(hr::modifier::left_shift);
  case 0xEFE2:
    return static_cast<uint8_t>(hr::modifier::right_shift);
  case 0xEFE3:
    return static_cast<uint8_t>(hr::modifier::left_control);
  case 0xEFE4:
    return static_cast<uint8_t>(hr::modifier::right_control);
  case 0xEFE9:
    return static_cast<uint8_t>(hr::modifier::left_option);
  case 0xEFEA:
    return static_cast<uint8_t>(hr::modifier::right_option);
  case 0xEFE7:
  case 0xEFEB:
    return static_cast<uint8_t>(hr::modifier::left_command);
  case 0xEFE8:
  case 0xEFEC:
    return static_cast<uint8_t>(hr::modifier::right_command);
  default:
    return 0;
  }
}

// Deskflow mask -> HID modifier byte lives in BridgeCalibration.h (pure, unit
// tested). Its SDK-free constants must equal hr::modifier's values:
static_assert(bridge_logic::kHidLeftShift == static_cast<uint8_t>(hr::modifier::left_shift));
static_assert(bridge_logic::kHidLeftControl == static_cast<uint8_t>(hr::modifier::left_control));
static_assert(bridge_logic::kHidLeftOption == static_cast<uint8_t>(hr::modifier::left_option));
static_assert(bridge_logic::kHidLeftCommand == static_cast<uint8_t>(hr::modifier::left_command));
static_assert(bridge_logic::kMaskShift == proto::kMaskShift && bridge_logic::kMaskSuper == proto::kMaskSuper);

// ---------------------------------------------------------------------------
// Bridge — owns input state and translates one host connection.
// ---------------------------------------------------------------------------
class Bridge
{
public:
  Bridge(
      VirtualHidSink &sink, std::string client_name, int16_t fallback_w, int16_t fallback_h, double scale_factor,
      bool scale_fixed
  )
      : sink_(sink),
        client_name_(std::move(client_name)),
        screen_w_(fallback_w),
        screen_h_(fallback_h),
        scale_factor_(scale_factor),
        scale_fixed_(scale_fixed)
  {
    // query_main_display overwrites these if the live display is readable; if it
    // isn't (can happen at the login window), the caller-supplied fallback — the
    // machine's real size from config — is kept instead of a wrong hardcoded guess.
    // It also reports the display's backing scale, from which we derive the SEED
    // motion scale (1x->4, 2x->8, 3x->12). Unless --scale-fixed, calibrate()
    // replaces the seed with a measured counts-per-point.
    double backing_scale = 2.0;
    query_main_display(screen_w_, screen_h_, backing_scale);
    motion_scale_ = backing_scale * scale_factor_;
    log_line(
        std::string(scale_fixed_ ? "motion scale FIXED " : "motion scale seed ") + std::to_string(motion_scale_) +
        " (backing " + std::to_string(backing_scale) + " x factor " + std::to_string(scale_factor_) + ")"
    );
  }

  // Self-calibration: slam the cursor to the top-left corner (OS clamps it
  // there), emit a known delta in <=8-count reports, read the cursor back and
  // derive counts_per_point. Runs at startup and, if that attempt could not
  // read the cursor (WindowServer not up yet), again after the next Enter's
  // corner slam. Returns true once calibrated.
  bool calibrate()
  {
    if (scale_fixed_ || calibrated_)
      return calibrated_;
    if (++calibration_attempts_ > kMaxCalibrationAttempts) {
      return false;
    }
    constexpr int kSlam = 1 << 15;
    emit_slam(-kSlam, -kSlam);
    std::this_thread::sleep_for(milliseconds(150));
    const auto p0 = read_cursor_position();
    if (!p0) {
      log_line("calibrate: cursor unreadable (WindowServer not up?) — keeping seed scale, will retry on Enter");
      return false;
    }
    constexpr int kProbeX = 400, kProbeY = 300; // counts
    emit_counts(kProbeX, kProbeY);
    std::this_thread::sleep_for(milliseconds(150));
    const auto p1 = read_cursor_position();
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

  // Runs one connection to completion (returns on disconnect/error/close).
  void run(FramedSocket &socket)
  {
    if (!handshake(socket))
      return;
    release_all();
    while (!g_stop.load()) {
      std::optional<std::vector<uint8_t>> message = socket.read_message();
      if (!message)
        break;
      if (!dispatch(socket, *message))
        break;
      warn_stuck_keys();
    }
    release_all();
  }

private:
  // One held key/modifier, keyed by the Deskflow physical button id so that
  // key-up matches key-down even if the reported KeyID changed meanwhile.
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

  // Logs a WARNING (once per entry) for any key held longer than
  // kStuckKeyWarning: a missed key-up or a latched entry shows up here
  // instead of only as "everything types wrong until Leave".
  void warn_stuck_keys()
  {
    if (held_keys_.empty())
      return;
    const auto now = Clock::now();
    bool any = false;
    std::string summary;
    for (auto &[button, held] : held_keys_) {
      if (now - held.since < kStuckKeyWarning)
        continue;
      if (!held.warned) {
        held.warned = true;
        any = true;
      }
      summary += " {btn=" + std::to_string(button) + " id=0x" + to_hex(held.key_id) + " usage=0x" +
                 to_hex(held.usage.value_or(0)) + " mods=0x" + to_hex(held.modifier_bits) +
                 " held=" + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now - held.since).count()) +
                 "s}";
    }
    if (any) {
      log_line(
          "WARNING: " + std::to_string(held_keys_.size()) + " key(s) in held_keys_, some > 10s:" + summary +
          " (Leave/close will release)"
      );
    }
  }

  bool handshake(FramedSocket &socket)
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

  // Returns false to terminate the connection.
  bool dispatch(FramedSocket &socket, const std::vector<uint8_t> &body)
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

  // width/height come in pre-set to the fallback. At the login window the display
  // can be momentarily unreported (CG returns 0) right at boot, which previously
  // pinned the screen to the 1920x1080 fallback and confined the cursor to a
  // sub-rectangle of the real screen — the "invisible barrier". Retry briefly so a
  // not-yet-ready display is detected once WindowServer comes up, and log the
  // outcome so a genuine miss is diagnosable rather than silent.
  static void query_main_display(int16_t &width, int16_t &height, double &backing_scale)
  {
    backing_scale = 2.0;                             // sane default (these Macs are 2x Retina) if the query fails
    for (int attempt = 0; attempt < 15; ++attempt) { // ~3s
      CGDirectDisplayID display = CGMainDisplayID();
      size_t w = CGDisplayPixelsWide(display);
      size_t h = CGDisplayPixelsHigh(display);
      if (w >= 16 && w <= 32767 && h >= 16 && h <= 32767) {
        width = static_cast<int16_t>(w);
        height = static_cast<int16_t>(h);
        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
        size_t pw = mode ? CGDisplayModeGetPixelWidth(mode) : w;
        if (mode)
          CGDisplayModeRelease(mode);
        if (w && pw)
          backing_scale = static_cast<double>(pw) / static_cast<double>(w);
        log_line(
            "display detected " + std::to_string(w) + "x" + std::to_string(h) + " (native px width " +
            std::to_string(pw) + ", backing scale " + std::to_string(backing_scale) + ")"
        );
        return;
      }
      std::this_thread::sleep_for(milliseconds(200));
    }
    log_line(
        "display NOT detected (CG returned 0) -> fallback " + std::to_string(width) + "x" + std::to_string(height) +
        " (cursor will be confined if this is wrong)"
    );
  }

  bool send_screen_info(FramedSocket &socket)
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
  void warp_to(int x, int y)
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

  bool on_enter(const std::vector<uint8_t> &body)
  {
    ByteReader r(body);
    r.skip(4);
    std::optional<int16_t> x = r.i16();
    std::optional<int16_t> y = r.i16();
    if (!r.ok() || !x || !y)
      return true;
    // Logs the host's crossing point against our reported size: if the host ever
    // drives near a boundary the bridge can't reach, the mismatch shows up here.
    log_line(
        "enter " + std::to_string(*x) + "," + std::to_string(*y) + " of " + std::to_string(screen_w_) + "x" +
        std::to_string(screen_h_)
    );
    warp_to(*x, *y);
    return true;
  }

  bool on_mouse_abs(const std::vector<uint8_t> &body)
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
        if (const auto here = read_cursor_position()) {
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

  bool on_mouse_rel(const std::vector<uint8_t> &body)
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

  bool on_mouse_button(const std::vector<uint8_t> &body, bool down)
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

  bool on_mouse_wheel(const std::vector<uint8_t> &body)
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
  relayed Esc presses and exit -- launchd's KeepAlive restarts us with a fresh
  virtual HID device and empty held-key state. The threshold is 4, not 5,
  because the server swallows the final tap of its own rescue gesture: the
  same five presses therefore rescue the cores AND the bridge.
  */
  void note_escape_down()
  {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_escape_ > std::chrono::seconds(2)) {
      escape_taps_ = 0;
    }
    last_escape_ = now;
    if (++escape_taps_ < 4) {
      return;
    }
    log_line("keyboard rescue: escape burst -- releasing input and restarting bridge");
    release_all();
    g_stop.store(true);
    std::exit(0); // launchd KeepAlive restarts us clean
  }

  bool on_key_down(const std::vector<uint8_t> &body)
  {
    int16_t key_id = 0, mask = 0, button = 0;
    if (!parse_key(body, key_id, mask, button))
      return true;
    const auto id16 = static_cast<uint16_t>(key_id);
    const auto mask32 = static_cast<uint32_t>(static_cast<uint16_t>(mask));
    if (id16 == 0xEF1B) { // Escape
      note_escape_down();
    }
    // Caps Lock is an EDGE, never a held key. A relayed Caps Down used to be
    // stored in held_keys_ with the mask's modifiers, so Shift held at caps
    // time latched onto it until Leave, and a second caps Down overwrote the
    // same button with an identical entry -> no HID edge -> no toggle. Also
    // handle a mask-only event (id 0 with the caps bit): same sync logic.
    if (id16 == bridge_logic::kKeyIdCapsLock || (id16 == 0 && (mask32 & bridge_logic::kMaskCapsLock))) {
      sync_caps_lock(id16, mask32, button);
      return true;
    }
    HeldKey entry;
    entry.key_id = id16;
    entry.since = Clock::now();
    uint8_t modifier_bit = modifier_keyid_to_bit(id16);
    if (modifier_bit != 0) {
      entry.modifier_bits = modifier_bit;
    } else {
      std::optional<uint16_t> usage = translate_key(id16);
      if (!usage) {
        log_line("unmapped key id 0x" + to_hex(id16));
        return true;
      }
      entry.usage = usage;
      entry.modifier_bits = bridge_logic::key_down_modifier_bits(id16, mask32);
      // The KeyID already names the character the server wants typed; a
      // shifted character must carry shift even when the protocol mask
      // lacks it (caps-lock-composed uppercase, relay-normalized masks).
      //
      // Letters are caps-sensitive: macOS composes caps+shift as LOWERCASE,
      // so with caps ON the shift decision INVERTS -- an uppercase letter
      // needs NO shift and a lowercase letter needs one. Ignoring this made
      // every relayed letter come out inverted whenever caps was on at the
      // login window (where the user cannot see or fix it).
      //
      // For letters the KeyID + caps truth decide shift OUTRIGHT (the mask's
      // shift bit is dropped): the log showed 'K' relayed with Shift while
      // caps was on, which macOS composes as lowercase. Non-letters keep the
      // mask's shift (shift+arrow selection etc.) and only ever gain it.
      bool wantShift = keyid_requires_shift(id16);
      if (keyid_is_letter(id16)) {
        const CapsTruth truth = target_caps_lock_state();
        if (truth.state.value_or(false)) {
          wantShift = !wantShift;
        }
        entry.modifier_bits &= static_cast<uint8_t>(~static_cast<uint8_t>(hr::modifier::left_shift));
      }
      if (wantShift) {
        entry.modifier_bits |= static_cast<uint8_t>(hr::modifier::left_shift);
      }
    }
    held_keys_[button] = entry;
    log_line(
        "key down id=0x" + to_hex(id16) + " mask=0x" + to_hex(static_cast<uint16_t>(mask)) +
        " btn=" + std::to_string(button) + " -> usage=0x" + to_hex(entry.usage.value_or(0)) + " mods=0x" +
        to_hex(entry.modifier_bits) + " held=" + std::to_string(held_keys_.size())
    );
    emit_keyboard();
    return true;
  }

  // Caps Lock: compare the server's desired lock state (the mask's caps bit)
  // with this machine's truth and emit ONE press+release edge (usage 0x39,
  // no modifiers, the currently held keys untouched) only when they differ.
  // When the truth is unreadable, emit the edge unconditionally -- one edge
  // per press is the best approximation of a real keyboard.
  void sync_caps_lock(uint16_t key_id, uint32_t mask, int16_t button)
  {
    const bool desired = bridge_logic::desired_caps_from_mask(mask);
    const CapsTruth truth = target_caps_lock_state();
    const bool emit = bridge_logic::caps_edge_needed(truth.state, desired);
    log_line(
        "caps " + std::string(key_id == 0 ? "mask-only" : "down") + " id=0x" + to_hex(key_id) + " mask=0x" +
        to_hex(static_cast<uint16_t>(mask)) + " btn=" + std::to_string(button) +
        " desired=" + (desired ? "on" : "off") + " truth=" + (truth.state ? (*truth.state ? "on" : "off") : "unknown") +
        " (" + truth.source + ") -> " + (emit ? "EDGE" : "skip")
    );
    if (!emit)
      return;
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

  bool on_key_up(const std::vector<uint8_t> &body)
  {
    int16_t key_id = 0, mask = 0, button = 0;
    if (!parse_key(body, key_id, mask, button))
      return true;
    const auto id16 = static_cast<uint16_t>(key_id);
    auto it = held_keys_.find(button);
    const bool was_held = it != held_keys_.end();
    const uint16_t usage = was_held ? it->second.usage.value_or(0) : 0;
    if (was_held)
      held_keys_.erase(it);
    log_line(
        "key up id=0x" + to_hex(id16) + " mask=0x" + to_hex(static_cast<uint16_t>(mask)) +
        " btn=" + std::to_string(button) + " -> usage=0x" + to_hex(usage) + (was_held ? "" : " (not held)") +
        " held=" + std::to_string(held_keys_.size())
    );
    if (id16 == bridge_logic::kKeyIdCapsLock)
      return true; // edge already emitted on the down
    emit_keyboard();
    return true;
  }

  // Auto-repeat: the held key is already down, so no report change is required.
  bool on_key_repeat(const std::vector<uint8_t> &)
  {
    return true;
  }

  bool parse_key(const std::vector<uint8_t> &body, int16_t &key_id, int16_t &mask, int16_t &button)
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

  std::optional<uint16_t> translate_key(uint16_t key_id)
  {
    if (std::optional<uint16_t> special = special_keyid_to_usage(key_id))
      return special;
    if (key_id >= 0x20 && key_id <= 0x7e)
      return ascii_to_physical_usage(static_cast<char>(key_id));
    return std::nullopt;
  }

  void collect_held(uint8_t &modifiers, std::set<uint16_t> &keys) const
  {
    for (const auto &[button, held] : held_keys_) {
      modifiers |= held.modifier_bits;
      if (held.usage)
        keys.insert(*held.usage);
    }
  }

  void emit_keyboard()
  {
    uint8_t modifiers = 0;
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

  void emit_counts(int dx, int dy)
  {
    for (const bridge_logic::Step s : bridge_logic::chunk_delta_xy(dx, dy))
      sink_.post_pointing(mouse_buttons_, s.dx, s.dy, 0, 0);
  }

  void emit_slam(int dx, int dy)
  {
    for (const bridge_logic::Step s : bridge_logic::chunk_delta_xy(dx, dy, 127))
      sink_.post_pointing(mouse_buttons_, s.dx, s.dy, 0, 0);
  }

  void emit_relative(int dx, int dy)
  {
    const double fx = dx * motion_scale_ + frac_x_;
    const double fy = dy * motion_scale_ + frac_y_;
    const int cx = static_cast<int>(std::lround(fx));
    const int cy = static_cast<int>(std::lround(fy));
    frac_x_ = fx - cx;
    frac_y_ = fy - cy;
    emit_counts(cx, cy);
  }

  int escape_taps_ = 0;
  std::chrono::steady_clock::time_point last_escape_{};

  void release_all()
  {
    held_keys_.clear();
    mouse_buttons_.clear();
    have_last_abs_ = false;
    sink_.post_keyboard(0, {});
    sink_.post_pointing({}, 0, 0, 0, 0);
  }

  static int8_t clamp_to_i8(int v)
  {
    return static_cast<int8_t>(std::clamp(v, -127, 127));
  }
  static uint8_t synergy_button_to_hid(uint8_t synergy_button)
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
  static std::string to_hex(uint16_t v)
  {
    static const char *digits = "0123456789abcdef";
    std::string s(4, '0');
    for (int i = 3; i >= 0; --i) {
      s[i] = digits[v & 0xf];
      v >>= 4;
    }
    return s;
  }

  VirtualHidSink &sink_;
  std::string client_name_;
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

public:
  static std::atomic<bool> g_stop;
};

std::atomic<bool> Bridge::g_stop{false};

} // namespace

int main(int argc, char **argv)
{
  // Split flag arguments (--size=WxH, --scale=S) from positionals so the
  // launchd plist generated by the GUI can pass options without having to
  // fill every preceding positional slot. Legacy positional forms still work.
  std::vector<std::string> positional;
  std::optional<int16_t> flag_w, flag_h;
  std::optional<double> flag_scale;
  std::optional<uint16_t> flag_coord_port;
  bool scale_fixed = false, calibrate = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--size=", 0) == 0) {
      int w = 0, h = 0;
      if (std::sscanf(arg.c_str() + 7, "%dx%d", &w, &h) == 2 && w >= 16 && w <= 32767 && h >= 16 && h <= 32767) {
        flag_w = static_cast<int16_t>(w);
        flag_h = static_cast<int16_t>(h);
      } else {
        log_line("invalid --size (expected WxH): " + arg);
        return 2;
      }
    } else if (arg.rfind("--scale=", 0) == 0) {
      double s = std::strtod(arg.c_str() + 8, nullptr);
      if (s > 0.1 && s < 100.0) {
        flag_scale = s;
      } else {
        log_line("invalid --scale: " + arg);
        return 2;
      }
    } else if (arg.rfind("--coord-port=", 0) == 0) {
      const long parsed = std::strtol(arg.c_str() + 13, nullptr, 10);
      if (parsed <= 0 || parsed > 65535) {
        log_line("invalid --coord-port: " + arg);
        return 2;
      }
      flag_coord_port = static_cast<uint16_t>(parsed);
    } else if (arg == "--scale-fixed") {
      scale_fixed = true;
    } else if (arg == "--calibrate") {
      calibrate = true; // the default; accepted so plists can say so explicitly
    } else {
      positional.push_back(arg);
    }
  }
  if (scale_fixed && calibrate) {
    log_line("--scale-fixed and --calibrate are mutually exclusive");
    return 2;
  }

  if (positional.size() < 2) {
    log_line(
        "usage: deskflow-vhid-bridge <server_hosts> <client_screen_name> "
        "[port [width height [scale_factor]]] [--size=WxH] [--scale=S] [--scale-fixed] [--calibrate] "
        "[--coord-port=N]"
    );
    return 2;
  }

  // Comma-separated server candidates: in auto-switch mode only the elected
  // server listens, so the bridge cycles the list until one accepts.
  std::vector<std::string> server_hosts;
  {
    const std::string &list = positional[0];
    size_t start = 0;
    while (start <= list.size()) {
      size_t comma = list.find(',', start);
      std::string host = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
      while (!host.empty() && host.front() == ' ')
        host.erase(host.begin());
      while (!host.empty() && host.back() == ' ')
        host.pop_back();
      if (!host.empty())
        server_hosts.push_back(host);
      if (comma == std::string::npos)
        break;
      start = comma + 1;
    }
  }
  if (server_hosts.empty()) {
    log_line("no server hosts given");
    return 2;
  }
  const std::string client_name = positional[1];
  uint16_t port = proto::kDefaultPort;
  if (positional.size() >= 3) {
    long parsed = std::strtol(positional[2].c_str(), nullptr, 10);
    if (parsed <= 0 || parsed > 65535) {
      log_line("invalid port");
      return 2;
    }
    port = static_cast<uint16_t>(parsed);
  }
  // Optional authoritative screen size (from config) used as the fallback when the
  // live display query is empty — fixes the login-window cursor "barrier" where a
  // wrong size confined the cursor to a sub-rectangle of the real screen.
  int16_t fallback_w = 1920, fallback_h = 1080;
  if (positional.size() >= 5) {
    long pw = std::strtol(positional[3].c_str(), nullptr, 10);
    long ph = std::strtol(positional[4].c_str(), nullptr, 10);
    if (pw >= 16 && pw <= 32767)
      fallback_w = static_cast<int16_t>(pw);
    if (ph >= 16 && ph <= 32767)
      fallback_h = static_cast<int16_t>(ph);
  }
  if (flag_w && flag_h) {
    fallback_w = *flag_w;
    fallback_h = *flag_h;
  }
  // Optional sensitivity knob: counts-per-point = backing_scale * scale_factor.
  // 4.0 gives the calibrated full-reach 1:1 on 2x Retina (x8); lower = less sensitive
  // (gentler motion, but the cursor reaches less of the screen). Tunable from config.
  double scale_factor = 4.0;
  if (positional.size() >= 6) {
    double s = std::strtod(positional[5].c_str(), nullptr);
    if (s > 0.1 && s < 100.0)
      scale_factor = s;
  }
  if (flag_scale)
    scale_factor = *flag_scale;

  struct sigaction sa{};
  sa.sa_handler = [](int) { Bridge::g_stop.store(true); };
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);
  signal(SIGPIPE, SIG_IGN);

  VirtualHidSink sink;
  sink.start();
  if (!sink.wait_ready(milliseconds(10000))) {
    log_line("virtual HID device not ready (is the Karabiner daemon running?)");
    return 1;
  }
  {
    std::string hosts;
    for (const auto &h : server_hosts)
      hosts += (hosts.empty() ? "" : ", ") + h;
    log_line("virtual HID ready; server candidates: " + hosts + " port " + std::to_string(port));
  }

  // Disable acceleration on the virtual pointing service (it exists only now
  // that the sink is ready) so motion is LINEAR. On its own thread so a hung
  // HID call can never block the bridge; we give it a moment to land before
  // calibrating so the measurement reflects the accel-off state.
  {
    std::promise<void> done;
    std::future<void> done_f = done.get_future();
    std::thread([p = std::move(done)]() mutable {
      disable_pointer_acceleration();
      p.set_value();
    }).detach();
    if (done_f.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
      log_line("accel: disable still pending after 2s — continuing without waiting");
  }

  Bridge bridge(sink, client_name, fallback_w, fallback_h, scale_factor, scale_fixed);
  if (!scale_fixed) {
    std::this_thread::sleep_for(milliseconds(250)); // let the accel property settle
    bridge.calibrate();
  }
  const uint16_t coord_port = flag_coord_port.value_or(0);
  // Cycle the candidate list; back off only after a full pass with no server
  // accepting, so a role flip to any peer is picked up within one pass.
  int backoff_ms = 500;
  size_t host_idx = 0;
  while (!Bridge::g_stop.load()) {
    if (coord_port != 0) {
      std::vector<std::string> refreshed = server_hosts;
      if (refresh_hosts_from_coord_snapshot(coord_port, client_name, refreshed)) {
        server_hosts = std::move(refreshed);
        host_idx = 0;
      }
    }
    const std::string &host = server_hosts[host_idx];
    int fd = connect_tcp(host, port);
    if (fd < 0) {
      host_idx = (host_idx + 1) % server_hosts.size();
      if (host_idx == 0) {
        std::this_thread::sleep_for(milliseconds(backoff_ms));
        backoff_ms = std::min(backoff_ms * 2, 5000);
      }
      continue;
    }
    backoff_ms = 500;
    log_line("connected to host " + host);
    FramedSocket socket(fd);
    bridge.run(socket);
    log_line("host connection closed");
  }
  return 0;
}
