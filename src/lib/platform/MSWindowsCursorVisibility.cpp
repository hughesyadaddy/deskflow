/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MSWindowsCursorVisibility.h"

#include "arch/win32/XArchWindows.h"
#include "base/Log.h"

namespace deskflow::platform::mswindows {

bool setCursorVisibility(bool visible)
{
  LOG_DEBUG("%s cursor", visible ? "showing" : "hiding");

  const int max = 10;
  int attempts = 0;
  while (attempts++ < max) {
    const auto displayCounter = ShowCursor(visible ? TRUE : FALSE);
    LOG_VERBOSE("cursor display counter: %d", displayCounter);

    if (visible) {
      if (displayCounter >= 0) {
        LOG_VERBOSE("cursor is now visible, attempts: %d", attempts);
        return true;
      }
      LOG_VERBOSE("cursor still hidden, retrying, attempt: %d", attempts);
    } else {
      if (displayCounter < 0) {
        LOG_VERBOSE("cursor is now hidden, attempts: %d", attempts);
        return true;
      }
      LOG_VERBOSE("cursor still visible, retrying, attempt: %d", attempts);
    }
  }

  LOG_ERR("unable to set cursor visibility after %d attempts", attempts);
  return false;
}

} // namespace deskflow::platform::mswindows
