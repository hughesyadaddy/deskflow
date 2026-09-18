/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QTest>

class WheelExProtocolTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();
  void negotiatedMinor_clampsToServer();
  void fixedToNotchUnits_roundsToNearest();
  void fixedToNotchUnits_continuousUsesPixelsPerLine();
  void fromNotches_roundTripsLegacyDeltas();
  void isEmpty_phaseMarkersAreNotEmpty();
  void takeWholeNotches_keepsRemainder();
  void fromWire_sanitizesPhaseBytes();
  void dmwx_packsAndUnpacks();
  void dmwx_wireLengthIsFixed();
  void secondaryDefault_banksToWholeNotches();
  void secondaryDefault_dropsPhaseOnly();
  void secondaryDefault_negativeBankFlushesNegative();
  void applyScrollModifierFixed_scalesWithoutTruncation();
};
