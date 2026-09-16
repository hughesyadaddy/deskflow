/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "server/VirtualHostTracker.h"

#include "server/BaseClientProxy.h"

#include <QJsonDocument>
#include <QJsonObject>

std::string VirtualHostTracker::focusLine(const std::string &screen, bool here)
{
  QJsonObject object;
  object[QStringLiteral("t")] = QStringLiteral("focus");
  object[QStringLiteral("screen")] = QString::fromStdString(screen);
  object[QStringLiteral("here")] = here;
  return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}

void VirtualHostTracker::setConnectLine(std::string line)
{
  if (line != m_connectLine) {
    // New device identity (or none): whoever had the old one must be told
    // again before it can host this one.
    m_attached.clear();
  }
  m_connectLine = std::move(line);
}

bool VirtualHostTracker::hasConnectLine() const
{
  return !m_connectLine.empty();
}

const std::string &VirtualHostTracker::connectLine() const
{
  return m_connectLine;
}

BaseClientProxy *VirtualHostTracker::host() const
{
  return m_host;
}

void VirtualHostTracker::clearHostIf(BaseClientProxy *client)
{
  m_attached.erase(client);
  if (m_host == client) {
    m_host = nullptr;
  }
}

bool VirtualHostTracker::hostsActiveClient(BaseClientProxy *active) const
{
  return m_host != nullptr && m_host == active;
}

bool VirtualHostTracker::isAttached(const BaseClientProxy *client) const
{
  return m_attached.count(const_cast<BaseClientProxy *>(client)) != 0;
}
