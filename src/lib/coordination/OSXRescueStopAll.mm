/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/RescueStopAll.h"

#include "base/Log.h"
#include "coordination/KeyboardRescue.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <libproc.h>
#include <signal.h>
#include <spawn.h>
#include <sys/proc_info.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

extern char **environ;

namespace deskflow::coordination {

namespace {

constexpr int kLaunchctlWaitMs = 5000;
//! deskflow-ctl stop: bootout + wait_gone (10 s) + TERM/KILL escalation
//! (5 s each). We are normally dead long before this.
constexpr int kCanonicalStopWaitMs = 25000;

std::string homeDir()
{
  return QDir::homePath().toStdString();
}

std::string ownExecutablePath()
{
  char buffer[PROC_PIDPATHINFO_MAXSIZE] = {};
  if (proc_pidpath(getpid(), buffer, sizeof(buffer)) <= 0) {
    return {};
  }
  return buffer;
}

//! `.../Deskflow.app/Contents/MacOS/` of the bundle this core runs from,
//! else the canonical install location.
std::string bundleMacOSDir()
{
  static const std::string marker = "Deskflow.app/Contents/MacOS/";
  const auto self = ownExecutablePath();
  if (const auto pos = self.rfind(marker); pos != std::string::npos) {
    return self.substr(0, pos + marker.size());
  }
  return "/Applications/" + marker;
}

std::string canonicalCtlPath()
{
  // Same override the script honours (tests); the launchd-safe copy lives
  // outside ~/Desktop so TCC never blocks it (K10).
  const char *safeDir = std::getenv("DESKFLOW_CTL_SAFE_DIR");
  const std::string dir = (safeDir != nullptr && *safeDir != '\0') ? safeDir : homeDir() + "/Library/Deskflow/bin";
  return dir + "/deskflow-ctl";
}

//! Spawn \p argv in its own session with a clean fd table (only stdio is
//! inherited): the child must never hold this core's listening sockets,
//! or the next core start fails with EADDRINUSE while it lingers.
pid_t spawnDetached(const std::vector<std::string> &argv)
{
  std::vector<char *> args;
  args.reserve(argv.size() + 1);
  for (const auto &arg : argv) {
    args.push_back(const_cast<char *>(arg.c_str()));
  }
  args.push_back(nullptr);

  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  for (int fd = 0; fd <= 2; ++fd) {
    posix_spawn_file_actions_addinherit_np(&actions, fd);
  }

  pid_t pid = -1;
  const int rc = posix_spawn(&pid, args[0], &actions, &attr, args.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attr);
  if (rc != 0) {
    LOG_WARN("[rescue] could not spawn %s: %s", args[0], std::strerror(rc));
    return -1;
  }
  return pid;
}

//! Wait up to \p timeoutMs for \p pid; returns its exit code, or -1 when it
//! is still running (left alone: never kill a stop script mid-way).
int waitBounded(pid_t pid, int timeoutMs)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (true) {
    int status = 0;
    const pid_t done = waitpid(pid, &status, WNOHANG);
    if (done == pid) {
      return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    if (done < 0 && errno != EINTR) {
      return -1;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return -1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

class OSXStopAllCommands final : public IStopAllCommands
{
public:
  bool writeQuitIntent() override
  {
    const QString path = QString::fromStdString(homeDir() + "/Library/Application Support/Deskflow/quit-intent");
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
      return false;
    }
    QFile sentinel(path);
    if (!sentinel.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      return false;
    }
    // Same shape as deskflow-ctl's `date +%s`; converge judges it by mtime.
    sentinel.write(QByteArray::number(static_cast<qint64>(std::time(nullptr))) + '\n');
    return true;
  }

  bool canonicalStopAvailable() override
  {
    const QFileInfo ctl(QString::fromStdString(canonicalCtlPath()));
    return ctl.isFile() && ctl.isExecutable();
  }

  bool runCanonicalStop() override
  {
    const auto path = canonicalCtlPath();
    const pid_t pid = spawnDetached({path, "stop"});
    if (pid < 0) {
      return false;
    }
    const int code = waitBounded(pid, kCanonicalStopWaitMs);
    LOG_INFO("[rescue] %s stop exited with %d", path.c_str(), code);
    return code == 0;
  }

  bool bootout(const std::string &label, bool wait) override
  {
    const std::string target = "gui/" + std::to_string(getuid()) + "/" + label;
    LOG_INFO("[rescue] launchctl bootout %s", target.c_str());
    const pid_t pid = spawnDetached({"/bin/launchctl", "bootout", target});
    if (pid < 0) {
      return false;
    }
    if (!wait) {
      return true;
    }
    const int code = waitBounded(pid, kLaunchctlWaitMs);
    if (code != 0) {
      // Non-zero also covers "not loaded" (nothing to stop); fine either way.
      LOG_DEBUG("[rescue] launchctl bootout %s exited with %d", target.c_str(), code);
    }
    return code == 0;
  }

  std::vector<int> strayPids() override
  {
    std::vector<int> result;
    const std::string dir = bundleMacOSDir();
    const int bytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (bytes <= 0) {
      return result;
    }
    std::vector<pid_t> pids(static_cast<size_t>(bytes) / sizeof(pid_t) + 64);
    const int filled = proc_listpids(PROC_ALL_PIDS, 0, pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    if (filled <= 0) {
      return result;
    }
    const size_t count = static_cast<size_t>(filled) / sizeof(pid_t);
    const pid_t self = getpid();
    const uid_t uid = getuid();
    for (size_t i = 0; i < count; ++i) {
      const pid_t pid = pids[i];
      if (pid <= 0 || pid == self) {
        continue;
      }
      char path[PROC_PIDPATHINFO_MAXSIZE] = {};
      if (proc_pidpath(pid, path, sizeof(path)) <= 0) {
        continue;
      }
      const std::string exe = path;
      if (exe.compare(0, dir.size(), dir) != 0) {
        continue;
      }
      const std::string base = exe.substr(dir.size());
      if (base != "Deskflow" && base != "deskflow-core") {
        continue;
      }
      // Never the root login-window bridge or another user's session.
      proc_bsdinfo info{};
      if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info) || info.pbi_uid != uid) {
        continue;
      }
      result.push_back(static_cast<int>(pid));
    }
    return result;
  }

  void terminate(int pid, bool force) override
  {
    if (::kill(static_cast<pid_t>(pid), force ? SIGKILL : SIGTERM) != 0 && errno != ESRCH) {
      LOG_WARN("[rescue] kill(%d) failed: %s", pid, std::strerror(errno));
    }
  }

  void sleepMs(int ms) override
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  }

  void quitSelf() override
  {
    requestLocalCoreQuit();
  }
};

} // namespace

void runMacStopAll(const std::string &seat)
{
  LOG_INFO("[rescue] stop-all on %s: quit-intent, converge, GUI, strays, then this core", seat.c_str());
  OSXStopAllCommands commands;
  runMacStopAllSequence(commands);
}

} // namespace deskflow::coordination
