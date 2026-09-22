/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "base/Log.h"

#include "arch/Arch.h"
#include "platform/OSXKeyState.h"

#include <QTest>

class OSXKeyStateTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  // Test are run in order top to bottom
  void mapModifiersFromOSX_OSXMask();
  void fakePollShift();
  void fakePollChar();
  void fakePollCharWithModifier();
  void mapKeyFromEventOffMainThreadDoesNotCrash();
  // headless: every OS touch point is hooked, nothing is injected
  void shadowFlagsReseedFromOsOnUpdateKeyState();
  void keyboardEventFlagsKeepDeviceBitsWithCapsOn();
  void keyboardEventFlagsCarryShiftForUpperLetterWithCapsOn();
  void sanitizeReleasesOnlyInjectedModifiers();
  void sanitizeSkipsModifiersOsReportsUp();
  void sanitizeReleasesStaleModifiersWithoutRecentHardwarePress();
  void sanitizeKeepsModifiersBackedByRecentHardwarePress();
  void sanitizeReleasesInjectedCaps();
  void sanitizeLeavesOsCapsLockAlone();
  void releaseInjectedKeysLeavesPhysicallyHeldModifierAlone();
  void releaseInjectedKeysReleasesLedgeredCmd();
  void fakeAllKeysUpReleasesLedgeredModifierOutsideSyntheticSet();
  void primarySweepNeverReleasesPhysicallyCapturedShift();
  void releaseInjectedKeysKeepsReassertedModifier();
  void setToggleStateNoOpsWhenCapsMatches();
  void setToggleStateDrivesCapsViaLockStateApi();
  void setToggleStateFallsBackToFakeCapsPress();
  void setToggleStateIgnoresNumAndScrollLock();
  void setToggleStateKeepsTrackedMaskInStep();
  // K5: local-keyboard capitalization at password prompts
  void sanitizeSkipsStaleSweepWhileSecureInput();
  void sanitizeSkipsStaleSweepWithoutObservationWindow();
  void ledgerReleaseKeepsShiftHeldOnRightHandKey();
  void modifierPostFlagsDeriveFromLiveOsNotShadow();

private:
  bool isKeyPressed(const OSXKeyState &keyState, KeyButton button);
  Arch m_arch;
  Log m_log;
};
