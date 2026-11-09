add_executable(LogSmokeTest log_smoke_test.cc)
target_link_libraries(LogSmokeTest PRIVATE sp::core sp::warnings)
add_test(NAME log_smoke COMMAND LogSmokeTest)

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
