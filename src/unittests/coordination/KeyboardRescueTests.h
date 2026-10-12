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
  // EscHoldRescue: pure, clock injected. Fires once the hold has LASTED.
  void hold_releasedBeforeDeadline_none();
  void hold_heldPastDeadline_restartOnce();
  void hold_duplicateDownKeepsStart();
  void hold_chordedEscDoesNotStart();
  void hold_lockModifiersStillStart();
  void hold_otherKeyCancels();
  void hold_modifierJoiningCancels();
  void hold_otherKeyUpIgnored();
  void hold_resetClears();
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
