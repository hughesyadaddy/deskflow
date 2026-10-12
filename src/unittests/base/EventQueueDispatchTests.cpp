/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

// 2026-10-12 tiny11: deskflow-core.exe died with 0xC0000005 at Screen+8 on
// two of three client-epoch starts, every day since 10-05, always right after
// "[keys] delayed enable sweep". dispatchEvent() ran the handler through a
// pointer into the handler table; the sweep's one-shot lambda removed itself
// as its first act (erasing the std::function -- closure included -- it was
// executing from) and then reloaded its captured Screen* from the freed node.
// A handler must run from a copy that outlives the call, whatever it does to
// the table meanwhile.

#include "arch/Arch.h"
#include "base/Event.h"
#include "base/EventQueue.h"
#include "base/EventTypes.h"
#include "base/Log.h"

#include <QTest>

#include <atomic>
#include <memory>

class EventQueueDispatchTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();
  void handlerThatRemovesItselfOutlivesItsOwnCall();
  void handlerThatRemovesItsTargetOutlivesItsOwnCall();
  void unknownTypeFallbackRunsFromACopyToo();
};

namespace {

Log g_log;
std::unique_ptr<Arch> g_arch;

//! Lives in the closure; counts how many closures holding it are alive.
struct Probe
{
  explicit Probe(std::atomic<int> *alive) : m_alive(alive)
  {
    ++*m_alive;
  }
  ~Probe()
  {
    --*m_alive;
  }
  std::atomic<int> *m_alive;
};

} // namespace

void EventQueueDispatchTests::initTestCase()
{
  g_arch = std::make_unique<Arch>();
}

void EventQueueDispatchTests::cleanupTestCase()
{
  g_arch.reset();
}

void EventQueueDispatchTests::handlerThatRemovesItselfOutlivesItsOwnCall()
{
  EventQueue events;
  std::atomic<int> alive{0};
  int target = 0;
  int runs = 0;
  int aliveAfterRemove = -1;

  auto probe = std::make_shared<Probe>(&alive);
  events.addHandler(EventTypes::Timer, &target, [&, probe](const Event &) {
    ++runs;
    // what every one-shot timer handler does first
    events.removeHandler(EventTypes::Timer, &target);
    // the table entry is gone; the closure this line runs from must not be
    aliveAfterRemove = alive.load();
    QVERIFY(probe->m_alive == &alive);
  });
  probe.reset(); // the closure holds the only reference
  QCOMPARE(alive.load(), 1);

  const Event event(EventTypes::Timer, &target);
  QVERIFY(events.dispatchEvent(event));
  QCOMPARE(runs, 1);
  QCOMPARE(aliveAfterRemove, 1);
  // the copy died with the call, and so did the removed original
  QCOMPARE(alive.load(), 0);

  // removed: nothing runs for that target any more
  QVERIFY(!events.dispatchEvent(event));
  QCOMPARE(runs, 1);
}

void EventQueueDispatchTests::handlerThatRemovesItsTargetOutlivesItsOwnCall()
{
  EventQueue events;
  std::atomic<int> alive{0};
  int target = 0;
  int aliveAfterRemove = -1;

  auto probe = std::make_shared<Probe>(&alive);
  events.addHandler(EventTypes::Timer, &target, [&, probe](const Event &) {
    events.removeHandlers(&target); // Screen::disable()-style: every type at once
    aliveAfterRemove = alive.load();
  });
  probe.reset();

  const Event event(EventTypes::Timer, &target);
  QVERIFY(events.dispatchEvent(event));
  QCOMPARE(aliveAfterRemove, 1);
  QCOMPARE(alive.load(), 0);
  QVERIFY(!events.dispatchEvent(event));
}

void EventQueueDispatchTests::unknownTypeFallbackRunsFromACopyToo()
{
  EventQueue events;
  std::atomic<int> alive{0};
  int target = 0;
  int aliveAfterRemove = -1;

  auto probe = std::make_shared<Probe>(&alive);
  events.addHandler(EventTypes::Unknown, &target, [&, probe](const Event &) {
    events.removeHandler(EventTypes::Unknown, &target);
    aliveAfterRemove = alive.load();
  });
  probe.reset();

  // no Timer handler for the target: the Unknown ("any") handler takes it
  const Event event(EventTypes::Timer, &target);
  QVERIFY(events.dispatchEvent(event));
  QCOMPARE(aliveAfterRemove, 1);
  QCOMPARE(alive.load(), 0);
  QVERIFY(!events.dispatchEvent(event));
}

QTEST_MAIN(EventQueueDispatchTests)

#include "EventQueueDispatchTests.moc"
