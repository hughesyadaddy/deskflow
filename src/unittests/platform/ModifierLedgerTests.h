/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QObject>

//! Pure decision logic of the Windows injected-modifier ledger and the
//! stale-modifier audit (platform/MSWindowsModifierLedger.h). Platform
//! neutral: no Win32 input state.
class ModifierLedgerTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  // D1: ledger hygiene
  void ledger_afterBoundaryRelease_isEmpty();
  void ledger_afterLeave_isEmpty();
  void ledger_forgetKeepsUntouchedEntries();

  // D3: audit grace while entered
  void audit_unledgeredDown_needsTwoTicks();
  void audit_unledgeredDown_notReleasedWhileTyping();
  void audit_ledgeredDown_neverReleased();
  void audit_boundary_releasesImmediately();
  void audit_upBetweenTicks_resetsGrace();
  void audit_releaseResetsGraceForRepress();
};
