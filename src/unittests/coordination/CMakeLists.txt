# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT

create_test(
  NAME ElectionStateTests
  DEPENDS coordination
  SOURCE ElectionStateTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/coordination"
)

create_test(
  NAME CoordinationProtocolTests
  DEPENDS coordination
  SOURCE CoordinationProtocolTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/coordination"
)

create_test(
  NAME FleetStateMergeTests
  DEPENDS coordination
  SOURCE FleetStateMergeTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/coordination"
)

create_test(
  NAME CoordinatorFleetPublishTests
  DEPENDS coordination
  SOURCE CoordinatorFleetPublishTests.cpp
  LIBS base arch mt
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/coordination"
)

create_test(
  NAME PreConnectHostsTests
  DEPENDS coordination
  SOURCE PreConnectHostsTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/coordination"
)

create_test(
  NAME WakeOnLanTests
  DEPENDS coordination
  SOURCE WakeOnLanTests.cpp
  LIBS base arch mt
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/coordination"
)

create_test(
  NAME KeyboardRouterTests
  DEPENDS coordination
  SOURCE KeyboardRouterTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/coordination"
)

create_test(
  NAME KeyboardRescueTests
  DEPENDS coordination
  SOURCE KeyboardRescueTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/coordination"
)

create_test(
  NAME KeyboardRelayHookPolicyTests
  DEPENDS coordination
  SOURCE KeyboardRelayHookPolicyTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/coordination"
)

create_test(
  NAME CoordinationLocalStatusTests
  DEPENDS common coordination
  SOURCE CoordinationLocalStatusTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/coordination"
)

if(APPLE)
  create_test(
    NAME KeyboardRelayMapTests
    DEPENDS coordination
    SOURCE KeyboardRelayMapTests.cpp
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/coordination"
  )
endif()

if(WIN32)
  create_test(
    NAME KeyboardRelayMapWindowsTests
    DEPENDS coordination
    SOURCE KeyboardRelayMapWindowsTests.cpp
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/coordination"
  )
endif()
