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
  envelope_tests.cpp
  feature_tests.cpp
  http_tests.cpp
  legacy_tests.cpp
  modern_tests.cpp
  stdio_tests.cpp
  tool_call_tests.cpp
)
target_link_libraries(
  mcp_tests
  PRIVATE mcp::mcp mcp_project_options Catch2::Catch2WithMain
)
catch_discover_tests(mcp_tests TEST_PREFIX "mcp.")


# The server the official conformance suite tests, over HTTP.
add_executable(mcp_conformance_fixture conformance/fixture.cpp)
target_link_libraries(
  mcp_conformance_fixture
  PRIVATE mcp::mcp httplib::httplib mcp_project_options
)
