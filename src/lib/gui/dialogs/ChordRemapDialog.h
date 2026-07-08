/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "ChordRemap.h"

#include <QDialog>
#include <memory>

class KeySequenceWidget;

namespace Ui {
class ChordRemapDialog;
}

class ChordRemapDialog : public QDialog
{
  Q_OBJECT

public:
  ChordRemapDialog(QWidget *parent, ChordRemap &remap);
  ~ChordRemapDialog() override;

  const ChordRemap &remap() const
  {
    return m_remap;
  }

protected Q_SLOTS:
  void accept() override;

protected:
  ChordRemap &remap()
  {
    return m_remap;
  }

private:
  std::unique_ptr<Ui::ChordRemapDialog> ui;
  ChordRemap &m_remap;
};
