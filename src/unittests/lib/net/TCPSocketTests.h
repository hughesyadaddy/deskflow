/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QTest>

class TCPSocketTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();
  void caps_defaultsAreDocumentedValues();
  void defaultCap_appliesToNewSockets();
  void writeWithinCap_buffersWithoutError();
  void writePastCap_raisesBackpressureAndDropsQueue();
};
