/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QTest>

//! Key-ledger invariants on the client side (Screen + KeyState).
/*!
I1: a machine only has keys down that were pressed while it was the
active target. I2: Caps Lock is STATE, not a toggle. I4: every async
boundary resyncs.
*/
class KeyStateLedgerTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();
  void enterSecondary_reassertsHeldShiftWhenOsLacksIt();
  void enterSecondary_doesNotReassertWhenOsAlreadyHoldsIt();
  void enterSecondary_reassertsModifiersIncrementally();
  void enterSecondary_appliesCapsViaSetToggleState();
  void enterSecondary_clearsCapsWhenPrimaryHasItOff();
  void keyUp_mapsRealReleaseOntoReassertedModifier();
  void leaveSecondary_releasesEverySyntheticKey();
  void disablePrimary_releasesInjectedKeysAndSanitizes();
  void updateKeyState_releasesSyntheticKeysBeforeZeroing();
  void fakeKeyDown_capsKeyRoutesMaskBitToSetToggleState();
  void fakeKeyDown_doesNotClickCapsAgainAfterSetToggleState();
  void enable_sanitizesOnlyWhenNotEntered();
  void describeKey_printsCharacterWithItsCase();
};
