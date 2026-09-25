/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/KeyLogToken.h"

#include "base/Log.h"
#include "base/String.h"
#include "deskflow/IKeyState.h"

#include <atomic>

namespace deskflow {

namespace {
std::atomic<bool> g_keystrokeLoggingEnabled{false};
constexpr auto kRedacted = "<redacted>";
} // namespace

void setKeystrokeLoggingEnabled(bool enabled)
{
  const bool was = g_keystrokeLoggingEnabled.exchange(enabled);
  if (!enabled || was) {
    return;
  }
  if (keystrokeLoggingActive()) {
    LOG_WARN("keystroke logging ENABLED by [log] keystrokes=true -- this log will contain typed content");
  } else {
    LOG_INFO("[log] keystrokes=true is set but [log] level is below VERBOSE; typed content stays redacted");
  }
}

bool keystrokeLoggingEnabled()
{
  return g_keystrokeLoggingEnabled.load();
}

bool keystrokeLoggingActive()
{
  return g_keystrokeLoggingEnabled.load() && CLOG->getFilter() >= LogLevel::Level::Verbose;
}

bool isKeyLogAllowlisted(KeyID id)
{
  // Modifiers (Shift_L .. Hyper_R) and function keys (F1 .. F35) are two
  // contiguous X11 keysym blocks; the rest are spelled out. KP digits and
  // KP operators are deliberately NOT here: they are typed content.
  if (id >= kKeyF1 && id <= kKeyHyper_R) {
    return true;
  }
  if (id >= kKeyHome && id <= kKeyBegin) {
    return true;
  }
  if (id >= kKeyKP_Home && id <= kKeyKP_Delete) {
    return true;
  }
  switch (id) {
  case kKeyBackSpace:
  case kKeyTab:
  case kKeyLeftTab:
  case kKeyClear:
  case kKeyReturn:
  case kKeyPause:
  case kKeyScrollLock:
  case kKeyEscape:
  case kKeyDelete:
  case kKeyPrint:
  case kKeyInsert:
  case kKeyMenu:
  case kKeyBreak:
  case kKeyAltGr:
  case kKeyNumLock:
  case kKeyKP_Tab:
  case kKeyKP_Enter:
    return true;
  default:
    return false;
  }
}

std::string keyLogToken(KeyID id)
{
  if (isKeyLogAllowlisted(id) || keystrokeLoggingActive()) {
    return "id=" + IKeyState::describeKey(id);
  }
  return std::string("id=") + kRedacted;
}

std::string keyLogToken(KeyID id, KeyButton button)
{
  if (isKeyLogAllowlisted(id) || keystrokeLoggingActive()) {
    return deskflow::string::sprintf("id=%s button=0x%04x", IKeyState::describeKey(id).c_str(), button);
  }
  return deskflow::string::sprintf("id=%s button=%s", kRedacted, kRedacted);
}

std::string keyLogCode(uint32_t code)
{
  if (keystrokeLoggingActive()) {
    return deskflow::string::sprintf("0x%04x", code);
  }
  return kRedacted;
}

} // namespace deskflow
