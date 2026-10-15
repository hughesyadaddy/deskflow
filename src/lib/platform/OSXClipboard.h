/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/IClipboard.h"

#include <Carbon/Carbon.h>
#include <vector>

class IOSXClipboardConverter;

//! OS X clipboard implementation
class OSXClipboard : public IClipboard
{
public:
  OSXClipboard();
  //! Use a named pasteboard instead of the general one (tests)
  explicit OSXClipboard(CFStringRef pasteboardName);
  virtual ~OSXClipboard();

  //! Test if clipboard is owned by deskflow
  static bool isOwnedByDeskflow();

  // IClipboard overrides
  bool empty() override;
  void add(Format, const std::string &data) override;
  bool open(Time) const override;
  void close() const override;
  Time getTime() const override;
  bool has(Format) const override;
  std::string get(Format) const override;

  bool synchronize();

  //! Test hook: PasteboardCopyItemFlavors calls made so far
  int flavorListCountForTests() const
  {
    return m_flavorListCount;
  }

private:
  void init(CFStringRef pasteboardName);
  void clearConverters();
  bool itemHasFlavor(PasteboardItemID item, CFStringRef type) const;
  void resetFlavors() const;

private:
  using ConverterList = std::vector<IOSXClipboardConverter *>;

  mutable Time m_time;
  ConverterList m_converters;
  PasteboardRef m_pboard;
  // the item's flavour list, fetched once per open()/close(): every
  // per-flavour query can make the copying app serve its data again (Synergy
  // 7c95affa "list clipboard flavours once per read": a url list hung there)
  mutable CFArrayRef m_flavors = nullptr;
  mutable bool m_open = false;
  mutable int m_flavorListCount = 0;
};

//! Clipboard format converter interface
/*!
This interface defines the methods common to all Scrap book format
*/
class IOSXClipboardConverter
{
public:
  virtual ~IOSXClipboardConverter() = default;
  //! @name accessors
  //@{

  //! Get clipboard format
  /*!
  Return the clipboard format this object converts from/to.
  */
  virtual IClipboard::Format getFormat() const = 0;

  //! returns the scrap flavor type that this object converts from/to
  virtual CFStringRef getOSXFormat() const = 0;

  //! Convert from IClipboard format
  /*!
  Convert from the IClipboard format to the Carbon scrap format.
  The input data must be in the IClipboard format returned by
  getFormat().  The return data will be in the scrap
  format returned by getOSXFormat().
  */
  virtual std::string fromIClipboard(const std::string &) const = 0;

  //! Convert to IClipboard format
  /*!
  Convert from the carbon scrap format to the IClipboard format
  (i.e., the reverse of fromIClipboard()).
  */
  virtual std::string toIClipboard(const std::string &) const = 0;

  //@}
};
