/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/EscDropout.h"

#include "base/Log.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

namespace deskflow::coordination {

namespace {
int64_t nowEpoch()
{
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}
} // namespace

std::string dropoutFilePath()
{
#if defined(_WIN32)
  // the core runs as the logged-in user: its own LOCALAPPDATA is writable
  const char *base = std::getenv("LOCALAPPDATA");
  if (base == nullptr) {
    base = std::getenv("TEMP");
  }
  return std::string(base != nullptr ? base : "C:\\Windows\\Temp") + "\\deskflow-dropout-until";
#else
  const char *home = std::getenv("HOME");
  return std::string(home != nullptr ? home : "/tmp") + "/Library/Deskflow/dropout-until";
#endif
}

bool writeDropoutDeadline(int seconds)
{
  std::ofstream out(dropoutFilePath(), std::ios::trunc);
  if (!out) {
    return false;
  }
  out << (nowEpoch() + seconds) << "\n";
  return static_cast<bool>(out);
}

int64_t dropoutSecondsLeft(const std::string &fileContents, int64_t now)
{
  std::istringstream in(fileContents);
  int64_t deadline = 0;
  if (!(in >> deadline) || deadline <= now) {
    return 0;
  }
  const int64_t left = deadline - now;
  return left > kDropoutMaxWaitSeconds ? kDropoutMaxWaitSeconds : left;
}

void waitOutDropoutDeadline(const char *role)
{
  const auto path = dropoutFilePath();
  std::ifstream in(path);
  if (!in) {
    return;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  in.close();
  int64_t left = dropoutSecondsLeft(ss.str(), nowEpoch());
  if (left > 0) {
    LOG_INFO("[esc-dropout] %s staying out of Deskflow for %lld more s (Esc x5)", role, static_cast<long long>(left));
    while (left > 0) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      std::ifstream again(path);
      if (!again) {
        break; // file removed by hand: come back now
      }
      --left;
    }
  }
  std::remove(path.c_str());
  LOG_INFO("[esc-dropout] %s rejoining Deskflow (pinned role unchanged)", role);
}

} // namespace deskflow::coordination
