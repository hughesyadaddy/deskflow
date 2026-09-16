/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "arch/Arch.h"
#include "base/Log.h"

#include <QObject>

class MouserLinkTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void helloThenAttachRoleAndFocus();
  void protoMismatchSleepsAndLogsOnce();
  void authRejectionRereadsTokenAfterDelay();
  void byeSuppressesBackoff();
  void reconnectReusesSessionId();
  void focusSwitchSendsFocusNotDisconnect();
  void survivesServerEpochChurnWithOneAttach();
  void dropsAfterUnansweredPings();
  void legacyListenerWhenTokenFileAbsent();
  void legacyConnectorTranslatesFocus();

private:
  Arch m_arch;
  Log m_log;
};
