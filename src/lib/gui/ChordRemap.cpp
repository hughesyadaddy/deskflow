/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ChordRemap.h"

#include "deskflow/KeyMap.h"
#include "server/ChordRemapTypes.h"

#include <QSettings>
#include <Qt>

namespace {

QString normalizeChordSpec(QString spec)
{
  return spec.replace(QStringLiteral("Meta"), QStringLiteral("Super"));
}

QString chordSpecFromSequence(const KeySequence &sequence)
{
  return normalizeChordSpec(sequence.toString());
}

int keyIdToQtKey(KeyID id)
{
  if (id >= 32 && id < 127) {
    return static_cast<int>(id);
  }
  switch (id) {
  case kKeyTab:
    return Qt::Key_Tab;
  case kKeyF4:
    return Qt::Key_F4;
  case kKeyF12:
    return Qt::Key_F12;
  case kKeyDown:
    return Qt::Key_Down;
  default:
    return 0;
  }
}

KeySequence keySequenceFromChordSpec(const QString &specQt)
{
  std::string spec = normalizeChordSpec(specQt).toStdString();
  KeyModifierMask mask = 0;
  if (!deskflow::KeyMap::parseModifiers(spec, mask)) {
    return {};
  }
  KeyID id = kKeyNone;
  if (!spec.empty() && !deskflow::KeyMap::parseKey(spec, id)) {
    return {};
  }

  KeySequence seq;
  const auto appendMod = [&](KeyModifierMask bit, int qtMod) {
    if ((mask & bit) != 0) {
      seq.appendKey(qtMod, seq.modifiers());
    }
  };
  appendMod(KeyModifierShift, Qt::ShiftModifier);
  appendMod(KeyModifierControl, Qt::ControlModifier);
  appendMod(KeyModifierAlt, Qt::AltModifier);
  appendMod(KeyModifierSuper, Qt::MetaModifier);

  if (id != kKeyNone) {
    const int qtKey = keyIdToQtKey(id);
    if (qtKey != 0) {
      seq.appendKey(qtKey, seq.modifiers());
    }
  }
  return seq;
}

KeySequence keySequenceFromServerChord(KeyModifierMask mask, KeyID id)
{
  return keySequenceFromChordSpec(QString::fromStdString(deskflow::KeyMap::formatKey(id, mask)));
}

} // namespace

QString ChordRemap::text() const
{
  return QStringLiteral("%1 → %2").arg(chordSpecFromSequence(m_inSequence), chordSpecFromSequence(m_outSequence));
}

void ChordRemap::loadSettings(QSettings &settings)
{
  m_screen = settings.value(QStringLiteral("screen")).toString();
  settings.beginGroup(kInSequence);
  m_inSequence.loadSettings(settings);
  settings.endGroup();
  settings.beginGroup(kOutSequence);
  m_outSequence.loadSettings(settings);
  settings.endGroup();
}

void ChordRemap::saveSettings(QSettings &settings) const
{
  settings.setValue(QStringLiteral("screen"), m_screen);
  settings.beginGroup(kInSequence);
  m_inSequence.saveSettings(settings);
  settings.endGroup();
  settings.beginGroup(kOutSequence);
  m_outSequence.saveSettings(settings);
  settings.endGroup();
}

bool ChordRemap::operator==(const ChordRemap &other) const
{
  return m_screen == other.m_screen && m_inSequence == other.m_inSequence && m_outSequence == other.m_outSequence;
}

ChordRemap ChordRemap::fromServerEntry(const deskflow::server::ChordRemapEntry &entry)
{
  ChordRemap remap;
  remap.m_screen = QString::fromStdString(entry.screen);
  remap.m_inSequence = keySequenceFromServerChord(entry.inMods, entry.inKey);
  remap.m_outSequence = keySequenceFromServerChord(entry.outMods, entry.outKey);
  return remap;
}

QTextStream &operator<<(QTextStream &outStream, const ChordRemap &remap)
{
  const QString inSpec = chordSpecFromSequence(remap.inSequence());
  const QString outSpec = chordSpecFromSequence(remap.outSequence());
  if (inSpec.isEmpty() || outSpec.isEmpty()) {
    return outStream;
  }
  outStream << QStringLiteral("\t\tchordRemap(%1) = %2\n").arg(inSpec, outSpec);
  return outStream;
}
