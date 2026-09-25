/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

// K8: "event queue is not ready within 5 sec" killed one worker thread per
// failed auto-mode epoch. waitForReady() only returned on a *signal*, so a
// thread created after the queue had already looped (every epoch after the
// first) or in an epoch that never reached its loop waited the whole bound
// and died by exception. The flag, not the signal, is the condition.

#include "arch/Arch.h"
#include "base/Event.h"
#include "base/EventQueue.h"
#include "base/EventTypes.h"
#include "base/Log.h"
#include "base/TMethodJob.h"
#include "mt/Thread.h"

#include <QTest>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

class EventQueueReadyTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();
  void queueThatAlreadyLoopedIsReadyAtOnce();
  void waiterIsReleasedWhenTheLoopStarts();
  void queueThatNeverLoopsTimesOutWithTheRealBound();
};

namespace {

Log g_log;
std::unique_ptr<Arch> g_arch;

//! Arch-registered waiter (CondVar waits are cancellation points, so the
//! thread must be a deskflow Thread, exactly like OSXScreen's power thread).
struct Waiter
{
  EventQueue *queue = nullptr;
  double bound = 30.0;
  std::atomic<bool> done{false};
  std::atomic<bool> ok{false};
  std::string error;

  void run(const void *)
  {
    try {
      queue->waitForReady(bound);
      ok = true;
    } catch (std::exception &e) {
      error = e.what();
    }
    done = true;
  }
};

bool waitFor(const std::atomic<bool> &flag, std::chrono::milliseconds limit)
{
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!flag) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

} // namespace

void EventQueueReadyTests::initTestCase()
{
  g_arch = std::make_unique<Arch>();
}

void EventQueueReadyTests::cleanupTestCase()
{
  g_arch.reset();
}

void EventQueueReadyTests::queueThatAlreadyLoopedIsReadyAtOnce()
{
  EventQueue queue;
  queue.addEvent(Event(EventTypes::Quit));
  QCOMPARE(queue.loop(), 0);

  // Epoch 2+: the queue looped before; a new screen's worker must not wait.
  const auto started = std::chrono::steady_clock::now();
  queue.waitForReady(5.0);
  const auto took = std::chrono::steady_clock::now() - started;
  QVERIFY2(took < std::chrono::milliseconds(500), "waitForReady() blocked on an already-ready queue");

  Waiter waiter;
  waiter.queue = &queue;
  waiter.bound = 5.0;
  Thread thread(new TMethodJob<Waiter>(&waiter, &Waiter::run));
  QVERIFY(waitFor(waiter.done, std::chrono::seconds(2)));
  thread.wait();
  QVERIFY2(waiter.ok, waiter.error.c_str());
}

void EventQueueReadyTests::waiterIsReleasedWhenTheLoopStarts()
{
  // Epoch 1: the worker starts before the loop and is released by it.
  EventQueue queue;
  Waiter waiter;
  waiter.queue = &queue;
  waiter.bound = 10.0;
  Thread thread(new TMethodJob<Waiter>(&waiter, &Waiter::run));
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  QVERIFY(!waiter.done);

  queue.addEvent(Event(EventTypes::Quit));
  QCOMPARE(queue.loop(), 0);
  QVERIFY(waitFor(waiter.done, std::chrono::seconds(2)));
  thread.wait();
  QVERIFY2(waiter.ok, waiter.error.c_str());
  QCOMPARE(EventQueue::kReadyTimeoutS, 30.0);
}

void EventQueueReadyTests::queueThatNeverLoopsTimesOutWithTheRealBound()
{
  EventQueue queue;
  bool threw = false;
  std::string message;
  try {
    queue.waitForReady(0.3);
  } catch (std::runtime_error &e) {
    threw = true;
    message = e.what();
  }
  QVERIFY(threw);
  // Honest about the bound it applied (the old text said 5 s, waited 10).
  QCOMPARE(QString::fromStdString(message), QStringLiteral("event queue is not ready within 0.3 sec"));
}

QTEST_MAIN(EventQueueReadyTests)

#include "EventQueueReadyTests.moc"
