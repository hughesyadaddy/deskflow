/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QTest>

//! Source-address gating of fleet-wide commands: every address a
//! configured peer resolves to is allowed, nothing else.
class PeerAddressAllowlistTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();
  void normalize_canonicalizesLiterals();
  void literals_allowedImmediately();
  void unknown_denied();
  void names_resolvedOnRefresh();
  void miss_schedulesEarlyRefreshRateLimited();
  void emptyResolveKeepsLastGoodSet();
};
