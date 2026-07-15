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
  void fiveEscWithinWindow_triggers();
  void gapBeyondWindow_resetsCount();
  void modifiers_doNotCount();
  void capsLockIgnored_stillCounts();
};
