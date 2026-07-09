/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ChordRemapDialog.h"
#include "ui_ChordRemapDialog.h"

ChordRemapDialog::ChordRemapDialog(QWidget *parent, ChordRemap &remap)
    : QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint),
      ui{std::make_unique<Ui::ChordRemapDialog>()},
      m_remap(remap)
{
  ui->setupUi(this);

  ui->m_pKeySequenceWidgetIn->setKeySequence(remap.inSequence());
  ui->m_pKeySequenceWidgetOut->setKeySequence(remap.outSequence());
}

ChordRemapDialog::~ChordRemapDialog() = default;

void ChordRemapDialog::accept()
{
  if (!ui->m_pKeySequenceWidgetIn->valid() || !ui->m_pKeySequenceWidgetOut->valid()) {
    return;
  }

  remap().setInSequence(ui->m_pKeySequenceWidgetIn->keySequence());
  remap().setOutSequence(ui->m_pKeySequenceWidgetOut->keySequence());
  QDialog::accept();
}
