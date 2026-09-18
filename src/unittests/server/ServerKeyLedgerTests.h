/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QTest>

//! Server-side key ledger: every key sent to the active client is
//! released on every boundary (I1), lock state is pushed as STATE (I2).
class ServerKeyLedgerTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();
  void ledger_recordsEveryKeyAndReleasesOnSwitch();
  void ledger_keyUpForgetsEntry();
  void ledger_ignoresKeysSentToPrimary();
  void teardown_releasesLedgerAndSendsLeave();
  void forceLeave_sendsBestEffortReleases();
  void broadcast_keysReleasedPerScreen();
  void broadcast_offReleasesHeldKeys();
  void lockChange_pushesStateToActiveClient();
  void clearAll_releasesOnlyThatSendersKeysOnTheActiveClient();
  void clearAll_withoutSenderReleasesEveryRelayedKey();
};
