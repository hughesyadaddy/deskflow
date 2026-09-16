/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "PermissionCheck.h"

#include <cstring>

namespace deskflow::core::permissions {

namespace {

bool hasFlag(int argc, char **argv, const char *flag)
{
  for (int i = 1; i < argc; ++i) {
    if (argv[i] != nullptr && std::strcmp(argv[i], flag) == 0) {
      return true;
    }
  }
  return false;
}

const char *word(bool granted)
{
  return granted ? "granted" : "denied";
}

const char *boolean(bool value)
{
  return value ? "true" : "false";
}

} // namespace

#if !defined(__APPLE__)
Status probe()
{
  return {};
}
#endif

bool requested(int argc, char **argv)
{
  return hasFlag(argc, argv, "--check-permissions");
}

bool wantsJson(int argc, char **argv)
{
  return hasFlag(argc, argv, "--json");
}

std::string render(const Status &status, bool json)
{
  std::string out;
  if (json) {
    out += "{\"supported\":";
    out += boolean(status.supported);
    if (status.supported) {
      out += ",\"accessibility\":\"";
      out += word(status.accessibility);
      out += "\",\"input-monitoring\":\"";
      out += word(status.inputMonitoring);
      out += "\",\"post-event\":\"";
      out += word(status.postEvent);
      out += "\"";
    }
    out += "}\n";
    return out;
  }

  if (!status.supported) {
    return "unsupported\n";
  }
  out += "accessibility=";
  out += word(status.accessibility);
  out += "\ninput-monitoring=";
  out += word(status.inputMonitoring);
  out += "\npost-event=";
  out += word(status.postEvent);
  out += "\n";
  return out;
}

int exitCodeFor(const Status &status)
{
  if (!status.supported) {
    return kExitUnsupported;
  }
  return (status.accessibility && status.inputMonitoring) ? kExitGranted : kExitDenied;
}

int run(int argc, char **argv, Probe probeFn, std::ostream &out)
{
  if (!requested(argc, argv)) {
    return -1;
  }
  const Status status = probeFn != nullptr ? probeFn() : Status{};
  out << render(status, wantsJson(argc, argv));
  out.flush();
  return exitCodeFor(status);
}

} // namespace deskflow::core::permissions
