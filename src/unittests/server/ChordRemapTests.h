/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QTest>

class ChordRemapTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void applyChordRemap_exactMatch();
  void applyChordRemap_wrongScreen();
  void applyChordRemap_preservesCapsLock();
  void applyChordRemap_firstMatchPrecedence();
  void needsHoldThrough_defaultRows();
  void seedDefaultChordRemaps_whenTiny11Present();
  void seedDefaultChordRemaps_skipsWithoutTiny11();
};
