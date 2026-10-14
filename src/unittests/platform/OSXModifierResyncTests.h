/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QObject>

class OSXModifierResyncTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void agreementPostsNothing();
  void cmdHeldAcrossToClientReleasedThere_releasesOnReturn();
  void cmdStillHeldWhenReturning_assertsCmd();
  void everyModifierReleasedOnReturn();
  void capsLockStateUntouched();
  void stepsCarryModifierKeycodesAndEndAtTruth();
  void leftRightDeviceBitsFollowHid();
};
