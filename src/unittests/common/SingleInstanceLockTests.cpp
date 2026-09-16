/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "SingleInstanceLockTests.h"

#include "common/SingleInstanceLock.h"

#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QThread>

#if defined(Q_OS_WIN)
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

using deskflow::SingleInstanceLock;
using Role = SingleInstanceLock::Role;
using Scope = SingleInstanceLock::Scope;

namespace {

const char *const kHoldArg = "--hold";
const char *const kEnvDir = "DESKFLOW_LOCK_DIR";

// Helper mode: `SingleInstanceLockTests --hold <role> <scope>`.
// Acquires the lock, prints "held" or "busy", then keeps holding until stdin
// closes (parent kills or closes the channel), so the parent can observe the
// kernel dropping the lock on process exit.
int runHolder(int argc, char **argv)
{
  if (argc < 4) {
    std::fprintf(stderr, "usage: --hold <core|gui|daemon|vhid-bridge> <session|machine>\n");
    return 2;
  }
  Role role = Role::Core;
  if (std::strcmp(argv[2], "gui") == 0) {
    role = Role::Gui;
  } else if (std::strcmp(argv[2], "daemon") == 0) {
    role = Role::Daemon;
  } else if (std::strcmp(argv[2], "vhid-bridge") == 0) {
    role = Role::VhidBridge;
  }
  const Scope scope = (std::strcmp(argv[3], "machine") == 0) ? Scope::Machine : Scope::Session;

  const auto lock = SingleInstanceLock::tryAcquire(role, scope);
  std::printf("%s\n", lock ? "held" : "busy");
  std::fflush(stdout);
  if (!lock) {
    return 1;
  }
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "quit") {
      break;
    }
  }
  return 0;
}

struct Holder
{
  QProcess process;
  QString firstLine;

  bool start(const QString &dir, const char *role, const char *scope)
  {
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert(QString::fromLatin1(kEnvDir), dir);
    process.setProcessEnvironment(env);
    process.setProgram(QCoreApplication::applicationFilePath());
    process.setArguments({QString::fromLatin1(kHoldArg), QString::fromLatin1(role), QString::fromLatin1(scope)});
    process.start();
    if (!process.waitForStarted(5000)) {
      return false;
    }
    if (!process.waitForReadyRead(5000)) {
      return false;
    }
    firstLine = QString::fromUtf8(process.readLine()).trimmed();
    return true;
  }

  void stop()
  {
    if (process.state() == QProcess::Running) {
      process.write("quit\n");
      process.closeWriteChannel();
      if (!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished(5000);
      }
    }
  }

  ~Holder()
  {
    stop();
  }
};

} // namespace

void SingleInstanceLockTests::initTestCase()
{
  QVERIFY(m_dir.isValid());
  // Relocate every lock under the temp dir so the suite never touches the
  // real per-user / machine paths (a live core or GUI would make it flaky).
  QVERIFY(qputenv(kEnvDir, m_dir.path().toUtf8()));
}

void SingleInstanceLockTests::init()
{
  // Every case starts with nothing held in this process.
  QVERIFY(!SingleInstanceLock::isHeld(Role::Core, Scope::Session));
  QVERIFY(!SingleInstanceLock::isHeld(Role::Core, Scope::Machine));
}

void SingleInstanceLockTests::secondAcquireInSameProcessFails()
{
  auto first = SingleInstanceLock::tryAcquire(Role::Core, Scope::Session);
  QVERIFY(first.has_value());
  QVERIFY(!first->name().empty());
  QVERIFY(first->name().find(m_dir.path().toStdString()) == 0);

  const auto second = SingleInstanceLock::tryAcquire(Role::Core, Scope::Session);
  QVERIFY(!second.has_value());
  QVERIFY(!SingleInstanceLock::lastMessage().empty());
}

void SingleInstanceLockTests::rolesAndScopesAreIndependent()
{
  const auto coreSession = SingleInstanceLock::tryAcquire(Role::Core, Scope::Session);
  QVERIFY(coreSession.has_value());

  // Different scope, same role: independent.
  const auto coreMachine = SingleInstanceLock::tryAcquire(Role::Core, Scope::Machine);
  QVERIFY(coreMachine.has_value());

  // Different role, same scope: independent.
  const auto guiSession = SingleInstanceLock::tryAcquire(Role::Gui, Scope::Session);
  QVERIFY(guiSession.has_value());
  const auto daemon = SingleInstanceLock::tryAcquire(Role::Daemon, Scope::Machine);
  QVERIFY(daemon.has_value());
  const auto bridge = SingleInstanceLock::tryAcquire(Role::VhidBridge, Scope::Machine);
  QVERIFY(bridge.has_value());

  QVERIFY(SingleInstanceLock::isHeld(Role::Core, Scope::Session));
  QVERIFY(SingleInstanceLock::isHeld(Role::Core, Scope::Machine));
  QVERIFY(SingleInstanceLock::isHeld(Role::Gui, Scope::Session));
  QVERIFY(!SingleInstanceLock::isHeld(Role::Gui, Scope::Machine));
}

void SingleInstanceLockTests::releaseAllowsReacquire()
{
  auto lock = SingleInstanceLock::tryAcquire(Role::Gui, Scope::Session);
  QVERIFY(lock.has_value());
  QVERIFY(!SingleInstanceLock::tryAcquire(Role::Gui, Scope::Session).has_value());

  lock->release();
  QVERIFY(!SingleInstanceLock::isHeld(Role::Gui, Scope::Session));
  QVERIFY(SingleInstanceLock::tryAcquire(Role::Gui, Scope::Session).has_value());

  // Destructor path.
  lock.reset();
  {
    const auto scoped = SingleInstanceLock::tryAcquire(Role::Gui, Scope::Session);
    QVERIFY(scoped.has_value());
    QVERIFY(SingleInstanceLock::isHeld(Role::Gui, Scope::Session));
  }
  QVERIFY(!SingleInstanceLock::isHeld(Role::Gui, Scope::Session));
}

