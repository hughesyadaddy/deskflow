/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "PermissionCheck.h"

#include <QTest>

#include <sstream>
#include <vector>

using namespace deskflow::core::permissions;

namespace {

// Injected probes: no TCC needed.
Status allGranted()
{
  return {true, true, true, true};
}
Status noPostEvent()
{
  return {true, true, true, false};
}
Status accessibilityOnly()
{
  return {true, true, false, false};
}
Status inputMonitoringOnly()
{
  return {true, false, true, false};
}
Status allDenied()
{
  return {true, false, false, false};
}
Status unsupported()
{
  return {};
}

struct Argv
{
  explicit Argv(std::vector<std::string> args) : m_args(std::move(args))
  {
    for (auto &a : m_args) {
      m_ptrs.push_back(a.data());
    }
  }
  int argc() const
  {
    return static_cast<int>(m_ptrs.size());
  }
  char **argv()
  {
    return m_ptrs.data();
  }
  std::vector<std::string> m_args;
  std::vector<char *> m_ptrs;
};

int runWith(std::vector<std::string> args, Probe probe, std::string *output = nullptr)
{
  Argv a(std::move(args));
  std::ostringstream out;
  const int rc = run(a.argc(), a.argv(), probe, out);
  if (output) {
    *output = out.str();
  }
  return rc;
}

} // namespace

class PermissionCheckTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void notRequested_returnsMinusOne_andPrintsNothing();
  void requested_flagDetection();
  void exitCodes();
  void textOutput();
  void jsonOutput();
  void unsupportedPlatform();
  void nullProbe_treatedAsUnsupported();
};

void PermissionCheckTests::notRequested_returnsMinusOne_andPrintsNothing()
{
  std::string out;
  QCOMPARE(runWith({"deskflow-core"}, allGranted, &out), -1);
  QVERIFY(out.empty());
  QCOMPARE(runWith({"deskflow-core", "--server", "--json"}, allGranted, &out), -1);
  QVERIFY(out.empty());
  // Prefix/suffix variants must not match.
  QCOMPARE(runWith({"deskflow-core", "--check-permissionsx"}, allGranted, &out), -1);
  QCOMPARE(runWith({"deskflow-core", "check-permissions"}, allGranted, &out), -1);
}

void PermissionCheckTests::requested_flagDetection()
{
  Argv a({"deskflow-core", "--foo", "--check-permissions", "--json"});
  QVERIFY(requested(a.argc(), a.argv()));
  QVERIFY(wantsJson(a.argc(), a.argv()));

  Argv b({"deskflow-core", "--check-permissions"});
  QVERIFY(requested(b.argc(), b.argv()));
  QVERIFY(!wantsJson(b.argc(), b.argv()));

  // argv[0] is never treated as a flag.
  Argv c({"--check-permissions"});
  QVERIFY(!requested(c.argc(), c.argv()));

  QVERIFY(!requested(0, nullptr));
}

void PermissionCheckTests::exitCodes()
{
  QCOMPARE(runWith({"x", "--check-permissions"}, allGranted), kExitGranted);
  // post-event does not gate the exit code.
  QCOMPARE(runWith({"x", "--check-permissions"}, noPostEvent), kExitGranted);
  QCOMPARE(runWith({"x", "--check-permissions"}, accessibilityOnly), kExitDenied);
  QCOMPARE(runWith({"x", "--check-permissions"}, inputMonitoringOnly), kExitDenied);
  QCOMPARE(runWith({"x", "--check-permissions"}, allDenied), kExitDenied);
  QCOMPARE(runWith({"x", "--check-permissions"}, unsupported), kExitUnsupported);

  QCOMPARE(kExitGranted, 0);
  QCOMPARE(kExitDenied, 1);
  QCOMPARE(kExitUnsupported, 2);
}

void PermissionCheckTests::textOutput()
{
  std::string out;
  runWith({"x", "--check-permissions"}, noPostEvent, &out);
  QCOMPARE(QString::fromStdString(out), QStringLiteral("accessibility=granted\ninput-monitoring=granted\npost-event=denied\n"));

  runWith({"x", "--check-permissions"}, inputMonitoringOnly, &out);
  QCOMPARE(QString::fromStdString(out), QStringLiteral("accessibility=denied\ninput-monitoring=granted\npost-event=denied\n"));
}

void PermissionCheckTests::jsonOutput()
{
  std::string out;
  QCOMPARE(runWith({"x", "--json", "--check-permissions"}, accessibilityOnly, &out), kExitDenied);
  QCOMPARE(
      QString::fromStdString(out),
      QStringLiteral(
          "{\"supported\":true,\"accessibility\":\"granted\",\"input-monitoring\":\"denied\",\"post-event\":\"denied\"}\n"
      )
  );

  QCOMPARE(runWith({"x", "--check-permissions", "--json"}, unsupported, &out), kExitUnsupported);
  QCOMPARE(QString::fromStdString(out), QStringLiteral("{\"supported\":false}\n"));
}

void PermissionCheckTests::unsupportedPlatform()
{
  std::string out;
  QCOMPARE(runWith({"x", "--check-permissions"}, unsupported, &out), kExitUnsupported);
  QCOMPARE(QString::fromStdString(out), QStringLiteral("unsupported\n"));
}

void PermissionCheckTests::nullProbe_treatedAsUnsupported()
{
  std::string out;
  QCOMPARE(runWith({"x", "--check-permissions"}, nullptr, &out), kExitUnsupported);
  QCOMPARE(QString::fromStdString(out), QStringLiteral("unsupported\n"));
}

QTEST_APPLESS_MAIN(PermissionCheckTests)

#include "PermissionCheckTests.moc"
