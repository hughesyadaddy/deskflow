/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QTest>

class KeyboardRescueTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  // RescueBurst: pure, ms clock injected. Decided when the burst ENDS.
  void burst_fourTaps_none();
  void burst_fiveTaps_restart();
  void burst_sevenTaps_restart();
  void burst_tenTaps_stop();
  void burst_twelveTaps_stop();
  void burst_spacedBeyondGap_none();
  void burst_exactlyTenNeverYieldsRestart();
  void burst_pauseAtFiveThenContinue_restartThenFreshCount();
  void burst_decideBeforeSettle_none();
  void burst_latePressClosesStaleBurst();
  void burst_breakAbandonsCount();
  // EscTapRescue: key filtering over the burst.
  void escTap_modifiersDoNotCount();
  void escTap_capsLockIgnored_stillCounts();
  void escTap_nonEscBreaksStreak();
  void escTap_swallowsFromFifthTap();
  // RescueSettleTimer: wakes once the deadline passes, re-arm replaces.
  void settleTimer_firesOnceAfterDeadline();
  void settleTimer_rearmReplacesDeadline();
  void settleTimer_cancelSuppresses();
  // ExitWatchdog: hard-exits (injected here) unless cancelled in time.
  void exitWatchdog_firesAfterDelayWithCodeAndReason();
  void exitWatchdog_cancelSuppresses();
  void exitWatchdog_rearmReplacesDeadline();
  void processExitFallback_usesInjectedExit();
};
