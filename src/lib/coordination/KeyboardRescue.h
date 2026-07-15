/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/KeyTypes.h"

#include <chrono>

namespace deskflow::coordination {

//! Five plain Escape downs within 2s on the keyboard host → soft-restart
//! local Deskflow core via the deskflow-core-registered restart handler.
/*!
Call only for KeyDown / KeyPhase::Down (not Repeat). Chord modifiers
(Shift/Ctrl/Alt/Super) must be clear; Caps/NumLock are ignored. Rolling
window is measured from the last counted Esc tap.
*/
struct EscTapRescue
{
  static constexpr int kTaps = 5;
  static constexpr double kWindowSec = 2.0;

  using Clock = std::chrono::steady_clock;

  bool noteEscDown(KeyID id, KeyModifierMask mask, Clock::time_point now = Clock::now())
  {
    constexpr KeyModifierMask chordMods = KeyModifierShift | KeyModifierControl | KeyModifierAlt | KeyModifierSuper;
    if (id != kKeyEscape || (mask & chordMods) != 0) {
      // Non-Esc (or Esc with chord mods) breaks the streak; Caps/Num on plain Esc still count.
      reset();
      return false;
    }

    if (m_count > 0) {
      const auto elapsed = std::chrono::duration<double>(now - m_last).count();
      if (elapsed > kWindowSec) {
        m_count = 0;
      }
    }

    m_last = now;
    ++m_count;
    if (m_count < kTaps) {
      return false;
    }
    reset();
    return true;
  }

  void reset()
  {
    m_count = 0;
    m_last = {};
  }

  int count() const
  {
    return m_count;
  }

private:
  int m_count = 0;
  Clock::time_point m_last{};
};

//! Process-wide soft-restart hook (set by deskflow-core to call Core IPC).
using LocalCoreRestartFn = void (*)();
void setLocalCoreRestartHandler(LocalCoreRestartFn fn);
void requestLocalCoreRestart();

} // namespace deskflow::coordination
