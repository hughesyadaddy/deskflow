/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QTest>

//! 10x Esc stop-all: the macOS sequence through a command recorder, and the
//! process-wide once-only executor plumbing.
class RescueStopAllTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();
  void init();
  void macSequence_fallbackOrder_quitIntentConvergeGuiStraysCoreLast();
  void macSequence_canonicalStop_runsScriptThenQuits();
  void macSequence_canonicalFailure_fallsBackInProcess();
  void macSequence_noStrays_skipsTheGrace();
  void requestLocalStopAll_runsExecutorOnceAndIgnoresRepeats();
  void requestLocalStopAll_withoutExecutor_doesNothingAndRearms();
  void requestFleetStopAll_withoutMesh_fallsBackToLocal();
  void requestLocalCoreQuit_isNoopOnceCleared();
};
