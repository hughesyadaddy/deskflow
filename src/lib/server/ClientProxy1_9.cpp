/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ClientProxy1_9.h"

#include "base/Log.h"
#include "deskflow/ProtocolUtil.h"

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
