/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

namespace deskflow::platform::mswindows {

//! Drive the Win32 ShowCursor counter until the pointer is shown or hidden.
bool setCursorVisibility(bool visible);

} // namespace deskflow::platform::mswindows
