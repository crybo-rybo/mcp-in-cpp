# A complete server: a few annotated functions, served over stdio, or over
# Streamable HTTP through cpp-httplib, which only the example uses.
include(FetchContent)
FetchContent_Declare(
  httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.65.0
  GIT_SHALLOW TRUE
  SYSTEM
  EXCLUDE_FROM_ALL
)
set(HTTPLIB_REQUIRE_OPENSSL OFF)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF)
FetchContent_MakeAvailable(httplib)

add_executable(mcp_weather_example weather.cpp)
target_link_libraries(
  mcp_weather_example
  PRIVATE mcp::mcp httplib::httplib mcp_project_options
)
