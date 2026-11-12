# -DFORMAT=msvc|gnu|apple -DTOOL=<link.exe or nm> -DLIBRARY=<static library> [-DSHARED=<shared library>]
# Fails on any symbol the core imports that is not on the allow-list below.

cmake_policy(VERSION 3.15)

set(allowed_everywhere
    malloc calloc realloc free
    memcpy memmove memset memcmp memchr
    strlen strnlen strcmp strncmp strchr strstr
    snprintf vsnprintf)

set(allowed_from_msvc_inline_snprintf
    __stdio_common_vsprintf "?_OptionsStorage@?1??__local_stdio_printf_options@@9@9")
set(allowed_from_msvc_debug_runtime_checks _RTC_CheckStackVars _RTC_InitBase _RTC_Shutdown)
set(allowed_from_msvc_buffer_security_check __security_check_cookie __security_cookie __GSHandlerCheck)
set(allowed_from_msvc_stack_probe __chkstk)
set(allowed_from_the_stack_protector __stack_chk_fail __stack_chk_guard)
set(allowed_from_position_independent_code _GLOBAL_OFFSET_TABLE_)
set(allowed_from_fortified_allowed_calls
    __memcpy_chk __memmove_chk __memset_chk __snprintf_chk __vsnprintf_chk __strlen_chk)
set(allowed_from_apple_memset_lowering bzero __bzero)
set(allowed_from_the_toolchain
    ${allowed_from_msvc_inline_snprintf} ${allowed_from_msvc_debug_runtime_checks}
    ${allowed_from_msvc_buffer_security_check} ${allowed_from_msvc_stack_probe} ${allowed_from_the_stack_protector}
    ${allowed_from_position_independent_code} ${allowed_from_fortified_allowed_calls}
    ${allowed_from_apple_memset_lowering})

set(allowed_in_the_mutex_only
    InitializeSRWLock AcquireSRWLockExclusive ReleaseSRWLockExclusive
    pthread_mutex_init pthread_mutex_destroy pthread_mutex_lock pthread_mutex_unlock)

set(allowed_from_glibc_shared_library_startup
    __cxa_finalize _ITM_deregisterTMCloneTable _ITM_registerTMCloneTable __gmon_start__)
set(allowed_from_the_mach_o_lazy_binder dyld_stub_binder)
set(allowed_in_a_shared_library ${allowed_from_glibc_shared_library_startup} ${allowed_from_the_mach_o_lazy_binder})

set(mutex_marker weivel_mutex_lock)

