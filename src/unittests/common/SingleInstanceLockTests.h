/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <QTemporaryDir>
#include <QTest>

class SingleInstanceLockTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void init();

  void secondAcquireInSameProcessFails();
  void rolesAndScopesAreIndependent();
  void releaseAllowsReacquire();
  void moveKeepsLock();
  void childCannotAcquireWhileParentHolds();
  void lockReleasedWhenChildExits();
  void isHeldReflectsState();
  void boundedWaitDrainsPredecessor();

private:
  QTemporaryDir m_dir;
};
