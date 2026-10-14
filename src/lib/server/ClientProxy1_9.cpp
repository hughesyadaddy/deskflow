/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ClientProxy1_9.h"

#include "base/Log.h"
#include "coordination/RelayKeyEvent.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "server/Server.h"

#include <cstring>

ClientProxy1_9::ClientProxy1_9(
    const std::string &name, deskflow::IStream *adoptedStream, Server *server, IEventQueue *events
)
    : ClientProxy1_8(name, adoptedStream, server, events)
{
}

void ClientProxy1_9::mouseWheel(int32_t xDelta, int32_t yDelta)
{
  mouseWheelEx(WheelEx::fromNotches(xDelta, yDelta));
}

void ClientProxy1_9::mouseWheelEx(const WheelEx &ex)
{
  LOG_VERBOSE(
      "send mouse wheel ex to \"%s\" %+d,%+d cont=%d phase=%d momentum=%d ts=%u", getName().c_str(), ex.xDelta,
      ex.yDelta, ex.continuous, static_cast<int>(ex.phase), static_cast<int>(ex.momentum), ex.timestampMs
  );
  ProtocolUtil::writef(
      getStream(), kMsgDMouseWheelEx, ex.xDelta, ex.yDelta, static_cast<uint32_t>(ex.continuous ? 1 : 0),
      static_cast<uint32_t>(ex.phase), static_cast<uint32_t>(ex.momentum), ex.timestampMs
  );
}

bool ClientProxy1_9::parseMessage(const uint8_t *code)
{
  if (memcmp(code, kMsgDKeyRelay, 4) == 0) {
    return recvKeyRelay();
  }
  if (memcmp(code, kMsgCKeyRelayClear, 4) == 0) {
    LOG_INFO("[relay] \"%s\" asked to release every key it relayed", getName().c_str());
    if (getServer() != nullptr) {
      getServer()->releaseForwardedKeys(getName());
    }
    return true;
  }
  return ClientProxy1_8::parseMessage(code);
}

bool ClientProxy1_9::recvKeyRelay()
{
  uint8_t phase = 0;
  uint32_t id = 0;
  uint16_t mask = 0;
  uint16_t button = 0;
  std::string lang;
  if (!ProtocolUtil::readf(getStream(), kMsgDKeyRelay + 4, &phase, &id, &mask, &button, &lang)) {
    return false;
  }
  using deskflow::coordination::RelayKeyPhase;
  deskflow::coordination::RelayKeyEvent event;
  event.phase = phase == 1 ? RelayKeyPhase::Up : phase == 2 ? RelayKeyPhase::Repeat : RelayKeyPhase::Down;
  event.id = static_cast<KeyID>(id);
  event.mask = static_cast<KeyModifierMask>(mask);
  event.button = static_cast<KeyButton>(button);
  event.lang = lang;
  event.from = getName();
  LOG_DEBUG("[relay] key %d from \"%s\" button=0x%04x", static_cast<int>(phase), getName().c_str(), button);
  if (getServer() != nullptr) {
    getServer()->relayForwardedKey(event);
  }
  return true;
}
