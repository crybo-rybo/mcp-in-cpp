# mcp-in-cpp: an MCP server library for C++26, in which static reflection turns
# annotated functions into tools. Scrydantic supplies the schemas, the strict
# decoding, and the binding; this library supplies the protocol.
cmake_minimum_required(VERSION 3.30)

project(
  mcp-in-cpp
  VERSION 0.1.0
  DESCRIPTION "A reflection-first MCP server library for C++26"
  LANGUAGES CXX
)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MCP_BUILD_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})
option(MCP_BUILD_EXAMPLES "Build the examples" ${PROJECT_IS_TOP_LEVEL})
option(MCP_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ${PROJECT_IS_TOP_LEVEL})
option(MCP_INSTALL "Generate the install rules" ON)
set(MCP_SANITIZER "none" CACHE STRING "Sanitizer mode: none, address-undefined, or thread")
set_property(CACHE MCP_SANITIZER PROPERTY STRINGS none address-undefined thread)

# C++26 reflection (P2996), which only GCC 16 compiles. Scrydantic states the
# same floor.
if(
  NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
  OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16.2
)
  message(
    FATAL_ERROR
    "mcp-in-cpp requires GCC 16.2 or newer (C++26 reflection), but the C++ "
    "compiler is ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} "
    "(${CMAKE_CXX_COMPILER}). Configure a fresh build directory with CXX set "
    "to GCC 16.2 or newer."
  )
endif()

# Scrydantic 0.4: an installed one if find_package() finds it, and otherwise
# its 0.4.0 release, fetched and built here and installed beside this library.
# Set FETCHCONTENT_SOURCE_DIR_SCRYDANTIC to build against a checkout instead.
include(FetchContent)
FetchContent_Declare(
  scrydantic
  GIT_REPOSITORY https://github.com/crybo-rybo/scrydantic.git
  GIT_TAG v0.4.0
  GIT_SHALLOW TRUE
  FIND_PACKAGE_ARGS 0.4
)
set(SCRYDANTIC_INSTALL ${MCP_INSTALL})
FetchContent_MakeAvailable(scrydantic)

# The flags of this project's own targets. No consumer sees them.
add_library(mcp_project_options INTERFACE)
target_compile_options(
  mcp_project_options
  INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wdouble-promotion
    -Wshadow
    -Wold-style-cast
    -Wcast-qual
    -Wextra-semi
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wnull-dereference
    -Wformat=2
    -Wimplicit-fallthrough
    -Wduplicated-cond
    -Wlogical-op
    -Wundef
)
if(MCP_WARNINGS_AS_ERRORS)
  target_compile_options(mcp_project_options INTERFACE -Werror)
endif()
target_compile_definitions(
  mcp_project_options
  INTERFACE "$<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>"
)
if(MCP_SANITIZER STREQUAL "address-undefined")
  set(MCP_SANITIZER_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=undefined)
elseif(MCP_SANITIZER STREQUAL "thread")
  set(MCP_SANITIZER_FLAGS -fsanitize=thread)
elseif(NOT MCP_SANITIZER STREQUAL "none")
  message(FATAL_ERROR "Unsupported MCP_SANITIZER: ${MCP_SANITIZER}")
endif()
if(MCP_SANITIZER_FLAGS)
  target_compile_options(
    mcp_project_options
    INTERFACE ${MCP_SANITIZER_FLAGS} -fno-omit-frame-pointer
  )
  target_link_options(mcp_project_options INTERFACE ${MCP_SANITIZER_FLAGS})
endif()

# The protocol is compiled once, here. A server's own source includes the
# headers, where only the templates that bind its tools are.
add_library(
  mcp
  STATIC
  src/ask.cpp
  src/binding.cpp
  src/client.cpp
  src/content.cpp
  src/dispatcher.cpp
  src/http.cpp
  src/loopback.cpp
  src/server.cpp
  src/sha256.cpp
  src/stdio.cpp
)
add_library(mcp::mcp ALIAS mcp)
file(GLOB MCP_PUBLIC_HEADERS CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/include/mcp/*.hpp")
target_sources(
  mcp
  PUBLIC
    FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/include"
    FILES ${MCP_PUBLIC_HEADERS}
)
target_include_directories(mcp PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(
  mcp
  PUBLIC scrydantic::scrydantic
  PRIVATE "$<BUILD_INTERFACE:mcp_project_options>"
)
set_target_properties(
  mcp
  PROPERTIES POSITION_INDEPENDENT_CODE ON EXPORT_NAME mcp VERSION ${PROJECT_VERSION}
)

# A relocatable package: find_package(mcp-in-cpp) gives mcp::mcp, and finds
# Scrydantic for it. <mcp/httplib.hpp> is installed too; it needs cpp-httplib
# only in a program that includes it.
if(MCP_INSTALL)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)
  set(MCP_PACKAGE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/mcp-in-cpp")
  install(
    TARGETS mcp
    EXPORT mcpTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET HEADERS DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  )
  install(
    EXPORT mcpTargets
    FILE mcp-in-cppTargets.cmake
    NAMESPACE mcp::
    DESTINATION "${MCP_PACKAGE_DIR}"
  )
  configure_package_config_file(
    cmake/mcp-in-cppConfig.cmake.in
    "${PROJECT_BINARY_DIR}/mcp-in-cppConfig.cmake"
    INSTALL_DESTINATION "${MCP_PACKAGE_DIR}"
  )
  # Before 1.0, a minor release may break the API.
  write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/mcp-in-cppConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMinorVersion
  )
  install(
    FILES
      "${PROJECT_BINARY_DIR}/mcp-in-cppConfig.cmake"
      "${PROJECT_BINARY_DIR}/mcp-in-cppConfigVersion.cmake"
    DESTINATION "${MCP_PACKAGE_DIR}"
  )
endif()

# cpp-httplib serves the example and the conformance fixture over HTTP. The
# library does not use it.
if(MCP_BUILD_EXAMPLES OR MCP_BUILD_TESTS)
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
endif()

if(MCP_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(MCP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
