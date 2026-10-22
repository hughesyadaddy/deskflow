/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "arch/Arch.h"
#include "base/Log.h"

#include <QObject>

class OSXModifierResyncTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void agreementPostsNothing();
  void cmdHeldAcrossToClientReleasedThere_releasesOnReturn();
  void cmdStillHeldWhenReturning_assertsCmd();
  void everyModifierReleasedOnReturn();
  void capsLockAndFnUntouched();
  void rightSideBitsUseRightKeycodes();
  void bothSidesChanged_twoSteps();
  void keyDownMatchesHidBit();
  void stepsEndAtTruth();
  void run_postsAbsoluteFlagsThroughSink();
  void run_resamplesAndSkipsResolvedStep();
  void run_boundedWhenSessionNeverAdopts();
  void keyState_postModifierResync_usesIOHIDHookWithAbsoluteFlags();
  void tap_echoOfOwnPostIsRecognisedOnce();
  void tap_dskrMarkerIsOwnAndInjected();
  void tap_hardwareFlagsChangedIsNotOwn();
  // stuck-Fn (2026-10-21): pure stray-modifier decision helpers
  void stray_eachWatchedBitUnexplainedIsStray();
  void stray_deviceBackedModifierIsNotStray();
  void stray_fnExplainedByHeldFunctionKeyOrPhysicalFn();
  void stray_capsLockIsNeverStray();
  void stray_numericPadCleaning();
  void stray_resyncKeycodePrefersFn();
  void stray_dwellRequiresSameSetTwice();

private:
  Arch m_arch;
  Log m_log;
};
