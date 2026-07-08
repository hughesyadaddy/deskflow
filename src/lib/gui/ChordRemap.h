/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "KeySequence.h"

#include <QString>
#include <QList>

namespace deskflow::server {
struct ChordRemapEntry;
}

class ChordRemapDialog;
class ServerConfigDialog;
class QSettings;
class QTextStream;

class ChordRemap
{
  friend class ChordRemapDialog;
  friend class ServerConfigDialog;
  friend QTextStream &operator<<(QTextStream &outStream, const ChordRemap &remap);

public:
  ChordRemap() = default;

  QString text() const;
  const QString &screen() const
  {
    return m_screen;
  }
  void setScreen(const QString &screen)
  {
    m_screen = screen;
  }
  const KeySequence &inSequence() const
  {
    return m_inSequence;
  }
  const KeySequence &outSequence() const
  {
    return m_outSequence;
  }

  void loadSettings(QSettings &settings);
  void saveSettings(QSettings &settings) const;

  bool operator==(const ChordRemap &other) const;

  static ChordRemap fromServerEntry(const deskflow::server::ChordRemapEntry &entry);

protected:
  KeySequence &inSequence()
  {
    return m_inSequence;
  }
  KeySequence &outSequence()
  {
    return m_outSequence;
  }
  void setInSequence(const KeySequence &seq)
  {
    m_inSequence = seq;
  }
  void setOutSequence(const KeySequence &seq)
  {
    m_outSequence = seq;
  }

private:
  QString m_screen;
  KeySequence m_inSequence;
  KeySequence m_outSequence;

  inline static const QString kInSequence = QStringLiteral("inSequence");
  inline static const QString kOutSequence = QStringLiteral("outSequence");
};

using ChordRemapList = QList<ChordRemap>;

QTextStream &operator<<(QTextStream &outStream, const ChordRemap &remap);
