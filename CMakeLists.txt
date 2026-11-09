cmake_minimum_required(VERSION 3.10)
project(StreamParser VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(ExternalProject)
# Include External Project
include(3rdparty/asio.cmake)

add_executable(SerialDemo serial_main.cc)
add_executable(TcpServerDemo tcp_server_main.cc)
add_executable(TcpClientDemo tcp_client_main.cc)
add_executable(UdpDemo udp_main.cc)
# Transport facade demo: no asio in the program itself.
add_executable(TransportDemo transport_main.cc)

# Facade implementation shared by TransportDemo (and by hosts that want it).
set(SP_TRANSPORT_SOURCES
    src/TransportBase.cc
    src/TransportFactory.cc
    src/TransportSerial.cc
    src/TransportTcpClient.cc
    src/TransportTcpServer.cc
    src/TransportUdp.cc
)

if(WIN32)
    add_definitions(-DWIN32_LEAN_AND_MEAN)
    add_definitions(-DNOMINMAX)
else()
    find_package(Threads REQUIRED)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES MSVC)
    add_compile_options(/source-charset:utf-8)
    add_compile_options(/execution-charset:utf-8)
    add_compile_options(-D_CRT_SECURE_NO_WARNINGS)
    add_compile_options(-D_WINSOCK_DEPRECATED_NO_WARNINGS)
endif()

target_sources(SerialDemo PRIVATE
    src/StreamParser.cc
)

target_sources(TcpServerDemo PRIVATE
    src/StreamParser.cc
)

target_sources(TcpClientDemo PRIVATE
    src/StreamParser.cc
)

target_sources(UdpDemo PRIVATE
    src/StreamParser.cc
)

target_include_directories(SerialDemo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_link_libraries(SerialDemo asio)
target_compile_definitions(SerialDemo PRIVATE ASIO_STANDALONE _WIN32_WINNT=0x0601)

target_include_directories(TcpServerDemo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_link_libraries(TcpServerDemo asio)
target_compile_definitions(TcpServerDemo PRIVATE ASIO_STANDALONE _WIN32_WINNT=0x0601)

target_include_directories(TcpClientDemo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_link_libraries(TcpClientDemo asio)
target_compile_definitions(TcpClientDemo PRIVATE ASIO_STANDALONE _WIN32_WINNT=0x0601)

target_include_directories(UdpDemo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_link_libraries(UdpDemo asio)
target_compile_definitions(UdpDemo PRIVATE ASIO_STANDALONE _WIN32_WINNT=0x0601)

target_sources(TransportDemo PRIVATE
    src/StreamParser.cc
    ${SP_TRANSPORT_SOURCES}
)

target_include_directories(TransportDemo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_link_libraries(TransportDemo asio)
target_compile_definitions(TransportDemo PRIVATE ASIO_STANDALONE _WIN32_WINNT=0x0601)


# Dependency free smoke test for the pluggable logging facility (StreamParserLog.h)
add_executable(LogSmokeTest test/log_smoke_test.cc)
target_include_directories(LogSmokeTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)

# ---------------------------------------------------------------------------
# Tests (ctest --test-dir build -C Release --output-on-failure)
# ---------------------------------------------------------------------------
enable_testing()

add_test(NAME log_smoke COMMAND LogSmokeTest)

# End-to-end test: start TcpServerDemo, feed it one A5 5A frame, check the frame
# callback fires. Needs a python 3 interpreter, silently skipped otherwise.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.12)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    set(_sp_python "${Python3_EXECUTABLE}")
else()
    find_program(_sp_python NAMES python3 python)
endif()

if(_sp_python)
    add_test(NAME tcp_e2e
             COMMAND ${_sp_python} ${CMAKE_CURRENT_SOURCE_DIR}/test/e2e_tcp_test.py
                     $<TARGET_FILE:TcpServerDemo>)
    set_tests_properties(tcp_e2e PROPERTIES TIMEOUT 90)

    # Same idea for the Transport facade: link up, frame callback, echo back.
    add_test(NAME transport_e2e
             COMMAND ${_sp_python} ${CMAKE_CURRENT_SOURCE_DIR}/test/e2e_transport_test.py
                     $<TARGET_FILE:TransportDemo>)
    set_tests_properties(transport_e2e PROPERTIES TIMEOUT 120)
else()
    message(STATUS "python3 not found, tcp_e2e test disabled")
    message(STATUS "python3 not found, transport_e2e test disabled")
endif()

message(STATUS "Build completed for ${CMAKE_SYSTEM_NAME}")
message(STATUS "C compiler: ${CMAKE_C_COMPILER}")
message(STATUS "C++ compiler: ${CMAKE_CXX_COMPILER}")
