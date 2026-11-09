cmake_minimum_required(VERSION 3.16)
project(StreamParser VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SP_BUILD_EXAMPLES "Build the example programs" ON)
option(SP_BUILD_TESTS "Build the tests" ON)

# ---------------------------------------------------------------------------
# Dependencies
#
# ASIO is vendored (3rdparty/asio) and header only. It is an INTERFACE target
# because the transport headers expose asio types, so whoever includes them
# needs the same include path and the same two defines.
# ---------------------------------------------------------------------------
add_library(sp_asio INTERFACE)
add_library(sp::asio ALIAS sp_asio)
target_include_directories(sp_asio INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/asio/include>)
target_compile_definitions(sp_asio INTERFACE ASIO_STANDALONE)
if(WIN32)
    target_compile_definitions(sp_asio INTERFACE _WIN32_WINNT=0x0601)
endif()

find_package(Threads REQUIRED)
target_link_libraries(sp_asio INTERFACE Threads::Threads)

# Warnings and platform defines for first party code only.
add_library(sp_warnings INTERFACE)
add_library(sp::warnings ALIAS sp_warnings)
if(MSVC)
    target_compile_options(sp_warnings INTERFACE /W4 /utf-8)
    target_compile_definitions(sp_warnings INTERFACE
        WIN32_LEAN_AND_MEAN NOMINMAX
        _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
else()
    target_compile_options(sp_warnings INTERFACE -Wall -Wextra)
    target_compile_definitions(sp_warnings INTERFACE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# ---------------------------------------------------------------------------
# sp::core -- the parsing core, no third party dependency.
#
# StreamParser is the extension point: a downstream class derives from it,
# implements processByte()/buildFrame() for its own protocol, and hands an
# instance to a transport through TransportConfig::parser_factory.
# ---------------------------------------------------------------------------
add_library(sp_core STATIC src/StreamParser.cc)
add_library(sp::core ALIAS sp_core)
set_target_properties(sp_core PROPERTIES EXPORT_NAME core)
target_include_directories(sp_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(sp_core PUBLIC cxx_std_14)
target_link_libraries(sp_core PRIVATE sp::warnings)

# ---------------------------------------------------------------------------
# sp::transport -- the ready made transports, on top of core and ASIO.
# ---------------------------------------------------------------------------
add_library(sp_transport STATIC
    src/TransportBase.cc
    src/TransportFactory.cc
    src/TransportSerial.cc
    src/TransportTcpClient.cc
    src/TransportTcpServer.cc
    src/TransportUdp.cc)
add_library(sp::transport ALIAS sp_transport)
set_target_properties(sp_transport PROPERTIES EXPORT_NAME transport)
target_link_libraries(sp_transport PUBLIC sp::core sp::asio PRIVATE sp::warnings)

# ---------------------------------------------------------------------------
# Examples (built and tested, never installed) and tests.
# ---------------------------------------------------------------------------
if(SP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(SP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "StreamParser ${PROJECT_VERSION} for ${CMAKE_SYSTEM_NAME}")
message(STATUS "C++ compiler: ${CMAKE_CXX_COMPILER}")
