/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "RescueStopAllTests.h"

#include "arch/Arch.h"
#include "base/Log.h"
#include "coordination/KeyboardRescue.h"
#include "coordination/RescueStopAll.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using deskflow::coordination::IStopAllCommands;
using deskflow::coordination::kMacConvergeLabel;
using deskflow::coordination::kMacCoreLabel;
using deskflow::coordination::kMacGuiLabel;
using deskflow::coordination::kQuitExitFallbackMs;
using deskflow::coordination::kStrayTermGraceMs;
using deskflow::coordination::runMacStopAllSequence;

namespace {

Log g_log;
std::unique_ptr<Arch> g_arch;

//! Records every command in order; answers are scripted per test.
struct RecordingCommands final : IStopAllCommands
{
  std::vector<std::string> calls;
  bool quitIntentOk = true;
  bool canonicalAvailable = false;
  bool canonicalOk = false;
  //! Successive strayPids() answers (the last one repeats).
  std::vector<std::vector<int>> strayRounds{{}};
  size_t strayCall = 0;

  bool writeQuitIntent() override
  {
    calls.push_back("quit-intent");
    return quitIntentOk;
  }
  bool canonicalStopAvailable() override
  {
    calls.push_back("canonical?");
    return canonicalAvailable;
  }
  bool spawnCanonicalStop() override
  {
    calls.push_back("canonical-stop");
    return canonicalOk;
  }
  bool bootout(const std::string &label, bool wait) override
  {
    calls.push_back(std::string("bootout ") + label + (wait ? " (wait)" : " (nowait)"));
    return true;
  }
  std::vector<int> strayPids() override
  {
    calls.push_back("strays");
    const auto index = std::min(strayCall, strayRounds.size() - 1);
    ++strayCall;
    return strayRounds[index];
  }
  void terminate(int pid, bool force) override
  {
    calls.push_back(std::string(force ? "kill " : "term ") + std::to_string(pid));
  }
  void sleepMs(int ms) override
  {
    calls.push_back("sleep " + std::to_string(ms));
  }
  void quitSelf() override
  {
    calls.push_back("quit-self");
  }
  void armExitFallback(int ms) override
  {
    calls.push_back("exit-fallback " + std::to_string(ms));
  }
};

//! One line per call: a mismatch then prints the whole sequence.
QString joined(const std::vector<std::string> &calls)
{
  QString out;
  for (const auto &call : calls) {
    out += QString::fromStdString(call) + QLatin1Char('\n');
  }
  return out;
}

template <typename Condition> bool waitFor(Condition condition, int timeoutMs)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (!condition()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

} // namespace

void RescueStopAllTests::initTestCase()
{
  g_arch = std::make_unique<Arch>();
  g_log.setFilter(LogLevel::Level::Error);
}

void RescueStopAllTests::cleanupTestCase()
{
  deskflow::coordination::setLocalStopAllHandler({});
  deskflow::coordination::setLocalCoreQuitHandler({});
  deskflow::coordination::setFleetStopAllHandler(nullptr);
  g_arch.reset();
}

void RescueStopAllTests::init()
{
  deskflow::coordination::resetStopAllStateForTests();
  deskflow::coordination::setLocalStopAllHandler({});
  deskflow::coordination::setLocalCoreQuitHandler({});
  deskflow::coordination::setFleetStopAllHandler(nullptr);
}

void RescueStopAllTests::macSequence_fallbackOrder_quitIntentConvergeGuiStraysCoreLast()
{
  RecordingCommands commands;
  commands.canonicalAvailable = false;
  // Two strays: 42 leaves on SIGTERM, 43 needs SIGKILL.
  commands.strayRounds = {{42, 43}, {43}, {}};

  runMacStopAllSequence(commands);

  const std::vector<std::string> expected = {
      "quit-intent",
      "canonical?",
      std::string("bootout ") + kMacConvergeLabel + " (wait)",
      std::string("bootout ") + kMacGuiLabel + " (wait)",
      "strays",
      "term 42",
      "term 43",
      "sleep " + std::to_string(kStrayTermGraceMs),
      "strays",
      "kill 43",
      std::string("bootout ") + kMacCoreLabel + " (nowait)",
      "quit-self",
      "exit-fallback " + std::to_string(kQuitExitFallbackMs),
  };
  QCOMPARE(joined(commands.calls), joined(expected));
}

void RescueStopAllTests::macSequence_canonicalStop_runsScriptThenQuits()
{
  RecordingCommands commands;
  commands.canonicalAvailable = true;
  commands.canonicalOk = true;

  runMacStopAllSequence(commands);

  // The script is spawned and NOT waited for (it boots this core out as
  // part of its job); we leave through the real quit path at once, with a
  // bounded hard-exit behind it.
  const std::vector<std::string> expected = {
      "quit-intent", "canonical?", "canonical-stop", "quit-self", "exit-fallback " + std::to_string(kQuitExitFallbackMs)
  };
  QCOMPARE(joined(commands.calls), joined(expected));
}

void RescueStopAllTests::macSequence_canonicalFailure_fallsBackInProcess()
{
  RecordingCommands commands;
  commands.canonicalAvailable = true;
  commands.canonicalOk = false;
  commands.quitIntentOk = false; // logged, never fatal

  runMacStopAllSequence(commands);

  const std::vector<std::string> expected = {
      "quit-intent",
      "canonical?",
      "canonical-stop",
      std::string("bootout ") + kMacConvergeLabel + " (wait)",
      std::string("bootout ") + kMacGuiLabel + " (wait)",
      "strays",
      std::string("bootout ") + kMacCoreLabel + " (nowait)",
      "quit-self",
      "exit-fallback " + std::to_string(kQuitExitFallbackMs),
  };
  QCOMPARE(joined(commands.calls), joined(expected));
}

void RescueStopAllTests::macSequence_noStrays_skipsTheGrace()
{
  RecordingCommands commands;
  runMacStopAllSequence(commands);
  for (const auto &call : commands.calls) {
    QVERIFY2(call.rfind("sleep", 0) != 0, "no strays: the sequence must not pause");
    QVERIFY2(call.rfind("term", 0) != 0 && call.rfind("kill", 0) != 0, "no strays: nothing to signal");
  }
  QVERIFY(commands.calls.size() >= 2);
  QCOMPARE(commands.calls[commands.calls.size() - 2], std::string("quit-self"));
  QCOMPARE(commands.calls.back(), "exit-fallback " + std::to_string(kQuitExitFallbackMs));
}

void RescueStopAllTests::requestLocalStopAll_runsExecutorOnceAndIgnoresRepeats()
{
  std::atomic<int> runs{0};
  std::mutex seatMutex;
  std::string seat;
  deskflow::coordination::setLocalStopAllHandler([&](const std::string &name) {
    std::scoped_lock lock{seatMutex};
    seat = name;
    ++runs;
  });

  QVERIFY(!deskflow::coordination::stopAllInProgress());
  QVERIFY(deskflow::coordination::requestLocalStopAll("macbookpro"));
  QVERIFY(deskflow::coordination::stopAllInProgress());
  // Repeats while stopping (duplicate mesh delivery, a second burst) are ignored.
  QVERIFY(deskflow::coordination::requestLocalStopAll("macbookpro"));
  QVERIFY(deskflow::coordination::requestLocalStopAll("macbookpro"));

  QVERIFY(waitFor([&runs] { return runs.load() >= 1; }, 2000));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  QCOMPARE(runs.load(), 1);
  {
    std::scoped_lock lock{seatMutex};
    QCOMPARE(seat, std::string("macbookpro"));
  }
}

void RescueStopAllTests::requestLocalStopAll_withoutExecutor_doesNothingAndRearms()
{
  // No executor registered (a bare unit-test process): logged, and the
  // in-progress flag is released so a later registration can still act.
  QVERIFY(!deskflow::coordination::requestLocalStopAll("tiny11"));
  QVERIFY(!deskflow::coordination::stopAllInProgress());
}

void RescueStopAllTests::requestFleetStopAll_withoutMesh_fallsBackToLocal()
{
  std::atomic<int> runs{0};
  deskflow::coordination::setLocalStopAllHandler([&runs](const std::string &) { ++runs; });
  deskflow::coordination::requestFleetStopAll();
  QVERIFY(waitFor([&runs] { return runs.load() == 1; }, 2000));
}

void RescueStopAllTests::requestLocalCoreQuit_isNoopOnceCleared()
{
  int quits = 0;
  deskflow::coordination::setLocalCoreQuitHandler([&quits] { ++quits; });
  deskflow::coordination::requestLocalCoreQuit();
  QCOMPARE(quits, 1);
  deskflow::coordination::setLocalCoreQuitHandler({});
  deskflow::coordination::requestLocalCoreQuit(); // the process is unwinding: nothing to call
  QCOMPARE(quits, 1);
}

QTEST_MAIN(RescueStopAllTests)
