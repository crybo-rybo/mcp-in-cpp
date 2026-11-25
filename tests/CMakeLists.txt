include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG 317ac1ed4c0bb6e6b91eafc817e05c488feffcb3
  SYSTEM
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(Catch2)
foreach(MCP_CATCH_TARGET IN ITEMS Catch2 Catch2WithMain)
  target_compile_features("${MCP_CATCH_TARGET}" PRIVATE cxx_std_23)
endforeach()
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

add_executable(
  mcp_tests
  ask_tests.cpp
  client_ask_tests.cpp
  client_feature_tests.cpp
  client_stdio_tests.cpp
  client_tests.cpp
  crypto_tests.cpp
  envelope_tests.cpp
  feature_tests.cpp
  http_client_tests.cpp
  http_socket_tests.cpp
  http_tests.cpp
  legacy_tests.cpp
  logging_tests.cpp
  loopback_ask_tests.cpp
  loopback_feature_tests.cpp
  loopback_tests.cpp
  modern_tests.cpp
  routing_tests.cpp
  stdio_tests.cpp
  tool_call_tests.cpp
  verbatim_tests.cpp
)
target_link_libraries(
  mcp_tests
  PRIVATE mcp::mcp mcp_project_options Catch2::Catch2WithMain httplib::httplib
)
# The tests of the library's own parts, such as its SHA-256, read its sources.
target_include_directories(mcp_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
catch_discover_tests(mcp_tests TEST_PREFIX "mcp.")

# The programs the tests of the stdio client start, named in a header of their
# own, and empty when the examples are not built.
set(MCP_WEATHER_EXAMPLE "")
if(TARGET mcp_weather_example)
  set(MCP_WEATHER_EXAMPLE "$<TARGET_FILE:mcp_weather_example>")
  add_dependencies(mcp_tests mcp_weather_example)
endif()
file(
  GENERATE
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/programs.hpp"
  CONTENT
    "// The programs the tests start, as CMake builds them, or empty.
#pragma once

#include <string_view>

namespace programs {

inline constexpr std::string_view weather{\"${MCP_WEATHER_EXAMPLE}\"};

} // namespace programs
"
)
target_include_directories(mcp_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# The example client, calling a tool of the example server.
if(TARGET mcp_client_example AND TARGET mcp_weather_example)
  add_test(
    NAME mcp.example.client
    COMMAND
      mcp_client_example forecast [[{"city":"Oslo"}]]
      "$<TARGET_FILE:mcp_weather_example>"
  )
  set_tests_properties(
    mcp.example.client
    PROPERTIES PASS_REGULAR_EXPRESSION [[\{"sky":"clear","temperature":20\.5\}]]
  )
endif()

# Drives a server written with the official Python SDK, which
# scripts/interop.sh starts.
add_executable(mcp_interop_client interop/client.cpp)
target_link_libraries(mcp_interop_client PRIVATE mcp::mcp mcp_project_options)

# The server the official conformance suite tests, over HTTP.
add_executable(mcp_conformance_fixture conformance/fixture.cpp)
target_link_libraries(
  mcp_conformance_fixture
  PRIVATE mcp::mcp httplib::httplib mcp_project_options
)

# The client the official conformance suite tests, over HTTP.
add_executable(mcp_conformance_client conformance/client.cpp)
target_link_libraries(
  mcp_conformance_client
  PRIVATE mcp::mcp httplib::httplib mcp_project_options
)
