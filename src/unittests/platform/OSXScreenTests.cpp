/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "OSXScreenTests.h"

#include "platform/OSXScreen.h"

#include <QTest>

void OSXScreenTests::clipboardChangeCountGate()
{
  long last = -1;
  QVERIFY(OSXScreen::clipboardChangeCountAdvanced(last, 7));
  QCOMPARE(last, 7L);
  QVERIFY(!OSXScreen::clipboardChangeCountAdvanced(last, 7));
  QVERIFY(OSXScreen::clipboardChangeCountAdvanced(last, 9));
  QCOMPARE(last, 9L);
  QVERIFY(OSXScreen::clipboardChangeCountAdvanced(last, 3));
  QCOMPARE(last, 3L);
}

void OSXScreenTests::wheelLinesFromEvent_integerWinsBelowOneLine()
{
  // a slow notch on a plain wheel: FixedPt 0.1 but DeltaAxis 1 -> one line
  QCOMPARE(OSXScreen::wheelLinesFromEvent(0.1, 1), 1.0);
  QCOMPARE(OSXScreen::wheelLinesFromEvent(-0.1, -1), -1.0);
  // fast notch carries a fraction worth keeping
  QCOMPARE(OSXScreen::wheelLinesFromEvent(2.5, 3), 2.5);
  QCOMPARE(OSXScreen::wheelLinesFromEvent(-1.0, -1), -1.0);
  // hi-res sub-notch tick: integer rounds to zero, the fraction survives
  QCOMPARE(OSXScreen::wheelLinesFromEvent(0.0125, 0), 0.0125);
  QCOMPARE(OSXScreen::wheelLinesFromEvent(-0.0125, 0), -0.0125);
  QCOMPARE(OSXScreen::wheelLinesFromEvent(0.0, 0), 0.0);
}

// K6: mid-session secure-input watcher. secureInputWatchTick() is a pure
// function so this covers the gating/cadence logic without constructing a
// real OSXScreen (which needs AX permissions / a live CGEventTap).

void OSXScreenTests::secureInputWatchTick_noneWhenNotEntered()
{
  // The boundary sweep (Screen::enable()'s sanitizeInjectedKeys() and its
  // delayed timer) only ever runs while NOT entered. This watcher must
  // never act there too, whatever the secure-input edge looks like --
  // otherwise the two would double-fire on the same transition.
  using T = OSXScreen::SecureInputTransition;
  QCOMPARE(OSXScreen::secureInputWatchTick(false, false, false), T::None);
  QCOMPARE(OSXScreen::secureInputWatchTick(false, false, true), T::None);
  QCOMPARE(OSXScreen::secureInputWatchTick(false, true, false), T::None);
  QCOMPARE(OSXScreen::secureInputWatchTick(false, true, true), T::None);
}

void OSXScreenTests::secureInputWatchTick_noneWithoutTransition()
{
  using T = OSXScreen::SecureInputTransition;
  QCOMPARE(OSXScreen::secureInputWatchTick(true, false, false), T::None);
  QCOMPARE(OSXScreen::secureInputWatchTick(true, true, true), T::None);
}

void OSXScreenTests::secureInputWatchTick_reportsFalseToTrueEdge()
{
  // A password field just grabbed input: log unconditionally (no boundary
  // sweep is coming to notice this on its own).
  QCOMPARE(OSXScreen::secureInputWatchTick(true, false, true), OSXScreen::SecureInputTransition::TurnedOn);
}

void OSXScreenTests::secureInputWatchTick_reportsTrueToFalseEdge()
{
  // The dialog was dismissed: the caller runs the same ledger-only release
  // K5 already uses at every boundary (see OSXKeyStateTests for proof that
  // release() never touches a physically held modifier).
  QCOMPARE(OSXScreen::secureInputWatchTick(true, true, false), OSXScreen::SecureInputTransition::TurnedOff);
}

void OSXScreenTests::secureInputPollCadence_boundedToAtMostOneHertz()
{
  // "do NOT poll faster than ~1Hz; this must not become a CPU/battery cost"
  QVERIFY(OSXScreen::kSecureInputPollSec >= 1.0);
}

QTEST_MAIN(OSXScreenTests)
