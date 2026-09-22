// deskflow-vhid-bridge — replays a Deskflow host's mouse & keyboard stream
// onto this Mac through a Karabiner DriverKit virtual HID device, so the
// host can drive the machine at the login window where CGEventPost is blocked.
//
// Usage: deskflow-vhid-bridge <server_hosts> <client_screen_name>
//          [port [width height [scale_factor]]]
//          [--size=WxH] [--scale=S] [--scale-fixed] [--calibrate] [--coord-port=N]
//          [--vhid-wait-s=N] [--debug-keys]
//
// Logging never includes key ids, usages, buttons or anything else decodable
// to typed text: at the login window that stream is the password. At the
// default level the per-session "[keys]" summary (Enter/Leave/disconnect)
// carries only the number of Caps Lock edges emitted and a yes/no for
// whether case composition was applied at all; the stuck-key WARNING carries
// counts and durations, never a button id (a scancode). --debug-keys adds
// per-key lines carrying only the held count (never the key, and not the
// modifier mask/byte either: per-key Shift is a password's case pattern),
// plus the per-session letter counters (shifted/unshifted -- a password's
// letter and uppercase counts, which is why they are debug-only); it is for
// diagnosis only, never for a production plist.
//
// Unmapped keys: the bridge speaks the US layout and the Deskflow KeyID
// space it can reach with a boot-keyboard usage table: ASCII, Backspace,
// Tab, Return/KP_Enter, Escape, Insert/Delete, Home/End/PageUp/PageDown,
// arrows, F1-F24, the keypad digits (as the main-row digits) and KP_Decimal.
// Anything else -- non-ASCII characters (accented letters, other scripts),
// dead keys, keypad operators and the remaining special keys -- is dropped
// with a "--debug-keys"-only "unmapped key" line. A login-window password
// containing such a character cannot be typed through the bridge; fleet
// health's `--check loginbridge` says so.
//
// The bridge only injects while the console user is loginwindow. When a user
// session takes the console (login, fast user switch) it releases every key,
// closes the socket (the FIN is the goodbye; the server has no CBYE handler)
// and waits; SIGTERM drains the same way and exits 0 at once instead of after
// the next message or socket timeout. The 4x Esc keyboard rescue ends the
// connection the same way (release reports, then main unwinds and the sink
// is destroyed) so the daemon always sees the release before we are gone.
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
//
// Structure: the protocol reader, the input ledger and the report translation
// live in BridgeCore.{h,cpp} (unit-tested through IReportSink); this file is
// the process: the Karabiner client, the IOKit/CoreGraphics readers and main.

#include <algorithm>
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
#include <memory>
#include <mutex>
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
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <CoreGraphics/CoreGraphics.h>
#include <SystemConfiguration/SystemConfiguration.h>
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
#include "BridgeCore.h"
#include "common/SingleInstanceLock.h"

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

using namespace vhid_bridge;
namespace hr = pqrs::karabiner::driverkit::virtual_hid_device_driver::hid_report;

bool console_user_is_loginwindow(SCDynamicStoreRef store)
{
  uid_t uid = 0;
  gid_t gid = 0;
  CFStringRef name = SCDynamicStoreCopyConsoleUser(store, &uid, &gid);
  if (name == nullptr)
    return true;
  const bool loginwindow = CFStringCompare(name, CFSTR("loginwindow"), 0) == kCFCompareEqualTo;
  CFRelease(name);
  return loginwindow;
}

void apply_console_user(SCDynamicStoreRef store)
{
  const bool standDown = !console_user_is_loginwindow(store);
  if (g_stand_down.exchange(standDown) != standDown) {
    log_line(standDown ? "console user is a user session: standing down" : "console user is loginwindow: resuming");
    wake();
  }
}

void console_user_changed(SCDynamicStoreRef store, CFArrayRef, void *)
{
  apply_console_user(store);
}

