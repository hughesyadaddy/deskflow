/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2016 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "KeyMapTests.h"

#include "deskflow/KeyMap.h"

using namespace deskflow;
using KeyItemList = KeyMap::KeyItemList;
using KeyEntryList = std::vector<KeyItemList>;

void KeyMapTests::findBestKey_requiredDown_matchExactFirstItem()
{
  KeyMap keyMap;
  KeyEntryList entryList;
  KeyItemList itemList;
  KeyMap::KeyItem item;
  item.m_required = KeyModifierShift;
  item.m_sensitive = KeyModifierShift;
  KeyModifierMask desiredState = KeyModifierShift;
  itemList.push_back(item);
  entryList.push_back(itemList);

  QCOMPARE(keyMap.findBestKey(entryList, desiredState), 0);
}

void KeyMapTests::findBestKey_requiredAndExtraSensitiveDown_matchExactFirstItem()
{
  KeyMap keyMap;
  KeyEntryList entryList;
  KeyItemList itemList;
  KeyMap::KeyItem item;
  item.m_required = KeyModifierShift;
  item.m_sensitive = KeyModifierShift | KeyModifierAlt;
  KeyModifierMask desiredState = KeyModifierShift;
  itemList.push_back(item);
  entryList.push_back(itemList);

  QCOMPARE(keyMap.findBestKey(entryList, desiredState), 0);
}

void KeyMapTests::findBestKey_requiredAndExtraSensitiveDown_matchExactSecondItem()
{
  KeyMap keyMap;
  KeyEntryList entryList;
  KeyItemList itemList1;
  KeyMap::KeyItem item1;
  item1.m_required = KeyModifierAlt;
  item1.m_sensitive = KeyModifierShift | KeyModifierAlt;
  KeyMap::KeyItemList itemList2;
  KeyMap::KeyItem item2;
  item2.m_required = KeyModifierShift;
  item2.m_sensitive = KeyModifierShift | KeyModifierAlt;
  KeyModifierMask desiredState = KeyModifierShift;
  itemList1.push_back(item1);
  itemList2.push_back(item2);
  entryList.push_back(itemList1);
  entryList.push_back(itemList2);
  QCOMPARE(keyMap.findBestKey(entryList, desiredState), 1);
}

void KeyMapTests::findBestKey_extraSensitiveDown_matchExactSecondItem()
{
  KeyMap keyMap;
  KeyEntryList entryList;
  KeyItemList itemList1;
  KeyMap::KeyItem item1;
  item1.m_required = 0;
  item1.m_sensitive = KeyModifierAlt;
  KeyMap::KeyItemList itemList2;
  KeyMap::KeyItem item2;
  item2.m_required = 0;
  item2.m_sensitive = KeyModifierShift;
  KeyModifierMask desiredState = KeyModifierAlt;
  itemList1.push_back(item1);
  itemList2.push_back(item2);
  entryList.push_back(itemList1);
  entryList.push_back(itemList2);

  QCOMPARE(keyMap.findBestKey(entryList, desiredState), 1);
}

void KeyMapTests::findBestKey_noRequiredDown_matchOneRequiredChangeItem()
{
  KeyMap keyMap;
  KeyEntryList entryList;
  KeyItemList itemList1;
  KeyMap::KeyItem item1;
  item1.m_required = KeyModifierShift | KeyModifierAlt;
  item1.m_sensitive = KeyModifierShift | KeyModifierAlt;
  KeyMap::KeyItemList itemList2;
  KeyMap::KeyItem item2;
  item2.m_required = KeyModifierShift;
  item2.m_sensitive = KeyModifierShift | KeyModifierAlt;
  KeyModifierMask desiredState = 0;
  itemList1.push_back(item1);
  itemList2.push_back(item2);
  entryList.push_back(itemList1);
  entryList.push_back(itemList2);

  QCOMPARE(keyMap.findBestKey(entryList, desiredState), 1);
}

void KeyMapTests::findBestKey_onlyOneRequiredDown_matchTwoRequiredChangesItem()
{
  KeyMap keyMap;
  KeyEntryList entryList;
  KeyItemList itemList1;
  KeyMap::KeyItem item1;
  item1.m_required = KeyModifierShift | KeyModifierAlt | KeyModifierControl;
  item1.m_sensitive = KeyModifierShift | KeyModifierAlt | KeyModifierControl;
  KeyItemList itemList2;
  KeyMap::KeyItem item2;
  item2.m_required = KeyModifierShift | KeyModifierAlt;
  item2.m_sensitive = KeyModifierShift | KeyModifierAlt | KeyModifierControl;
  KeyModifierMask desiredState = 0;
  itemList1.push_back(item1);
  itemList2.push_back(item2);
  entryList.push_back(itemList1);
  entryList.push_back(itemList2);

  QCOMPARE(keyMap.findBestKey(entryList, desiredState), 1);
}

