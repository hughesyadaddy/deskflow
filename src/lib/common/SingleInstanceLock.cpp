/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "common/SingleInstanceLock.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
//
#include <sddl.h>
#else
#include <fcntl.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace deskflow {

namespace {

thread_local std::string t_lastMessage;

void setMessage(std::string message)
{
  t_lastMessage = std::move(message);
}

constexpr auto kPollInterval = std::chrono::milliseconds(50);

#if !defined(_WIN32)

std::string homeDir()
{
  if (const char *home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    return home;
  }
  if (const passwd *pw = getpwuid(getuid()); pw != nullptr && pw->pw_dir != nullptr) {
    return pw->pw_dir;
  }
  return {};
}

// mkdir -p with a fixed mode. Returns true if the directory exists afterwards.
bool ensureDir(const std::string &path, mode_t mode)
{
  std::string partial;
  for (size_t i = 1; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      partial = path.substr(0, i);
      if (::mkdir(partial.c_str(), mode) != 0 && errno != EEXIST) {
        return false;
      }
    }
  }
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Test hook: DESKFLOW_LOCK_DIR relocates both scopes under one private dir
// so unit tests never collide with a live core/GUI on the developer box.
std::string overrideDir(const char *scope)
{
  if (const char *dir = std::getenv("DESKFLOW_LOCK_DIR"); dir != nullptr && *dir != '\0') {
    return std::string(dir) + "/" + scope;
  }
  return {};
}

std::string sessionDir()
{
  if (auto dir = overrideDir("session"); !dir.empty()) {
    return dir;
  }
#if defined(__APPLE__)
  const auto home = homeDir();
  if (home.empty()) {
    return {};
  }
  return home + "/Library/Application Support/Deskflow";
#else
  if (const char *runtime = std::getenv("XDG_RUNTIME_DIR"); runtime != nullptr && *runtime != '\0') {
    return std::string(runtime) + "/deskflow";
  }
  const auto home = homeDir();
  if (home.empty()) {
    return {};
  }
  return home + "/.local/state/deskflow";
#endif
}

std::string machineDir()
{
  if (auto dir = overrideDir("machine"); !dir.empty()) {
    return dir;
  }
#if defined(__APPLE__)
  return "/private/var/db/deskflow";
#else
  return "/var/lib/deskflow";
#endif
}

std::string machineFallbackPath(SingleInstanceLock::Role role)
{
  return std::string("/tmp/deskflow-") + SingleInstanceLock::roleName(role) + ".machine.lock";
}

// Open the lock file for a scope. Machine-scope files are world-readable so a
// non-root process can still open them O_RDONLY and flock() (flock does not
// care about the open mode); Session-scope files are private to the user.
// Returns -1 (errno set) if nothing could be opened.
int openLockFile(SingleInstanceLock::Role role, SingleInstanceLock::Scope scope, bool create, std::string &outPath)
{
  const int cloexec = O_CLOEXEC;
  if (scope == SingleInstanceLock::Scope::Session) {
    const auto dir = sessionDir();
    if (dir.empty()) {
      errno = ENOENT;
      return -1;
    }
    if (create && !ensureDir(dir, 0700)) {
      return -1;
    }
    outPath = dir + "/" + SingleInstanceLock::roleName(role) + ".lock";
    const int flags = O_RDWR | cloexec | (create ? O_CREAT : 0);
    return ::open(outPath.c_str(), flags, 0600);
  }

  // Machine scope: preferred system dir, then /tmp fallback.
  const auto dir = machineDir();
  outPath = dir + "/" + SingleInstanceLock::roleName(role) + ".machine.lock";
  if (create && ensureDir(dir, 01777)) {
    // Only root can normally create it. Make it /tmp-like (sticky, world
    // writable, umask-proof) so a later user-session process can create its
    // own role file there and actually collide with the root holder instead
    // of silently falling back to /tmp.
    struct stat st{};
    if (::stat(dir.c_str(), &st) == 0 && st.st_uid == geteuid() && (st.st_mode & 07777) != 01777) {
      ::chmod(dir.c_str(), 01777);
    }
  }
  const int rwFlags = O_RDWR | cloexec | (create ? O_CREAT : 0);
  int fd = ::open(outPath.c_str(), rwFlags, 0644);
  if (fd < 0 && errno == EACCES) {
    // Exists but owned by someone else (root); read-only is enough for flock.
    fd = ::open(outPath.c_str(), O_RDONLY | cloexec);
  }
  if (fd >= 0) {
    // umask-proof (the root bridge runs with umask 077): a private root lock file
    // would push every user-session core to the /tmp fallback.
    struct stat st{};
    if (::fstat(fd, &st) == 0 && st.st_uid == geteuid() && (st.st_mode & 0444) != 0444) {
      ::fchmod(fd, 0644);
    }
    return fd;
  }
  const int primaryErrno = errno;

  const auto fallback = machineFallbackPath(role);
  fd = ::open(fallback.c_str(), rwFlags, 0644);
  if (fd < 0 && errno == EACCES) {
    fd = ::open(fallback.c_str(), O_RDONLY | cloexec);
  }
  if (fd >= 0) {
    setMessage(
        "machine lock dir " + dir + " not usable (" + std::strerror(primaryErrno) + "); using " + fallback +
        " -- cross-user duplicate detection is weaker on this host"
    );
    outPath = fallback;
    return fd;
  }
  return -1;
}

void writePid(int fd)
{
  // Diagnostics only. Fails silently on a read-only fd.
  if (::ftruncate(fd, 0) != 0) {
    return;
  }
  char buf[32];
  const int len = std::snprintf(buf, sizeof(buf), "%ld\n", static_cast<long>(getpid()));
  if (len > 0) {
    [[maybe_unused]] const auto written = ::pwrite(fd, buf, static_cast<size_t>(len), 0);
  }
}

#else // _WIN32

// SYSTEM + Administrators: full. Everyone: SYNCHRONIZE|MUTEX_MODIFY_STATE
// (0x100001) so a lower-integrity opener gets ERROR_ALREADY_EXISTS instead
// of a spurious ERROR_ACCESS_DENIED.
constexpr const wchar_t *kMutexSddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x100001;;;WD)";