// Watches State:/Users/ConsoleUser on its own run loop for the life of the process.
void start_console_user_watch()
{
  SCDynamicStoreRef store = SCDynamicStoreCreate(nullptr, CFSTR("deskflow-vhid-bridge"), console_user_changed, nullptr);
  if (store == nullptr) {
    log_line("console-user watch unavailable (SCDynamicStoreCreate failed); assuming loginwindow");
    return;
  }
  apply_console_user(store);
  CFStringRef key = SCDynamicStoreKeyCreateConsoleUser(nullptr);
  CFArrayRef keys = CFArrayCreate(nullptr, reinterpret_cast<const void **>(&key), 1, &kCFTypeArrayCallBacks);
  SCDynamicStoreSetNotificationKeys(store, keys, nullptr);
  CFRelease(keys);
  CFRelease(key);
  std::thread([store] {
    CFRunLoopSourceRef source = SCDynamicStoreCreateRunLoopSource(nullptr, store, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
    CFRunLoopRun();
    CFRelease(source);
    CFRelease(store);
  }).detach();
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
// correction cover the case where this fails. The property lives on the
// virtual service, so a daemon reconnect (which re-creates the device)
// silently reverts it -- VirtualHidSink re-applies it when the pointing
// device comes back (A-8).
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
std::optional<CursorPoint> read_cursor_position()
{
  cf_ptr<CGEventRef> event(CGEventCreate(nullptr));
  if (!event)
    return std::nullopt;
  CGPoint p = CGEventGetLocation(event.get());
  if (!std::isfinite(p.x) || !std::isfinite(p.y))
    return std::nullopt;
  return CursorPoint{p.x, p.y};
}

// width/height come in pre-set to the fallback. At the login window the display
// can be momentarily unreported (CG returns 0) right at boot, which previously
// pinned the screen to the 1920x1080 fallback and confined the cursor to a
// sub-rectangle of the real screen — the "invisible barrier". Retry briefly so a
// not-yet-ready display is detected once WindowServer comes up; the caller logs
// a genuine miss so it is diagnosable rather than silent.
bool query_main_display(int16_t &width, int16_t &height, double &backing_scale)
{
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
      return true;
    }
    std::this_thread::sleep_for(milliseconds(200));
  }
  return false;
}

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

