/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QObject>

//! Pure decision logic of the Windows stale-key sweeps (no live input state).
class MSWindowsKeyStateTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  // sanitizeInjectedKeys(): which VKs to release
  void release_emptyLedgerNothingDown_releasesNothing();
  void release_ledgerEntryPhysicallyDown_isReleased();
  void release_ledgerEntryNotDown_isSkipped();
  void release_shiftIsAlwaysACandidate();
  void release_orderIsAscendingVk();

  // releaseInjectedKeys(): ledger only, keep-filtered
  void ledgerRelease_emptyLedgerIgnoresPhysicalShift();
  void ledgerRelease_keepsExcludedModifier();

  // audit skip expiry
  void audit_freshEntry_isProtected();
  void audit_entryAtGrace_isStillProtected();
  void audit_entryPastGrace_isNoLongerProtected();
  void audit_shiftNeverReachesAuditTable();
  void audit_bitOrderMatchesDeskTable();
};
