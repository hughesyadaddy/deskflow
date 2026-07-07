/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include <QTest>

class KeyboardRelayHookPolicyTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void injectedKeysPassThroughWhileForwarding();
  void swallowOnlyWhenForwarded();
  void passLocalAlwaysPassesThrough();
  void unmappedKeysPassThrough();
  void injectedWinsEvenWhenForwarded();
};
