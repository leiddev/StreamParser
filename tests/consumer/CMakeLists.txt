cmake_minimum_required(VERSION 3.16)
project(sp_consumer LANGUAGES CXX)

# The installed package is built Release; a Debug consumer would mix MSVCRT and
# MSVCRTD. Single config generators default to Release here for that reason.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Nothing but the installed package: no add_subdirectory, no vendored tree.
find_package(sp 1.0 CONFIG REQUIRED)

add_executable(consumer main.cc)
target_link_libraries(consumer PRIVATE sp::transport)
