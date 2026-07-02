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
  void mapKeyFromEventOffMainThread_keyDown();
  void mapKeyFromEventOffMainThread_keyUp();
  void mapKeyFromEventOffMainThread_autorepeat();
  void mapKeyFromEventOffMainThread_matchesMainThread();

private:
  bool isKeyPressed(const OSXKeyState &keyState, KeyButton button);
  bool probeOsKeyInjection();
  bool probeOsShiftInjection();

  Arch m_arch;
  Log m_log;
  bool m_osKeyInjectionWorks = false;
  bool m_osShiftInjectionWorks = false;
};
