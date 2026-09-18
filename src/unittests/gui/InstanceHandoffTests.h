/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QTest>

class InstanceHandoffTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void quit_is_honoured_by_unmanaged_instance();
  void quit_is_refused_by_launchd_owned_instance();
  void show_and_legacy_bare_connection_raise_window();
  void requests_fail_when_nothing_listens();
};