std::wstring toWide(const std::string &s)
{
  if (s.empty()) {
    return {};
  }
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

struct SecurityDescriptorHolder
{
  PSECURITY_DESCRIPTOR sd = nullptr;
  SECURITY_ATTRIBUTES sa{};

  SecurityDescriptorHolder()
  {
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(kMutexSddl, SDDL_REVISION_1, &sd, nullptr)) {
      sa.nLength = sizeof(sa);
      sa.lpSecurityDescriptor = sd;
      sa.bInheritHandle = FALSE;
    }
  }
  ~SecurityDescriptorHolder()
  {
    if (sd != nullptr) {
      LocalFree(sd);
    }
  }
  SECURITY_ATTRIBUTES *attributes()
  {
    return sd != nullptr ? &sa : nullptr;
  }
};

#endif

} // namespace

const char *SingleInstanceLock::roleName(Role role)
{
  switch (role) {
  case Role::Core:
    return "core";
  case Role::Gui:
    return "gui";
  case Role::Daemon:
    return "daemon";
  case Role::VhidBridge:
    return "vhid-bridge";
  }
  return "unknown";
}

std::string SingleInstanceLock::lastMessage()
{
  return t_lastMessage;
}

// Lock names are per role on every scope, including Machine. A Machine lock
// therefore serializes same-role instances across users/sessions (core vs
// core: root LoginWindow core vs user core) and nothing else: Core and
// VhidBridge never contend, so the bridge is not excluded by this lock. On
// macOS bridge-vs-core exclusion is provided by launchd tearing down the
// LoginWindow session at login plus the bridge's release_all() on exit.
std::string SingleInstanceLock::lockName(Role role, Scope scope)
{
#if defined(_WIN32)
  // Global\ crosses sessions and integrity levels; Local\ is per-session.
  std::string name = (scope == Scope::Machine) ? "Global\\Deskflow." : "Local\\Deskflow.";
  switch (role) {
  case Role::Core:
    name += "Core";
    break;
  case Role::Gui:
    name += "Gui";
    break;
  case Role::Daemon:
    name += "Daemon";
    break;
  case Role::VhidBridge:
    name += "VhidBridge";
    break;
  }
  // Same test hook as the POSIX dirs: namespace the mutex per test dir.
  if (const char *dir = std::getenv("DESKFLOW_LOCK_DIR"); dir != nullptr && *dir != '\0') {
    name += '.';
    for (const char *p = dir; *p != '\0'; ++p) {
      name += std::isalnum(static_cast<unsigned char>(*p)) ? *p : '_';
    }
  }
  return name;
#else
  if (scope == Scope::Session) {
    const auto dir = sessionDir();
    return dir.empty() ? std::string{} : dir + "/" + roleName(role) + ".lock";
  }
  return machineDir() + "/" + roleName(role) + ".machine.lock";
#endif
}