// Live Caps Lock state of THIS machine.
/*!
The bridge injects raw HID reports, and macOS composes caps+shift as
LOWERCASE, so the shift decision for letters depends on the target's caps
state -- and a relayed caps press must only emit a toggle edge when the
target's lock state differs from what the server wants. Three sources, in
order:
  1. CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState) & AlphaShift --
     WindowServer's view of the real lock state, the one the login window
     composes against; reachable from the bridge at the login window.
  2. A fresh legacy IOHIDSystem kIOHIDParamConnectType open +
     IOHIDGetModifierLockState. Fails with kIOReturnExclusiveAccess on every
     LoginWindow run after the first on macOS 26, but is cheap to try.
  3. IOHIDServiceClient "HIDCapsLockState" on the Karabiner virtual keyboard,
     LAST: it is the device the bridge itself toggles (sync_caps_lock), so its
     value can lag or disagree with the system after an edge. Consulting it
     first (from a process-lifetime cache) made every relayed letter come out
     lowercase whenever it held a wrong `true`. The handle lives on the
     VirtualHidSink and is reset on every daemon (re)connect.
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

std::optional<bool> caps_state_cg_flags()
{
  const CGEventFlags flags = CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState);
  // CG has no "unknown"; 0 is a legitimate answer, so only distrust it when
  // WindowServer is unreachable altogether (the cursor read fails too).
  if (flags == 0 && !read_cursor_position())
    return std::nullopt;
  return (flags & kCGEventFlagMaskAlphaShift) != 0;
}

// ---------------------------------------------------------------------------
// Virtual HID sink — owns the pqrs client and emits HID reports; the
// process-side IReportSink.
// ---------------------------------------------------------------------------
constexpr std::array<hr::modifier, 8> kAllModifiers = {hr::modifier::left_control,  hr::modifier::left_shift,
                                                       hr::modifier::left_option,   hr::modifier::left_command,
                                                       hr::modifier::right_control, hr::modifier::right_shift,
                                                       hr::modifier::right_option,  hr::modifier::right_command};

// BridgeCalibration.h's SDK-free constants must equal hr::modifier's values.
static_assert(bridge_logic::kHidLeftShift == static_cast<uint8_t>(hr::modifier::left_shift));
static_assert(bridge_logic::kHidLeftControl == static_cast<uint8_t>(hr::modifier::left_control));
static_assert(bridge_logic::kHidLeftOption == static_cast<uint8_t>(hr::modifier::left_option));
static_assert(bridge_logic::kHidLeftCommand == static_cast<uint8_t>(hr::modifier::left_command));
static_assert(bridge_logic::kHidRightShift == static_cast<uint8_t>(hr::modifier::right_shift));
static_assert(bridge_logic::kHidRightControl == static_cast<uint8_t>(hr::modifier::right_control));
static_assert(bridge_logic::kHidRightOption == static_cast<uint8_t>(hr::modifier::right_option));
static_assert(bridge_logic::kHidRightCommand == static_cast<uint8_t>(hr::modifier::right_command));
static_assert(bridge_logic::kMaskShift == proto::kMaskShift && bridge_logic::kMaskSuper == proto::kMaskSuper);

class VirtualHidSink : public IReportSink
{
public:
  VirtualHidSink()
  {
    pqrs::dispatcher::extra::initialize_shared_dispatcher();
    client_ = std::make_unique<pqrs::karabiner::driverkit::virtual_hid_device_service::client>();
    client_->connected.connect([this] {
      // A (re)connect means the daemon re-created the virtual keyboard: any
      // IOHIDServiceClient we resolved for the old one is stale, so drop it
      // and let the next caps query re-resolve.
      {
        std::lock_guard<std::mutex> lock(keyboard_service_mutex_);
        keyboard_service_ = HidServiceHandle{};
        keyboard_service_logged_miss_ = false;
      }
      // ... and the pointing device is re-created too, WITHOUT the
      // acceleration-off property main applied to the old one (A-8). It
      // does not exist yet at this point (initialize is only requested
      // below), so flag it and re-apply once pointing_ready reports it.
      if (connected_before_.exchange(true))
        reapply_accel_on_ready_ = true;
      pqrs::karabiner::driverkit::virtual_hid_device_service::virtual_hid_keyboard_parameters p;
      p.set_country_code(pqrs::hid::country_code::us);
      client_->async_virtual_hid_keyboard_initialize(p);
      client_->async_virtual_hid_pointing_initialize();
    });
    client_->connect_failed.connect([this](auto &&ec) { note_connect_failed(ec.value()); });
    client_->virtual_hid_keyboard_ready.connect([this](bool r) { keyboard_ready_ = r; });
    client_->virtual_hid_pointing_ready.connect([this](bool r) {
      pointing_ready_ = r;
      if (r && reapply_accel_on_ready_.exchange(false)) {
        log_line("accel: daemon reconnected, re-applying acceleration-off to the new pointing device");
        // Detached, like main's first application: a hung HID call must
        // never block the dispatcher thread.
        std::thread([] { disable_pointer_acceleration(); }).detach();
      }
    });
  }
  VirtualHidSink(const VirtualHidSink &) = delete;
  VirtualHidSink &operator=(const VirtualHidSink &) = delete;
  ~VirtualHidSink() override
  {
    if (client_)
      client_->async_stop();
    pqrs::dispatcher::extra::terminate_shared_dispatcher();
  }

  void start()
  {
    client_->async_start();
  }

  bool ready() const
  {
    return keyboard_ready_ && pointing_ready_;
  }

  // The pqrs client retries its connect every second and reports each miss;
  // at a login window that waits for the daemon that was ~86k lines/day.
  // Log the first failure, then at most one line per kConnectFailedLogEvery
  // carrying the count of the ones suppressed in between.
  static constexpr auto kConnectFailedLogEvery = std::chrono::seconds(60);

  void note_connect_failed(int ec)
  {
    std::lock_guard<std::mutex> lock(connect_failed_mutex_);
    const auto now = Clock::now();
    if (connect_failed_logged_ && now - last_connect_failed_log_ < kConnectFailedLogEvery) {
      ++connect_failed_suppressed_;
      return;
    }
    std::string line = "vhid connect_failed: " + std::to_string(ec);
    if (connect_failed_suppressed_ > 0)
      line += " (" + std::to_string(connect_failed_suppressed_) + " more suppressed in the last 60s)";
    log_line(line);
    connect_failed_logged_ = true;
    connect_failed_suppressed_ = 0;
    last_connect_failed_log_ = now;
  }

  // Waits up to `timeout` for both virtual devices; returns early (false)
  // on SIGTERM so an unbounded wait still exits promptly.
  bool wait_ready(milliseconds timeout)
  {
    Clock::time_point deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
      if (ready())
        return true;
      if (g_stop.load())
        return false;
      std::this_thread::sleep_for(milliseconds(20));
    }
    return ready();
  }

  // Caps Lock state as reported by the Karabiner VIRTUAL keyboard's own
  // service ("HIDCapsLockState"). This is the keyboard the bridge itself
  // toggles, so it is the least trustworthy of the three sources and is
  // consulted last. The service handle lives here (not in a process-lifetime
  // static) so a daemon reconnect resets it and it is re-resolved.
  std::optional<bool> caps_lock_state_from_virtual_keyboard()
  {
    std::lock_guard<std::mutex> lock(keyboard_service_mutex_);
    if (!keyboard_service_.service) {
      keyboard_service_ = find_karabiner_service(
          kKarabinerKeyboardProductId, kHIDPage_GenericDesktop, kHIDUsage_GD_Keyboard, "keyboard",
          !keyboard_service_logged_miss_
      );
      keyboard_service_logged_miss_ = true;
    }
    if (!keyboard_service_.service)
      return std::nullopt;
    cf_ptr<CFTypeRef> value(
        IOHIDServiceClientCopyProperty(keyboard_service_.service, CFSTR(kIOHIDServiceCapsLockStateKey))
    );
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

  // -- IReportSink ---------------------------------------------------------
  void post_keyboard(uint8_t modifier_bits, const std::set<uint16_t> &keys) override
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

  void post_pointing(
      const std::set<uint8_t> &buttons, int8_t dx, int8_t dy, int8_t vertical_wheel, int8_t horizontal_wheel
  ) override
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

  CapsTruth caps_lock_state() override
  {
    if (auto s = caps_state_cg_flags())
      return {s, "cg-flags"};
    if (auto s = caps_state_legacy_iohidsystem())
      return {s, "iohidsystem"};
    if (auto s = caps_lock_state_from_virtual_keyboard())
      return {s, "vkbd-service"};
    return {};
  }

  std::optional<CursorPoint> cursor_position() override
  {
    return read_cursor_position();
  }

  bool main_display(int16_t &width, int16_t &height, double &backing_scale) override
  {
    return query_main_display(width, height, backing_scale);
  }

private:
  std::unique_ptr<pqrs::karabiner::driverkit::virtual_hid_device_service::client> client_;
  std::atomic<bool> keyboard_ready_{false};
  std::atomic<bool> pointing_ready_{false};
  std::atomic<bool> connected_before_{false};
  std::atomic<bool> reapply_accel_on_ready_{false};
  // Resolved lazily by caps_lock_state_from_virtual_keyboard(); reset by the
  // `connected` slot (dispatcher thread), hence the mutex.
  std::mutex keyboard_service_mutex_;
  HidServiceHandle keyboard_service_;
  bool keyboard_service_logged_miss_ = false;
  std::mutex connect_failed_mutex_;
  bool connect_failed_logged_ = false;
  unsigned long connect_failed_suppressed_ = 0;
  Clock::time_point last_connect_failed_log_{};
};

} // namespace

int main(int argc, char **argv)
{
  ::umask(077);
  if (::pipe(g_wake_pipe) != 0) {
    log_line("pipe() failed: " + std::string(std::strerror(errno)));
    return 1;
  }
  for (int fd : g_wake_pipe) {
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  }

  // Split flag arguments (--size=WxH, --scale=S) from positionals so the
  // launchd plist generated by the GUI can pass options without having to
  // fill every preceding positional slot. Legacy positional forms still work.
  std::vector<std::string> positional;
  std::optional<int16_t> flag_w, flag_h;
  std::optional<double> flag_scale;
  std::optional<uint16_t> flag_coord_port;
  bool scale_fixed = false, calibrate = false;
  // Seconds to wait for the Karabiner daemon; 0 = forever (the default: at a
  // cold-boot login window the daemon comes up after us, and the pqrs client
  // retries its connect internally).
  long vhid_wait_s = 0;
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
    } else if (arg == "--debug-keys") {
      g_debug_keys = true;
    } else if (arg.rfind("--vhid-wait-s=", 0) == 0) {
      char *end = nullptr;
      const long parsed = std::strtol(arg.c_str() + 14, &end, 10);
      if (end == arg.c_str() + 14 || *end != '\0' || parsed < 0) {
        log_line("invalid --vhid-wait-s (expected a non-negative integer): " + arg);
        return 2;
      }
      vhid_wait_s = parsed;
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
        "[--coord-port=N] [--vhid-wait-s=N] [--debug-keys]"
    );
    return 2;
  }

  // Exactly one bridge per machine may own the virtual HID device. A second
  // launch (launchd retry racing a still-draining predecessor, or a manual
  // start) exits cleanly so launchd does not treat it as a crash loop. The
  // lock is a kernel-released flock, held until this process dies.
  using deskflow::SingleInstanceLock;
  const auto instanceLock =
      SingleInstanceLock::tryAcquire(SingleInstanceLock::Role::VhidBridge, SingleInstanceLock::Scope::Machine);
  if (!instanceLock) {
    log_line("another deskflow-vhid-bridge is already running: " + SingleInstanceLock::lastMessage());
    return 0;
  }
  if (const auto msg = SingleInstanceLock::lastMessage(); !msg.empty()) {
    log_line(msg);
  }
  // fleet-health keys off this line: "virtual HID ready" must follow it
  // within 30 s, with no vhid connect_failed in between.
  log_line("starting pid=" + std::to_string(::getpid()));

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
  sa.sa_handler = [](int) {
    g_stop.store(true);
    wake();
  };
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);
  signal(SIGPIPE, SIG_IGN);

  VirtualHidSink sink;
  sink.start();
  // Wait for the daemon: unbounded by default (launchd starts us at the
  // login window before the Karabiner daemon is listening -- the old 10 s
  // bail-out logged "connect_failed: 61" and exited 1 into a KeepAlive
  // respawn loop). Progress is logged (5 s, then 60 s); --vhid-wait-s=N bounds it.
  {
    const Clock::time_point started = Clock::now();
    long last_logged_s = 0;
    while (!sink.ready()) {
      if (g_stop.load()) {
        log_line("stopping (signal) while waiting for the virtual HID daemon");
        return 0;
      }
      sink.wait_ready(milliseconds(1000));
      if (sink.ready())
        break;
      const long elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started).count();
      if (vhid_wait_s > 0 && elapsed_s >= vhid_wait_s) {
        log_line(
            "virtual HID device not ready after " + std::to_string(elapsed_s) +
            "s (is the Karabiner daemon running?); giving up (--vhid-wait-s=" + std::to_string(vhid_wait_s) + ")"
        );
        return 1;
      }
      // Every 5 s for the first minute, then every 60 s: together with the
      // rate-limited connect_failed line that is ~2 lines/min, so the
      // "starting" line stays within fleet-health's reach for hours.
      const long progress_every_s = elapsed_s < 60 ? 5 : 60;
      if (elapsed_s - last_logged_s >= progress_every_s) {
        last_logged_s = elapsed_s;
        log_line("waiting for virtual HID daemon (" + std::to_string(elapsed_s) + "s)");
      }
    }
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
  start_console_user_watch();
  const uint16_t coord_port = flag_coord_port.value_or(0);
  // Cycle the candidate list; back off only after a full pass with no server
  // accepting, so a role flip to any peer is picked up within one pass.
  int backoff_ms = 500;
  size_t host_idx = 0;
  while (!g_stop.load()) {
    if (g_stand_down.load()) {
      wait_or_wake(1000);
      continue;
    }
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
        wait_or_wake(backoff_ms);
        backoff_ms = std::min(backoff_ms * 2, 5000);
      }
      continue;
    }
    backoff_ms = 500;
    log_line("connected to host " + host);
    FramedSocket socket(fd);
    bridge.run(socket);
    log_line("host connection closed");
    drain_wake_pipe();
  }
  // Falls out on SIGTERM/SIGINT and on the keyboard rescue; `sink` is
  // destroyed after the last release report was queued (A-4).
  log_line("stopping (signal)");
  return 0;
}
