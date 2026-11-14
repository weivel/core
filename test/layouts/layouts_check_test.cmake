# -DSCENARIO=<name> -DWORK=<dir> -DLAYOUTS=<layouts executable> -DHEADERS=<directory of public headers>
# -DCHECK_SCRIPT=<layouts_check.cmake>
#
# Each scenario copies the public headers and what the layouts executable prints into WORK, plants one struct in a
# header of its own beside the copies and the lines it prints beside the real ones, and runs the check against the
# copy, so no planted struct ever reaches a header a consumer reads.

cmake_minimum_required(VERSION 3.15)

set(root "${WORK}/${SCENARIO}")
file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${root}/include")
file(COPY "${HEADERS}/" DESTINATION "${root}/include")

execute_process(COMMAND ${LAYOUTS} RESULT_VARIABLE code OUTPUT_VARIABLE printed ERROR_VARIABLE errors)
if(NOT code STREQUAL "0")
    message(FATAL_ERROR "${LAYOUTS} exited ${code}\n${errors}")
endif()
string(REPLACE "\r" "" printed "${printed}")

function(plant header lines)
    file(WRITE "${root}/include/weivel/probe.h" "${header}")
    file(WRITE "${root}/printed.txt" "${printed}${lines}")
endfunction()

function(expect expected_exit must_match)
    execute_process(COMMAND ${CMAKE_COMMAND} -DPRINTED=${root}/printed.txt -DHEADERS=${root}/include
                        -P ${CHECK_SCRIPT}
                    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    string(REGEX REPLACE "[ \t\r\n]+" " " output "${out}${err}")
    if(expected_exit STREQUAL "0" AND NOT code STREQUAL "0")
        message(FATAL_ERROR "${SCENARIO}: exit ${code}, expected 0\n${output}")
    endif()
    if(NOT expected_exit STREQUAL "0" AND code STREQUAL "0")
        message(FATAL_ERROR "${SCENARIO}: exit 0, expected a refusal\n${output}")
    endif()
    if(NOT output MATCHES "${must_match}")
        message(FATAL_ERROR "${SCENARIO}: output does not match '${must_match}'\n${output}")
    endif()
endfunction()

set(two_fields "typedef struct weivel_probe {\n    uint32_t size;\n    uint32_t count;\n} weivel_probe;\n")
set(probe_size_only "weivel_probe size 0 4\nweivel_probe sizeof 4\n")

if(SCENARIO STREQUAL "layouts_check_reads_every_shape_of_field")
    plant("typedef struct weivel_probe {\n    uint32_t size; /* a comment; with { a brace */\n    char name[8];\n    void (*call)(void *context, int value);\n    const char *first, *second;\n    union {\n        int whole;\n        float part;\n    } data;\n} weivel_probe;\n"
        "weivel_probe size 0 4\nweivel_probe name 4 8\nweivel_probe call 16 8\nweivel_probe first 24 8\nweivel_probe second 32 8\nweivel_probe data 40 4\nweivel_probe data.whole 40 4\nweivel_probe data.part 40 4\nweivel_probe sizeof 48\n")
    expect(0 "public structs, each with a layout")
elseif(SCENARIO STREQUAL "layouts_check_refuses_a_struct_without_a_layout")
    plant("typedef struct weivel_probe {\n    uint32_t size;\n} weivel_probe;\n" "")
    expect(1 "weivel_probe is declared under .* and has no layout")
elseif(SCENARIO STREQUAL "layouts_check_refuses_a_layout_no_header_declares")
    plant("" "${probe_size_only}")
    expect(1 "weivel_probe has a layout and is declared by no public header")
elseif(SCENARIO STREQUAL "layouts_check_refuses_a_struct_defined_after_its_forward_typedef")
    plant("typedef struct weivel_probe weivel_probe;\n\nstruct weivel_probe {\n    uint32_t size;\n};\n" "")
    expect(1 "weivel_probe is declared under .* and has no layout")
elseif(SCENARIO STREQUAL "layouts_check_refuses_an_anonymous_typedef_struct")
    plant("typedef struct {\n    uint32_t size;\n} weivel_probe;\n" "")
    expect(1 "weivel_probe is declared under .* and has no layout")
elseif(SCENARIO STREQUAL "layouts_check_refuses_a_struct_with_a_comment_before_its_brace")
    plant("typedef struct weivel_probe /* the probe */ {\n    uint32_t size;\n} weivel_probe;\n" "")
    expect(1 "weivel_probe is declared under .* and has no layout")
elseif(SCENARIO STREQUAL "layouts_check_refuses_a_forgotten_field")
    plant("${two_fields}" "${probe_size_only}")
    expect(1 "weivel_probe prints the fields size but its header declares size count")
elseif(SCENARIO STREQUAL "layouts_check_refuses_fields_out_of_order")
    plant("${two_fields}" "weivel_probe count 4 4\nweivel_probe size 0 4\nweivel_probe sizeof 8\n")
    expect(1 "weivel_probe prints the fields count size but its header declares size count")
elseif(SCENARIO STREQUAL "layouts_check_refuses_fields_without_a_sizeof_line")
    plant("${two_fields}" "weivel_probe size 0 4\nweivel_probe count 4 4\n")
    expect(1 "weivel_probe: fields but no sizeof line")
elseif(SCENARIO STREQUAL "layouts_check_refuses_a_malformed_line")
    plant("${two_fields}" "weivel_probe size 0\n")
    expect(1 "malformed line 'weivel_probe size 0'")
else()
    message(FATAL_ERROR "unknown SCENARIO '${SCENARIO}'")
endif()
