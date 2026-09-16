/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace deskflow {

/**
 * @brief Process-lifetime single-instance guard.
 *
 * Deliberately Qt-free so the non-Qt vhid bridges can compile it in.
 *
 * POSIX: an `O_CREAT|O_CLOEXEC` fd + `flock(LOCK_EX)`. The kernel drops the
 * lock when the process dies, so a crash never leaves a stale lock (unlike
 * SysV shared memory or a QLocalServer socket file). The pid is written after
 * acquisition for diagnostics only; nothing trusts it.
 *
 * Windows: a named mutex in the `Global\` namespace with an explicit DACL so
 * a second open from any session / integrity level deterministically returns
 * ERROR_ALREADY_EXISTS (or ERROR_ACCESS_DENIED, which is also "held").
 *
 * Scope::Session   -- one per logged-in user (per-user state dir, 0600).
 * Scope::Machine   -- one per machine (LoginWindow/root vs user-session
 *                     collide, which is the whole point). The file is 0644
 *                     in a sticky world-writable dir so a non-root process
 *                     can still open it read-only and flock() it; if that
 *                     dir is unusable it falls back to /tmp with a warning.
 *
 * Test hook: if DESKFLOW_LOCK_DIR is set, both scopes live under it (POSIX)
 * or the mutex names are suffixed with it (Windows).
 */
class SingleInstanceLock
{
public:
  enum class Role
  {
    Core,
    Gui,
    Daemon,
    VhidBridge
  };

  enum class Scope
  {
    Session,
    Machine
  };

  /**
   * @brief Try to take the lock without blocking.
   * @return the held lock, or nullopt if another process holds it (or the
   *         lock could not be created at all -- see lastMessage()).
   */
  static std::optional<SingleInstanceLock> tryAcquire(Role role, Scope scope);

  /**
   * @brief Like tryAcquire(), but retries for up to @p wait so a previous
   *        holder that is shutting down (e.g. a LoginWindow bridge handoff)
   *        can drain. Never blocks past the deadline.
   */
  static std::optional<SingleInstanceLock> tryAcquire(Role role, Scope scope, std::chrono::milliseconds wait);

  /**
   * @brief Probe whether another process currently holds the lock, without
   *        acquiring it. Racy by nature; for diagnostics and tests.
   */
  static bool isHeld(Role role, Scope scope);

  /**
   * @brief Last warning/error produced by tryAcquire()/isHeld() on this
   *        thread (e.g. the machine dir fallback). Empty if none.
   */
  static std::string lastMessage();

  /// Path (POSIX) or mutex name (Windows) backing the lock.
  static std::string lockName(Role role, Scope scope);

  static const char *roleName(Role role);

  SingleInstanceLock(SingleInstanceLock &&other) noexcept;
  SingleInstanceLock &operator=(SingleInstanceLock &&other) noexcept;
  SingleInstanceLock(const SingleInstanceLock &) = delete;
  SingleInstanceLock &operator=(const SingleInstanceLock &) = delete;
  ~SingleInstanceLock();

  /// Explicit release (the destructor does this too).
  void release() noexcept;

  /// Where the lock lives; useful for log lines.
  const std::string &name() const
  {
    return m_name;
  }

private:
  SingleInstanceLock() = default;

#if defined(_WIN32)
  void *m_handle = nullptr; // HANDLE
#else
  int m_fd = -1;
#endif
  std::string m_name;
};

} // namespace deskflow