function(run_lines output)
    execute_process(COMMAND ${ARGN} OUTPUT_VARIABLE text ERROR_VARIABLE errors RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${ARGN} failed (${result}): ${errors}")
    endif()
    string(REPLACE ";" " " text "${text}")
    string(REPLACE "[" "<" text "${text}")
    string(REPLACE "]" ">" text "${text}")
    string(REPLACE "\r" "" text "${text}")
    string(REPLACE "\n" ";" lines "${text}")
    set(${output} "${lines}" PARENT_SCOPE)
endfunction()

function(normalised name output)
    if(FORMAT STREQUAL "gnu")
        string(REGEX REPLACE "@[A-Za-z_]+[0-9.]*$" "" name "${name}")
    elseif(FORMAT STREQUAL "msvc")
        string(REGEX REPLACE "^__imp_" "" name "${name}")
    elseif(FORMAT STREQUAL "apple")
        string(REGEX REPLACE "^_" "" name "${name}")
    endif()
    set(${output} "${name}" PARENT_SCOPE)
endfunction()

macro(start_object label)
    math(EXPR object_count "${object_count} + 1")
    set(object_${object_count}_label "${label}")
    set(object_${object_count}_defined "")
    set(object_${object_count}_undefined "")
endmacro()

macro(record kind raw_name)
    normalised("${raw_name}" symbol)
    list(APPEND object_${object_count}_${kind} "${symbol}")
endmacro()

macro(read_msvc_objects)
    run_lines(lines "${TOOL}" /dump /symbols "${LIBRARY}")
    foreach(line IN LISTS lines)
        if(line MATCHES "^COFF SYMBOL TABLE")
            start_object("")
        elseif(line MATCHES "^[0-9A-F]+ [0-9A-F]+ UNDEF .* External +\\| ([^ ]+)")
            record(undefined "${CMAKE_MATCH_1}")
        elseif(line MATCHES "^[0-9A-F]+ [0-9A-F]+ SECT[0-9A-F]+ .* External +\\| ([^ ]+)")
            record(defined "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    foreach(index RANGE 1 ${object_count})
        list(GET object_${index}_defined 0 first_defined)
        set(object_${index}_label "the object defining ${first_defined}")
    endforeach()
endmacro()

macro(read_nm_objects)
    run_lines(lines "${TOOL}" "${LIBRARY}")
    foreach(line IN LISTS lines)
        if(line MATCHES "^([^ ].*):$")
            start_object("${CMAKE_MATCH_1}")
        elseif(object_count GREATER 0 AND line MATCHES "^[0-9a-fA-F ]* ([Uw]) ([^ ]+)$")
            record(undefined "${CMAKE_MATCH_2}")
        elseif(object_count GREATER 0 AND line MATCHES "^[0-9a-fA-F]+ ([A-TV-Z]) ([^ ]+)$")
            record(defined "${CMAKE_MATCH_2}")
        endif()
    endforeach()
endmacro()

set(offenders "")
set(object_count 0)
if(FORMAT STREQUAL "msvc")
    read_msvc_objects()
else()
    read_nm_objects()
endif()
if(object_count EQUAL 0)
    message(FATAL_ERROR "no objects read from ${LIBRARY}, so nothing was checked")
endif()

set(defined_in_the_library "")
foreach(index RANGE 1 ${object_count})
    list(APPEND defined_in_the_library ${object_${index}_defined})
endforeach()

set(mutex_objects 0)
set(imports_read 0)
foreach(index RANGE 1 ${object_count})
    set(allowed ${allowed_everywhere} ${allowed_from_the_toolchain} ${defined_in_the_library})
    if(mutex_marker IN_LIST object_${index}_defined)
        math(EXPR mutex_objects "${mutex_objects} + 1")
        list(APPEND allowed ${allowed_in_the_mutex_only})
    endif()
    foreach(symbol IN LISTS object_${index}_undefined)
        math(EXPR imports_read "${imports_read} + 1")
        if(NOT symbol IN_LIST allowed)
            list(APPEND offenders "${object_${index}_label}: ${symbol}")
        endif()
    endforeach()
endforeach()
if(NOT mutex_objects EQUAL 1 OR imports_read EQUAL 0)
    message(FATAL_ERROR "read ${object_count} objects, ${mutex_objects} defining ${mutex_marker} and ${imports_read} imports: the library was not read")
endif()
message(STATUS "${object_count} objects, ${imports_read} imports read from ${LIBRARY}")

if(DEFINED SHARED)
    if(FORMAT STREQUAL "apple")
        run_lines(lines "${TOOL}" -u "${SHARED}")
    else()
        run_lines(lines "${TOOL}" -D -u "${SHARED}")
    endif()
    set(allowed ${allowed_everywhere} ${allowed_from_the_toolchain} ${allowed_in_the_mutex_only}
        ${allowed_in_a_shared_library})
    set(shared_imports "")
    foreach(line IN LISTS lines)
        if(line MATCHES "([^ ]+)$" AND NOT line MATCHES ":$")
            normalised("${CMAKE_MATCH_1}" symbol)
            list(APPEND shared_imports "${symbol}")
            if(NOT symbol IN_LIST allowed)
                list(APPEND offenders "${SHARED}: ${symbol}")
            endif()
        endif()
    endforeach()
    if(NOT "free" IN_LIST shared_imports)
        message(FATAL_ERROR "no import of free read from ${SHARED}: the shared library was not read")
    endif()
    list(LENGTH shared_imports shared_count)
    message(STATUS "${shared_count} imports read from ${SHARED}")
endif()

if(offenders)
    string(REPLACE ";" "\n  " listed "${offenders}")
    message(FATAL_ERROR "the core imports what it may not:\n  ${listed}")
endif()
