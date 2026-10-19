/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <QObject>

class OSXCmdDiagTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void cmdOn_detectsCommandBit();
  void noteEnterLeave_updatesBoundaryFields();
};
