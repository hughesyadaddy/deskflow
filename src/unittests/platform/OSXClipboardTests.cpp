/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2011 Nick Bolton
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "OSXClipboardTests.h"

#include "platform/OSXClipboard.h"
#include "platform/OSXClipboardUTF8Converter.h"

void OSXClipboardTests::open()
{
  OSXClipboard clipboard;
  QVERIFY(clipboard.open(0));
  QVERIFY(clipboard.empty());
  clipboard.close();
}

void OSXClipboardTests::singleFormat()
{
  using enum IClipboard::Format;

  OSXClipboard clipboard;
  QVERIFY(clipboard.empty());
  clipboard.add(Text, m_testString);
  QVERIFY(clipboard.has(Text));
  QCOMPARE(clipboard.get(Text), m_testString);
}

void OSXClipboardTests::formatConvert_UTF8()
{
  OSXClipboardUTF8Converter converter;
  QCOMPARE(IClipboard::Format::Text, converter.getFormat());
  QCOMPARE(converter.getOSXFormat(), CFSTR("public.utf8-plain-text"));
  QCOMPARE(converter.fromIClipboard("test data\n"), "test data\r");
  QCOMPARE(converter.toIClipboard("test data\r"), "test data\n");
}

QTEST_MAIN(OSXClipboardTests)

void OSXClipboardTests::privatePasteboard_listsFlavorsOncePerOpen()
{
  using enum IClipboard::Format;

  OSXClipboard clipboard(CFSTR("org.deskflow.unittest.flavors"));
  QVERIFY(clipboard.open(0));
  QVERIFY(clipboard.empty());
  clipboard.add(Text, m_testString);
  clipboard.close();

  // a whole read (has() for every format, get() for the present one) lists
  // the item's flavours once, instead of querying the owner per flavour
  QVERIFY(clipboard.open(0));
  const int before = clipboard.flavorListCountForTests();
  QVERIFY(clipboard.has(Text));
  QVERIFY(!clipboard.has(HTML));
  QVERIFY(!clipboard.has(Bitmap));
  QCOMPARE(clipboard.get(Text), m_testString);
  QCOMPARE(clipboard.get(HTML), std::string());
  QCOMPARE(clipboard.flavorListCountForTests(), before + 1);
  clipboard.close();

  // the list does not outlive the read: the next open lists again
  QVERIFY(clipboard.open(0));
  QVERIFY(clipboard.has(Text));
  QCOMPARE(clipboard.flavorListCountForTests(), before + 2);
  QVERIFY(clipboard.empty());
  QVERIFY(!clipboard.has(Text)); // emptied: stale list dropped
  clipboard.close();
}