void KeyMapTests::findBestKey_noRequiredDown_cannotMatch()
{
  KeyMap keyMap;
  KeyEntryList entryList;
  KeyItemList itemList;
  KeyMap::KeyItem item;
  item.m_required = 0xffffffff;
  item.m_sensitive = 0xffffffff;
  KeyModifierMask desiredState = 0;
  itemList.push_back(item);
  entryList.push_back(itemList);

  QCOMPARE(keyMap.findBestKey(entryList, desiredState), -1);
}

void KeyMapTests::isCommand()
{
  KeyMap keyMap;
  KeyModifierMask mask = KeyModifierShift;
  QVERIFY(!keyMap.isCommand(mask));

  mask = KeyModifierControl;
  QVERIFY(keyMap.isCommand(mask));

  mask = KeyModifierAlt;
  QVERIFY(keyMap.isCommand(mask));

  mask = KeyModifierAltGr;
  QVERIFY(keyMap.isCommand(mask));

  mask = KeyModifierMeta;
  QVERIFY(keyMap.isCommand(mask));

  mask = KeyModifierSuper;
  QVERIFY(keyMap.isCommand(mask));
}

void KeyMapTests::mapkey()
{
  KeyMap keyMap{};
  KeyMap::Keystroke stroke('A', true, false, 1);
  KeyMap::KeyItem keyItem;
  keyItem.m_button = 'A';
  keyItem.m_group = 1;
  keyItem.m_id = 'A';
  keyMap.addKeyEntry(keyItem);
  keyMap.finish();
  KeyMap::Keystrokes strokes{stroke};
  KeyMap::ModifierToKeys activeModifiers{};
  KeyModifierMask currentState{};
  KeyModifierMask desiredMask{};
  auto result = keyMap.mapKey(strokes, kKeySetModifiers, 1, activeModifiers, currentState, desiredMask, false, "en");
  QVERIFY(result != nullptr);
  desiredMask = KeyModifierControl;
  result = keyMap.mapKey(strokes, kKeySetModifiers, 1, activeModifiers, currentState, desiredMask, false, "en");
  QVERIFY(result == nullptr);
}

namespace {

constexpr KeyButton kShiftButton = 0x38;
constexpr KeyButton kCapsButton = 0x3A;
constexpr KeyButton kKButton = 0x28;

// The shape a real US layout produces for one letter key: a Shift modifier,
// a locking Caps Lock modifier, and 'k' / 'K' on the same button where the
// case is decided by Shift XOR Caps (Shift+Caps composes lowercase).
void addLetterLayout(KeyMap &map)
{
  KeyMap::KeyItem shift;
  shift.m_id = kKeyShift_L;
  shift.m_group = 0;
  shift.m_button = kShiftButton;
  shift.m_generates = KeyModifierShift;
  map.addKeyEntry(shift);

  KeyMap::KeyItem caps;
  caps.m_id = kKeyCapsLock;
  caps.m_group = 0;
  caps.m_button = kCapsButton;
  caps.m_generates = KeyModifierCapsLock;
  caps.m_lock = true;
  map.addKeyEntry(caps);

  const KeyModifierMask sensitive = KeyModifierShift | KeyModifierCapsLock;
  struct Row
  {
    KeyID id;
    KeyModifierMask required;
  };
  for (const Row &row :
       {Row{'k', 0}, Row{'K', KeyModifierShift}, Row{'K', KeyModifierCapsLock},
        Row{'k', KeyModifierShift | KeyModifierCapsLock}}) {
    KeyMap::KeyItem item;
    item.m_id = row.id;
    item.m_group = 0;
    item.m_button = kKButton;
    item.m_required = row.required;
    item.m_sensitive = sensitive;
    map.addKeyEntry(item);
  }
  map.finish();
}

struct ButtonStroke
{
  KeyButton button;
  bool press;
};

std::vector<ButtonStroke> buttonStrokes(const KeyMap::Keystrokes &keys)
{
  std::vector<ButtonStroke> out;
  for (const auto &k : keys) {
    if (k.m_type == KeyMap::Keystroke::KeyType::Button) {
      out.push_back({k.m_data.m_button.m_button, k.m_data.m_button.m_press});
    }
  }
  return out;
}

bool hasStroke(const std::vector<ButtonStroke> &strokes, KeyButton button, bool press)
{
  for (const auto &s : strokes) {
    if (s.button == button && s.press == press) {
      return true;
    }
  }
  return false;
}

} // namespace

