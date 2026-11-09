cmake_minimum_required(VERSION 3.16)
project(StreamParser VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SP_BUILD_EXAMPLES "Build the example programs" ON)
option(SP_BUILD_TESTS "Build the tests" ON)

# sp/version.hpp is generated, so the release number is written down once.
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated/sp)
configure_file(cmake/version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/sp/version.hpp @ONLY)

# ---------------------------------------------------------------------------
# Dependencies
#
# ASIO is vendored (3rdparty/asio) and header only. It is an INTERFACE target
# because the transport headers expose asio types, so whoever includes them
# needs the same include path and the same defines. The installed package
# carries the headers (include/sp/asio) instead of asking the system for asio:
# a downstream then needs nothing but this package.
# ---------------------------------------------------------------------------
add_library(sp_asio INTERFACE)
add_library(sp::asio ALIAS sp_asio)
set_target_properties(sp_asio PROPERTIES EXPORT_NAME asio)
target_include_directories(sp_asio INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/asio/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/sp/asio>)
target_compile_definitions(sp_asio INTERFACE ASIO_STANDALONE)
if(WIN32)
    target_compile_definitions(sp_asio INTERFACE _WIN32_WINNT=0x0601)
endif()

find_package(Threads REQUIRED)
target_link_libraries(sp_asio INTERFACE Threads::Threads)

# Warnings and platform defines for first party code only. The libraries apply
# them to their own sources (a PRIVATE link would put the flags into the
# exported package); the executables get the same thing as an INTERFACE target,
# because they are never installed.
function(sp_target_defaults target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
        target_compile_definitions(${target} PRIVATE
            WIN32_LEAN_AND_MEAN NOMINMAX
            _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
        target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
endfunction()

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
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(sp_core PUBLIC cxx_std_14)
sp_target_defaults(sp_core)

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
target_link_libraries(sp_transport PUBLIC sp::core sp::asio)
sp_target_defaults(sp_transport)

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

# ---------------------------------------------------------------------------
# Install: headers, the two libraries, the vendored ASIO headers, and a
# package config so that find_package(sp 1.0 CONFIG REQUIRED) works.
# ---------------------------------------------------------------------------
install(TARGETS sp_core sp_transport sp_asio
    EXPORT spTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/sp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/generated/sp/version.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/sp)
install(DIRECTORY 3rdparty/asio/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/sp/asio
    FILES_MATCHING PATTERN "*.hpp")

install(FILES LICENSE 3rdparty/asio/LICENSE_1_0.txt
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/sp)

install(EXPORT spTargets
    NAMESPACE sp::
    FILE spTargets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sp)

configure_package_config_file(cmake/sp-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/spConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sp)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/spConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/spConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/spConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sp)

message(STATUS "StreamParser ${PROJECT_VERSION} for ${CMAKE_SYSTEM_NAME}")
message(STATUS "C++ compiler: ${CMAKE_CXX_COMPILER}")
