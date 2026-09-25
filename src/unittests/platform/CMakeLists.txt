# SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
# SPDX-License-Identifier: MIT

# Platform-neutral: InjectionCoalesce.h is a pure header (no Win32 deps).
create_test(
  NAME InjectionCoalesceTests
  DEPENDS platform
  LIBS base arch
  SOURCE InjectionCoalesceTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/platform"
)

# Platform-neutral: MSWindowsModifierLedger.h is a pure header (no Win32
# deps) -- the injected-modifier ledger hygiene and the stale-modifier audit
# grace are decided there and tested here on every platform.
# Pure display-wait schedule from DisplayWaitPolicy.h (no CoreGraphics).
create_test(
  NAME DisplayWaitPolicyTests
  DEPENDS platform
  LIBS base arch
  SOURCE DisplayWaitPolicyTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/platform"
)

create_test(
  NAME ModifierLedgerTests
  DEPENDS platform
  LIBS base arch
  SOURCE ModifierLedgerTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/platform"
)

if (WIN32)
  create_test(
    NAME MSWindowsClipboardTests
    DEPENDS platform
    LIBS base arch
    SOURCE MSWindowsClipboardTests.cpp
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/platform"
  )
  # Pure restart-backoff policy from MSWindowsWatchdog.h (no process spawning).
  create_test(
    NAME WatchdogPolicyTests
    DEPENDS platform
    LIBS base arch
    SOURCE WatchdogPolicyTests.cpp
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/platform"
  )
  # Pure decision logic only (static functions); no live desks/key state.
  create_test(
    NAME MSWindowsKeyStateTests
    DEPENDS platform
    LIBS base arch
    SOURCE MSWindowsKeyStateTests.cpp
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/platform"
  )
elseif(APPLE)
  create_test(
    NAME OSXClipboardTests
    DEPENDS platform
    LIBS base arch
    SOURCE OSXClipboardTests.cpp
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/platform"
  )
  create_test(
    NAME OSXScreenTests
    DEPENDS platform
    LIBS base arch
    SOURCE OSXScreenTests.cpp
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/platform"
  )
  create_test(
    NAME OSXKeyStateTests
    DEPENDS platform
    LIBS base arch
    SOURCE OSXKeyStateTests.cpp
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/platform"
  )
elseif(UNIX)
  if (BUILD_X11_SUPPORT)
    create_test(
      NAME XWindowsClipboardTests
      DEPENDS platform
      LIBS base arch
      SOURCE XWindowsClipboardTests.cpp
      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/platform"
    )
  endif()
endif()
