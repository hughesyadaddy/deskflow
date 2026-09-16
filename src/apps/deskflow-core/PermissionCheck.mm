/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "PermissionCheck.h"

#import <ApplicationServices/ApplicationServices.h>
#import <IOKit/hidsystem/IOHIDLib.h>

namespace deskflow::core::permissions {

// Read-only TCC probe. AXIsProcessTrusted() (not ...WithOptions) and
// IOHIDCheckAccess() never present a prompt, so this is safe from a headless
// SSH session and from fleet-health cron jobs.
Status probe()
{
  Status status;
  status.supported = true;
  status.accessibility = AXIsProcessTrusted();
  status.inputMonitoring = IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) == kIOHIDAccessTypeGranted;
  status.postEvent = IOHIDCheckAccess(kIOHIDRequestTypePostEvent) == kIOHIDAccessTypeGranted;
  return status;
}

} // namespace deskflow::core::permissions
