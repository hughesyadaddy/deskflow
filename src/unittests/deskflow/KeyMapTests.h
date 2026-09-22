/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "base/Log.h"

#include <QTest>

namespace deskflow {
class KeyMapTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void findBestKey_requiredDown_matchExactFirstItem();
  void findBestKey_requiredAndExtraSensitiveDown_matchExactFirstItem();
  void findBestKey_requiredAndExtraSensitiveDown_matchExactSecondItem();
  void findBestKey_extraSensitiveDown_matchExactSecondItem();
  void findBestKey_noRequiredDown_matchOneRequiredChangeItem();
  void findBestKey_onlyOneRequiredDown_matchTwoRequiredChangesItem();
  void findBestKey_noRequiredDown_cannotMatch();
  void isCommand();
  void mapkey();
  void mapKey_upperLetterWithoutShiftInMask_pressesShift();
  void mapKey_lowerLetterWithShiftDesired_noShiftKeystroke();
  void mapKey_upperLetterWithShiftAndCaps_noExtraCapsPress();
  void mapKey_capsInsensitiveKeyWithCapsMaskMismatch_noCapsStrokes_data();
  void mapKey_capsInsensitiveKeyWithCapsMaskMismatch_noCapsStrokes();
  void mapKey_capsRequiredKeyStillTogglesCaps();
  void mapKey_halfDuplexCapsByKeyId_pressThenReleaseAroundKey();
  void parseModifiers_plusKey_keepsPlusAsKey();
  void parseKey_plusSymbol_parsesAsAsciiKey();

private:
  Log m_log;
};
} // namespace deskflow
