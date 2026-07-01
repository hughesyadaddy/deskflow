/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/OSXKeyLayout.h"
#include "platform/OSXAutoTypes.h"

#include <dispatch/dispatch.h>
#include <mutex>
#include <pthread.h>

namespace deskflow::platform::osx {

UnicodeKeyLayoutSnapshot::~UnicodeKeyLayoutSnapshot()
{
  if (layoutBytes != nullptr) {
    CFRelease(layoutBytes);
  }
}

UnicodeKeyLayoutSnapshot::UnicodeKeyLayoutSnapshot(UnicodeKeyLayoutSnapshot &&other) noexcept
    : layoutBytes(other.layoutBytes),
      keyboardType(other.keyboardType)
{
  other.layoutBytes = nullptr;
}

UnicodeKeyLayoutSnapshot &UnicodeKeyLayoutSnapshot::operator=(UnicodeKeyLayoutSnapshot &&other) noexcept
{
  if (this != &other) {
    if (layoutBytes != nullptr) {
      CFRelease(layoutBytes);
    }
    layoutBytes = other.layoutBytes;
    keyboardType = other.keyboardType;
    other.layoutBytes = nullptr;
  }
  return *this;
}

namespace {

UnicodeKeyLayoutSnapshot fetchUnicodeKeyLayoutSnapshot()
{
  UnicodeKeyLayoutSnapshot snapshot;
  snapshot.keyboardType = LMGetKbdType();

  std::lock_guard<std::mutex> lock(g_tisMutex);
  AutoTISInputSourceRef layoutSource(TISCopyCurrentKeyboardLayoutInputSource(), CFRelease);
  if (!layoutSource) {
    return snapshot;
  }

  CFDataRef ref = static_cast<CFDataRef>(
      TISGetInputSourceProperty(layoutSource.get(), kTISPropertyUnicodeKeyLayoutData)
  );
  if (!ref) {
    return snapshot;
  }

  CFRetain(ref);
  snapshot.layoutBytes = ref;
  return snapshot;
}

} // namespace

UnicodeKeyLayoutSnapshot copyUnicodeKeyLayoutOnMainQueue()
{
  if (pthread_main_np() != 0) {
    return fetchUnicodeKeyLayoutSnapshot();
  }

  UnicodeKeyLayoutSnapshot snapshot;
  UnicodeKeyLayoutSnapshot *snapshotOut = &snapshot;
  dispatch_sync(dispatch_get_main_queue(), ^{ *snapshotOut = fetchUnicodeKeyLayoutSnapshot(); });
  return snapshot;
}

} // namespace deskflow::platform::osx