std::optional<SingleInstanceLock> SingleInstanceLock::tryAcquire(Role role, Scope scope)
{
  return tryAcquire(role, scope, std::chrono::milliseconds(0));
}

std::optional<SingleInstanceLock> SingleInstanceLock::tryAcquire(Role role, Scope scope, std::chrono::milliseconds wait)
{
  setMessage({});
  const auto deadline = std::chrono::steady_clock::now() + wait;

#if defined(_WIN32)
  const auto name = lockName(role, scope);
  const auto wname = toWide(name);
  SecurityDescriptorHolder sd;

  for (;;) {
    HANDLE h = CreateMutexW(sd.attributes(), TRUE, wname.c_str());
    const DWORD err = GetLastError();
    if (h != nullptr && err != ERROR_ALREADY_EXISTS) {
      SingleInstanceLock lock;
      lock.m_handle = h;
      lock.m_name = name;
      return lock;
    }
    if (h != nullptr) {
      CloseHandle(h); // exists: someone else owns it
    } else if (err != ERROR_ACCESS_DENIED) {
      setMessage("CreateMutexW(" + name + ") failed: " + std::to_string(err));
      return std::nullopt;
    }
    // ERROR_ALREADY_EXISTS or ERROR_ACCESS_DENIED (higher-integrity holder):
    // both mean "held".
    if (std::chrono::steady_clock::now() >= deadline) {
      setMessage("lock " + name + " is held by another process");
      return std::nullopt;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
#else
  std::string path;
  const int fd = openLockFile(role, scope, true, path);
  if (fd < 0) {
    setMessage("cannot open lock file " + (path.empty() ? std::string("<none>") : path) + ": " + std::strerror(errno));
    return std::nullopt;
  }

  for (;;) {
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
      writePid(fd);
      SingleInstanceLock lock;
      lock.m_fd = fd;
      lock.m_name = path;
      return lock;
    }
    if (errno != EWOULDBLOCK && errno != EINTR) {
      const int e = errno;
      ::close(fd);
      setMessage("flock(" + path + ") failed: " + std::strerror(e));
      return std::nullopt;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      ::close(fd);
      setMessage("lock " + path + " is held by another process");
      return std::nullopt;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
#endif
}

bool SingleInstanceLock::isHeld(Role role, Scope scope)
{
  setMessage({});
#if defined(_WIN32)
  const auto wname = toWide(lockName(role, scope));
  HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, wname.c_str());
  if (h != nullptr) {
    // The holder keeps its handle for life, so existence == held.
    CloseHandle(h);
    return true;
  }
  return GetLastError() == ERROR_ACCESS_DENIED;
#else
  std::string path;
  const int fd = openLockFile(role, scope, false, path);
  if (fd < 0) {
    return false; // no file: nobody can be holding it
  }
  bool held = false;
  if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
    ::flock(fd, LOCK_UN);
  } else {
    held = (errno == EWOULDBLOCK);
  }
  ::close(fd);
  return held;
#endif
}

SingleInstanceLock::SingleInstanceLock(SingleInstanceLock &&other) noexcept
    : m_name(std::move(other.m_name))
{
#if defined(_WIN32)
  m_handle = std::exchange(other.m_handle, nullptr);
#else
  m_fd = std::exchange(other.m_fd, -1);
#endif
}

SingleInstanceLock &SingleInstanceLock::operator=(SingleInstanceLock &&other) noexcept
{
  if (this != &other) {
    release();
    m_name = std::move(other.m_name);
#if defined(_WIN32)
    m_handle = std::exchange(other.m_handle, nullptr);
#else
    m_fd = std::exchange(other.m_fd, -1);
#endif
  }
  return *this;
}

SingleInstanceLock::~SingleInstanceLock()
{
  release();
}

void SingleInstanceLock::release() noexcept
{
#if defined(_WIN32)
  if (m_handle != nullptr) {
    ReleaseMutex(m_handle);
    CloseHandle(m_handle);
    m_handle = nullptr;
  }
#else
  if (m_fd >= 0) {
    // Closing the fd drops the flock; the file itself is left in place on
    // purpose (unlinking would let a racer take a lock on a dead inode).
    ::close(m_fd);
    m_fd = -1;
  }
#endif
}

} // namespace deskflow
