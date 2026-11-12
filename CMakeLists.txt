cmake_minimum_required(VERSION 3.15)
project(weivel_core C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

if(NOT WIN32)
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
endif()

function(weivel_strict_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

set(WEIVEL_CORE_SOURCES
    src/core.c
    src/deadlines.c
    src/events.c
    src/handles.c
    src/mutex.c
    src/sized.c)

function(weivel_core_target target)
    target_include_directories(${target} PUBLIC include)
    weivel_strict_warnings(${target})
    if(NOT WIN32)
        target_link_libraries(${target} PUBLIC Threads::Threads)
    endif()
endfunction()

add_library(weivel_core STATIC ${WEIVEL_CORE_SOURCES})
weivel_core_target(weivel_core)
set_target_properties(weivel_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(weivel_core_shared SHARED ${WEIVEL_CORE_SOURCES})
weivel_core_target(weivel_core_shared)
target_compile_definitions(weivel_core_shared PRIVATE WEIVEL_BUILDING_SHARED INTERFACE WEIVEL_USING_SHARED)
set_target_properties(weivel_core_shared PROPERTIES
    OUTPUT_NAME weivel_core
    C_VISIBILITY_PRESET hidden
    ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/shared)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    enable_testing()
    add_subdirectory(test)
endif()
