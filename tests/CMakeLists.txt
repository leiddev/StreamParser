# Unit tests. Every file is self contained (a check() function and a main()),
# so there is no test framework to fetch and ctest just runs the binaries.

add_executable(StreamParserTest stream_parser_test.cc)
target_link_libraries(StreamParserTest PRIVATE sp::core sp::warnings)
add_test(NAME stream_parser COMMAND StreamParserTest)

# The two example parsers are headers under examples/, so the tests read them
# from there: they are part of the repository but not of the installed package.
add_executable(A55A55AAParserTest a55a55aa_parser_test.cc)
target_include_directories(A55A55AAParserTest PRIVATE ${PROJECT_SOURCE_DIR}/examples)
target_link_libraries(A55A55AAParserTest PRIVATE sp::core sp::warnings)
add_test(NAME a55a55aa_parser COMMAND A55A55AAParserTest)

add_executable(StartEndParserTest start_end_parser_test.cc)
target_include_directories(StartEndParserTest PRIVATE ${PROJECT_SOURCE_DIR}/examples)
target_link_libraries(StartEndParserTest PRIVATE sp::core sp::warnings)
add_test(NAME start_end_parser COMMAND StartEndParserTest)

add_executable(LogSmokeTest log_smoke_test.cc)
target_link_libraries(LogSmokeTest PRIVATE sp::core sp::warnings)
add_test(NAME log_smoke COMMAND LogSmokeTest)

# The facade tests drive sp::transport in-process: the transport under test on
# one side, a bare asio socket standing in for the peer on the other. They need
# no example program and no python, so they run wherever the library builds.
add_executable(TransportFacadeTest transport_facade_test.cc)
target_link_libraries(TransportFacadeTest PRIVATE sp::transport sp::warnings)
add_test(NAME transport_facade COMMAND TransportFacadeTest)
set_tests_properties(transport_facade PROPERTIES TIMEOUT 180)

# Serial needs a real device pair (or a pty pair) on the machine running the
# test. The missing-device paths run everywhere; the round-trip cases are
# enabled by setting both ports, e.g. -E SP_TEST_SERIAL_PORT=COM1
# -E SP_TEST_SERIAL_PORT_PEER=COM2.
add_executable(TransportSerialTest transport_serial_test.cc)
target_link_libraries(TransportSerialTest PRIVATE sp::transport sp::warnings)
add_test(NAME transport_serial COMMAND TransportSerialTest)
set_tests_properties(transport_serial PROPERTIES TIMEOUT 180)

# End-to-end tests start an example program, talk to it over a real socket and
# check the frame callback fires. They need the examples and a python 3
# interpreter, and are silently skipped when either is missing.
if(SP_BUILD_EXAMPLES)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.12)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        set(_sp_python "${Python3_EXECUTABLE}")
    else()
        find_program(_sp_python NAMES python3 python)
    endif()

    if(_sp_python)
        add_test(NAME tcp_e2e
                 COMMAND ${_sp_python} ${CMAKE_CURRENT_SOURCE_DIR}/e2e_tcp_test.py
                         $<TARGET_FILE:TcpServerDemo>)
        set_tests_properties(tcp_e2e PROPERTIES TIMEOUT 90)

        add_test(NAME transport_e2e
                 COMMAND ${_sp_python} ${CMAKE_CURRENT_SOURCE_DIR}/e2e_transport_test.py
                         $<TARGET_FILE:TransportDemo>)
        set_tests_properties(transport_e2e PROPERTIES TIMEOUT 120)
    else()
        message(STATUS "python3 not found, tcp_e2e and transport_e2e disabled")
    endif()
endif()
