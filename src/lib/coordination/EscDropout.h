/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace deskflow::coordination {

//! Esc x5 local dropout: 5 Esc presses on THIS machine's physical keyboard
//! within kWindow make this seat leave Deskflow for kDropoutSeconds (release
//! held keys, disconnect / stop serving, exit; the supervisor respawns the
//! core, which waits out the deadline file before serving/connecting again).
class EscTapBurst
{
public:
  using Clock = std::chrono::steady_clock;
  static constexpr int kTaps = 5;
  static constexpr std::chrono::milliseconds kWindow{1500};

  //! A physical key DOWN (repeats excluded by the caller or flagged here).
  //! Returns true exactly on the 5th qualifying Esc; the burst then resets.
  bool noteKeyDown(bool isEsc, bool isRepeat, Clock::time_point now)
  {
    if (isRepeat) {
      return false; // holding Esc is not tapping it
    }
    if (!isEsc) {
      m_count = 0;
      return false;
    }
    if (m_count == 0 || now - m_first > kWindow) {
      m_count = 0;
      m_first = now;
    }
    if (++m_count >= kTaps) {
      m_count = 0;
      return true;
    }
    return false;
  }

private:
  int m_count = 0;
  Clock::time_point m_first{};
};

inline constexpr int kDropoutSeconds = 60;
//! hard ceiling on the startup wait, whatever the file says (clock jumps)
inline constexpr int kDropoutMaxWaitSeconds = 75;

//! ~/Library/Deskflow/dropout-until (macOS), %ProgramData%\Deskflow\dropout-until (Windows)
std::string dropoutFilePath();
//! write now+seconds (unix epoch) to the deadline file
bool writeDropoutDeadline(int seconds = kDropoutSeconds);
//! seconds left on the deadline file (0 = none/expired); pure parse helper
int64_t dropoutSecondsLeft(const std::string &fileContents, int64_t nowEpoch);
//! startup gate: if a deadline is pending, log and sleep it out (bounded),
//! then remove the file and log the reconnect
void waitOutDropoutDeadline(const char *role);

//! Lowest-level LOCAL keyboard observer (listen-only; never swallows, never
//! sees injected keys). Calls onBurst (from its own thread) on Esc x5.
class IEscDropoutMonitor
{
public:
  virtual ~IEscDropoutMonitor() = default;
  virtual bool start(std::function<void()> onBurst) = 0;
  virtual void stop() = 0;
};

std::unique_ptr<IEscDropoutMonitor> createEscDropoutMonitor();

#if defined(_WIN32)
//! Windows: every WH_KEYBOARD_LL hook in this process (the dropout monitor's
//! own AND the keyboard relay's, which runs first and may swallow) reports
//! each key here; duplicates of the same OS event are dropped by (time, vk).
void escDropoutObserveWindowsKey(unsigned vk, bool down, bool injected, unsigned long eventTime);
#endif

} // namespace deskflow::coordination
