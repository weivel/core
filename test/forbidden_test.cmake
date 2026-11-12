# -DSOURCE_ROOT=<repo>: fails when src/ or include/ names a call that reads a file, the environment or a clock.

set(identifier_edge "(^|[^A-Za-z0-9_])")
set(identifier_end "([^A-Za-z0-9_]|$)")
set(call "[ \t\r\n]*\\(")
set(source_patterns
    "${identifier_edge}fopen(64|_s)?${identifier_end}"
    "${identifier_edge}freopen${identifier_end}"
    "${identifier_edge}open${call}"
    "${identifier_edge}CreateFile"
    "${identifier_edge}(secure_)?getenv${identifier_end}"
    "${identifier_edge}GetEnvironmentVariable"
    "${identifier_edge}time${call}"
    "${identifier_edge}clock${call}"
    "${identifier_edge}clock_gettime${identifier_end}"
    "${identifier_edge}timespec_get${identifier_end}"
    "${identifier_edge}gettimeofday${identifier_end}"
    "${identifier_edge}mach_absolute_time${identifier_end}"
    "${identifier_edge}QueryPerformanceCounter"
    "${identifier_edge}GetTickCount"
    "${identifier_edge}GetSystemTime")

file(GLOB_RECURSE files "${SOURCE_ROOT}/src/*" "${SOURCE_ROOT}/include/*")
list(LENGTH files count)
if(count EQUAL 0)
    message(FATAL_ERROR "no sources found under ${SOURCE_ROOT}")
endif()

set(offenders "")
foreach(file IN LISTS files)
    file(READ "${file}" content)
    foreach(pattern IN LISTS source_patterns)
        string(REGEX MATCH "${pattern}" hit "${content}")
        if(hit)
            list(APPEND offenders "${file}: ${hit}")
        endif()
    endforeach()
    string(TOLOWER "${content}" lowered)
    string(FIND "${lowered}" "json" position)
    if(NOT position EQUAL -1)
        list(APPEND offenders "${file}: json")
    endif()
endforeach()

if(offenders)
    string(REPLACE ";" "\n  " listed "${offenders}")
    message(FATAL_ERROR "forbidden calls found:\n  ${listed}")
endif()
message(STATUS "${count} sources read")