// Cross-machine vector for the login-screen capitalization fix: a server
// that sends 'K' with an EMPTY mask (uppercase composed via Caps Lock on
// its side, or a relay that normalised the mask away) must still produce a
// capital on a client whose caps is off -- the KeyID names the character,
// so the client presses Shift itself.
void KeyMapTests::mapKey_upperLetterWithoutShiftInMask_pressesShift()
{
  KeyMap keyMap;
  addLetterLayout(keyMap);
  KeyMap::Keystrokes keys;
  KeyMap::ModifierToKeys activeModifiers;
  KeyModifierMask currentState = 0;
  KeyModifierMask desiredMask = 0;
  const auto *item = keyMap.mapKey(keys, 'K', 0, activeModifiers, currentState, desiredMask, false, "en");
  QVERIFY(item != nullptr);
  QCOMPARE(item->m_button, kKButton);
  const auto strokes = buttonStrokes(keys);
  QVERIFY(hasStroke(strokes, kShiftButton, true));
  QVERIFY(hasStroke(strokes, kKButton, true));
  QVERIFY(!hasStroke(strokes, kCapsButton, true));
  // Shift is pressed BEFORE the letter.
  size_t shiftAt = strokes.size(), keyAt = strokes.size();
  for (size_t i = 0; i < strokes.size(); ++i) {
    if (strokes[i].button == kShiftButton && strokes[i].press && shiftAt == strokes.size())
      shiftAt = i;
    if (strokes[i].button == kKButton && strokes[i].press)
      keyAt = i;
  }
  QVERIFY(shiftAt < keyAt);
}

// 'k' with Shift in the desired mask: the KeyID is lowercase, so the
// client must NOT press Shift (which would type 'K'); Shift is only
// "desired", never required for a base letter.
void KeyMapTests::mapKey_lowerLetterWithShiftDesired_noShiftKeystroke()
{
  KeyMap keyMap;
  addLetterLayout(keyMap);
  KeyMap::Keystrokes keys;
  KeyMap::ModifierToKeys activeModifiers;
  KeyModifierMask currentState = 0;
  KeyModifierMask desiredMask = KeyModifierShift;
  const auto *item = keyMap.mapKey(keys, 'k', 0, activeModifiers, currentState, desiredMask, false, "en");
  QVERIFY(item != nullptr);
  QCOMPARE(item->m_button, kKButton);
  const auto strokes = buttonStrokes(keys);
  QVERIFY(hasStroke(strokes, kKButton, true));
  QVERIFY(!hasStroke(strokes, kShiftButton, true));
  QVERIFY(!hasStroke(strokes, kCapsButton, true));
}

// 'K' while the client's Caps Lock is already on and the server also says
// Shift|Caps: mapKey never toggles Caps to satisfy a mask (it takes the
// current lock state as given), so no caps press is synthesised and no
// Shift either -- caps alone composes the capital.
void KeyMapTests::mapKey_upperLetterWithShiftAndCaps_noExtraCapsPress()
{
  KeyMap keyMap;
  addLetterLayout(keyMap);
  KeyMap::Keystrokes keys;
  KeyMap::ModifierToKeys activeModifiers;
  KeyModifierMask currentState = KeyModifierCapsLock;
  KeyModifierMask desiredMask = KeyModifierShift | KeyModifierCapsLock;
  const auto *item = keyMap.mapKey(keys, 'K', 0, activeModifiers, currentState, desiredMask, false, "en");
  QVERIFY(item != nullptr);
  QCOMPARE(item->m_button, kKButton);
  const auto strokes = buttonStrokes(keys);
  QVERIFY(hasStroke(strokes, kKButton, true));
  QVERIFY(!hasStroke(strokes, kCapsButton, true));
  QVERIFY(!hasStroke(strokes, kCapsButton, false));
  QVERIFY(!hasStroke(strokes, kShiftButton, true));
  QVERIFY((currentState & KeyModifierCapsLock) != 0);
}

void KeyMapTests::parseModifiers_plusKey_keepsPlusAsKey()
{
  std::string keystroke = "Control+Shift++";
  KeyModifierMask mask = 0;

  QVERIFY(KeyMap::parseModifiers(keystroke, mask));
  QCOMPARE(mask, static_cast<KeyModifierMask>(KeyModifierControl | KeyModifierShift));
  QCOMPARE(keystroke, std::string("+"));
}

void KeyMapTests::parseKey_plusSymbol_parsesAsAsciiKey()
{
  KeyID key = kKeyNone;

  QVERIFY(KeyMap::parseKey("+", key));
  QCOMPARE(key, static_cast<KeyID>('+'));
}

QTEST_MAIN(KeyMapTests)
