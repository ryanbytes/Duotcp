cmake_minimum_required(VERSION 3.20)
project(DuoTCP VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()
add_executable(protocol_tests tests/protocol_tests.cpp)
target_include_directories(protocol_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME protocol_tests COMMAND protocol_tests)

if(WIN32)
    set(SDRPLAY_ROOT "C:/Program Files/SDRplay/API" CACHE PATH "SDRplay API installation root")
    find_path(SDRPLAY_INCLUDE_DIR sdrplay_api.h
        HINTS "${SDRPLAY_ROOT}/inc" "$ENV{ProgramFiles}/SDRplay/API/inc")
    if(NOT SDRPLAY_INCLUDE_DIR)
        message(FATAL_ERROR "sdrplay_api.h not found. Install SDRplay API 3.15 or set SDRPLAY_ROOT.")
    endif()

    add_executable(duotcp src/duotcp.cpp)
    target_include_directories(duotcp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${SDRPLAY_INCLUDE_DIR})
    target_compile_definitions(duotcp PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(duotcp PRIVATE ws2_32 advapi32)
    if(MSVC)
        target_compile_options(duotcp PRIVATE /W4 /permissive-)
    endif()
endif()