void SingleInstanceLockTests::moveKeepsLock()
{
  auto original = SingleInstanceLock::tryAcquire(Role::Daemon, Scope::Session);
  QVERIFY(original.has_value());
  const auto name = original->name();

  SingleInstanceLock moved(std::move(*original));
  original.reset(); // moved-from destructor must not release
  QCOMPARE(moved.name(), name);
  QVERIFY(SingleInstanceLock::isHeld(Role::Daemon, Scope::Session));
  QVERIFY(!SingleInstanceLock::tryAcquire(Role::Daemon, Scope::Session).has_value());
}

void SingleInstanceLockTests::childCannotAcquireWhileParentHolds()
{
  const auto parentLock = SingleInstanceLock::tryAcquire(Role::Core, Scope::Machine);
  QVERIFY(parentLock.has_value());

  Holder child;
  QVERIFY(child.start(m_dir.path(), "core", "machine"));
  QCOMPARE(child.firstLine, QStringLiteral("busy"));
  QVERIFY(child.process.waitForFinished(5000));
  QCOMPARE(child.process.exitCode(), 1);

  // A different role is not blocked by the parent's Core lock.
  Holder other;
  QVERIFY(other.start(m_dir.path(), "vhid-bridge", "machine"));
  QCOMPARE(other.firstLine, QStringLiteral("held"));
  other.stop();
}

void SingleInstanceLockTests::lockReleasedWhenChildExits()
{
  Holder child;
  QVERIFY(child.start(m_dir.path(), "core", "session"));
  QCOMPARE(child.firstLine, QStringLiteral("held"));

  // While the child lives, the parent sees it held and cannot take it.
  QVERIFY(SingleInstanceLock::isHeld(Role::Core, Scope::Session));
  QVERIFY(!SingleInstanceLock::tryAcquire(Role::Core, Scope::Session).has_value());

  // Kill it without any cleanup path running: the kernel must drop the lock.
  child.process.kill();
  QVERIFY(child.process.waitForFinished(5000));

  QVERIFY(!SingleInstanceLock::isHeld(Role::Core, Scope::Session));
  const auto reacquired = SingleInstanceLock::tryAcquire(Role::Core, Scope::Session);
  QVERIFY(reacquired.has_value());
}

void SingleInstanceLockTests::isHeldReflectsState()
{
  QVERIFY(!SingleInstanceLock::isHeld(Role::VhidBridge, Scope::Machine));

  // Probing must not itself acquire or leave anything behind.
  QVERIFY(!SingleInstanceLock::isHeld(Role::VhidBridge, Scope::Machine));
  {
    const auto lock = SingleInstanceLock::tryAcquire(Role::VhidBridge, Scope::Machine);
    QVERIFY(lock.has_value());
    QVERIFY(SingleInstanceLock::isHeld(Role::VhidBridge, Scope::Machine));
    // Probing while held must not steal it.
    QVERIFY(SingleInstanceLock::isHeld(Role::VhidBridge, Scope::Machine));
    QVERIFY(!SingleInstanceLock::tryAcquire(Role::VhidBridge, Scope::Machine).has_value());
  }
  QVERIFY(!SingleInstanceLock::isHeld(Role::VhidBridge, Scope::Machine));
}

void SingleInstanceLockTests::boundedWaitDrainsPredecessor()
{
  using namespace std::chrono;

  Holder child;
  QVERIFY(child.start(m_dir.path(), "core", "machine"));
  QCOMPARE(child.firstLine, QStringLiteral("held"));

  // Bounded: gives up after the timeout while the holder stays alive.
  const auto t0 = steady_clock::now();
  QVERIFY(!SingleInstanceLock::tryAcquire(Role::Core, Scope::Machine, milliseconds(300)).has_value());
  const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - t0);
  QVERIFY2(elapsed >= milliseconds(250), qPrintable(QString::number(elapsed.count())));
  QVERIFY2(elapsed < milliseconds(3000), qPrintable(QString::number(elapsed.count())));

  // Handoff: the predecessor exits mid-wait and we pick the lock up. The
  // wait loop blocks this thread, so the kill is driven from another one.
  const auto t1 = steady_clock::now();
  const auto childPid = child.process.processId();
  QThread *killer = QThread::create([childPid] {
    QThread::msleep(200);
#if defined(Q_OS_WIN)
    if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(childPid)); h != nullptr) {
      TerminateProcess(h, 9);
      CloseHandle(h);
    }
#else
    ::kill(static_cast<pid_t>(childPid), SIGKILL);
#endif
  });
  killer->start();
  const auto handoff = SingleInstanceLock::tryAcquire(Role::Core, Scope::Machine, seconds(3));
  killer->wait();
  delete killer;
  const auto took = duration_cast<milliseconds>(steady_clock::now() - t1);
  QVERIFY2(handoff.has_value(), SingleInstanceLock::lastMessage().c_str());
  QVERIFY2(took < milliseconds(2500), qPrintable(QString::number(took.count())));
  QVERIFY(child.process.waitForFinished(5000));
}

int main(int argc, char **argv)
{
  if (argc >= 2 && std::strcmp(argv[1], kHoldArg) == 0) {
    return runHolder(argc, argv);
  }
  QCoreApplication app(argc, argv);
  SingleInstanceLockTests tc;
  QTEST_SET_MAIN_SOURCE_PATH
  return QTest::qExec(&tc, argc, argv);
}
