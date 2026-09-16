# SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
# SPDX-License-Identifier: MIT

## Use To create tests
function(create_test)
  set(options)
  set(oneValueArgs
    NAME #NAME of new test
    WORKING_DIRECTORY #Working Dir
    DEPENDS #Library being tested
    SOURCE #Single Source File
  )
  set(multiValueArgs
    LIBS #Any Additional libs that are not Qt::Test or the DEPENDS lib
  )
  cmake_parse_arguments(m "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

  if("${m_NAME}" STREQUAL "")
    message(FATAL_ERROR "create_test, tests require a NAME")
  endif()

  if("${m_WORKING_DIRECTORY}" STREQUAL "")
    message(FATAL_ERROR "create_test, tests require a WORKING_DIRECTORY")
  endif()

  if("${m_SOURCE}" STREQUAL "")
    message(FATAL_ERROR "create_test, tests require a SOURCE")
  endif()

  if("${m_DEPENDS}" STREQUAL "")
    message(FATAL_ERROR "create_test, tests require a DEPENDS")
  endif()

  add_executable(${m_NAME} ${m_SOURCE})
  target_link_libraries(${m_NAME} ${m_DEPENDS} ${m_LIBS} Qt::Test)
  if (CMAKE_CXX_BYTE_ORDER STREQUAL "BIG_ENDIAN")
    target_compile_definitions(${m_NAME} PUBLIC WORDS_BIGENDIAN=1)
  endif()
  add_test(NAME ${m_NAME} COMMAND $<TARGET_FILE:${m_NAME}> WORKING_DIRECTORY ${m_WORKING_DIRECTORY})
  set_tests_properties(${m_NAME} PROPERTIES DEPENDS ${m_DEPENDS})
  set_property(GLOBAL APPEND PROPERTY ${CMAKE_PROJECT_NAME}_tests ${m_NAME})

  if(ENABLE_COVERAGE)
    setup_target_for_coverage_gcovr_xml(
      NAME coverage-${m_NAME}
      EXECUTABLE ${m_NAME}
      BASE_DIRECTORY ${CMAKE_SOURCE_DIR}/src
      EXCLUDE subprojects/* ${CMAKE_BINARY_DIR}/* src/unittest/*
    )
  endif()

endfunction()

enable_testing()
find_package(Qt6 ${REQUIRED_QT_VERSION} REQUIRED COMPONENTS Test)

add_subdirectory(base)
add_subdirectory(client)
add_subdirectory(common)
add_subdirectory(coordination)
add_subdirectory(deskflow)
add_subdirectory(gui)
add_subdirectory(legacytests)
add_subdirectory(net)
add_subdirectory(platform)
add_subdirectory(server)

# deskflow-core `--check-permissions` (Qt-free lib defined in src/apps/deskflow-core)
create_test(
  NAME PermissionCheckTests
  DEPENDS ${CMAKE_PROJECT_NAME}-core-permissioncheck
  SOURCE apps/deskflow-core/PermissionCheckTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/apps/deskflow-core"
)

# Hotspot D4-server-clipboard-copies (harness/registry.json) owns this file;
# it lives outside server/ so the registry's file ownership stays disjoint.
if(WIN32)
  set(server_clipboard_extra_libs version app mt net)
endif()
create_test(
  NAME ServerClipboardTests
  DEPENDS server
  LIBS base arch app ${server_clipboard_extra_libs}
  SOURCE lib/server/ServerClipboardTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/server"
)

# Hotspot D3-socket-buffers (harness/registry.json): tests live beside the
# library path they cover, under lib/<lib>/.
if(WIN32)
  set(socket_extra_libs version)
endif()

create_test(
  NAME StreamBufferTests
  DEPENDS io
  SOURCE lib/io/StreamBufferTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/io"
)

create_test(
  NAME TCPSocketTests
  DEPENDS net
  LIBS base arch mt io ${socket_extra_libs}
  SOURCE lib/net/TCPSocketTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/net"
)

# App-level tests for deskflow-core. AutoModeRunner's flip gate is
# header-only, so the test needs only the header path and coordination.
create_test(
  NAME AutoModeRunnerTests
  DEPENDS coordination
  SOURCE apps/deskflow-core/AutoModeRunnerTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/apps/deskflow-core"
)
target_include_directories(AutoModeRunnerTests PRIVATE "${CMAKE_SOURCE_DIR}/src/apps/deskflow-core")

#make sure to use CI only plugin on Unix
if (UNIX AND NOT APPLE)
  set(qPA_Platform "QT_QPA_PLATFORM=minimal")
endif()

#Store all test that should be run post build in the property
get_property(PROJECT_TESTS GLOBAL PROPERTY ${CMAKE_PROJECT_NAME}_tests)

if(APPLE)
  set(guiApp Deskflow)
else()
  set(guiApp deskflow)
endif()

option(SKIP_BUILD_TESTS "Skip build time test" OFF)

if(NOT SKIP_BUILD_TESTS)
  add_custom_target(run_tests ALL DEPENDS ${PROJECT_TESTS} ${guiApp})
  add_custom_command (
    TARGET run_tests
    POST_BUILD
    COMMAND ${qPA_Platform} ${CMAKE_CTEST_COMMAND} --test-dir  "${CMAKE_BINARY_DIR}/src/unittests" --output-on-failure
  )
endif()
