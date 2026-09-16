/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QTest>

class StreamBufferTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void peekWithinHead_doesNotCopyOrReallocate();
  void peekAcrossChunks_consolidatesOnce();
  void peekGrowing_amortizesReallocation();
  void popDrain_releasesChunks();
  void popPartial_compactsOversizedHead();
  void fifoOrder_preservedAcrossConsolidation();
};
