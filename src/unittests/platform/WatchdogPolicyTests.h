/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QObject>

class WatchdogPolicyTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void isFastExit_shortUptime();
  void isFastExit_duplicateInstanceRegardlessOfUptime();
  void isFastExit_longUptimeCleanOrCrash();
  void nextRestartDelay_longUptimeRestartsNow();
  void nextRestartDelay_duplicateInstanceWaits30s();
  void nextRestartDelay_fastExitsBackOffExponentially();
  void nextRestartDelay_backoffIsCapped();
  void nextRestartDelay_givesUpAfterMaxFastExits();
  void nextRestartDelay_giveUpBeatsDuplicateInstance();
  void nextRestartDelay_isConstexpr();
};
