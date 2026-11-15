function(weivel_test name library)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE ${library})
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    weivel_strict_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 60)
endfunction()

weivel_test(instance_test weivel_core)
weivel_test(handles_test weivel_core)
weivel_test(concurrency_test weivel_core)
weivel_test(sized_test weivel_core)
weivel_test(abi_test weivel_core_shared)
weivel_test(manifest_test weivel_core)
weivel_test(compatibility_test weivel_core_shared)
weivel_test(activation_test weivel_core_shared)
weivel_test(registry_test weivel_core_shared)
weivel_test(deadline_test weivel_core_shared)
weivel_test(subject_test weivel_core_shared)
weivel_test(handle_state_test weivel_core_shared)
weivel_test(reach_test weivel_core_testing)
weivel_test(lifecycle_test weivel_core_testing)
weivel_test(constraints_test weivel_core_testing)
weivel_test(capabilities_test weivel_core_testing)
weivel_test(ledger_test weivel_core_testing)
weivel_test(threads_test weivel_core_testing)
weivel_test(oom_test weivel_core_testing)
weivel_test(events_test weivel_core_testing)
weivel_test(releases_test weivel_core_testing)

add_library(echo SHARED echo/echo.c)
target_compile_definitions(echo PRIVATE ECHO_BUILDING)
weivel_strict_warnings(echo)
set_target_properties(echo PROPERTIES
    C_VISIBILITY_PRESET hidden
    ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/shared)
weivel_test(echo_test echo)

add_executable(echo_unload_test echo_unload_test.c)
target_link_libraries(echo_unload_test PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(echo_unload_test echo)
weivel_strict_warnings(echo_unload_test)
add_test(NAME echo_unload_test COMMAND echo_unload_test $<TARGET_FILE:echo>)
set_tests_properties(echo_unload_test PROPERTIES TIMEOUT 60)

add_executable(layouts layouts/layouts.c)
target_include_directories(layouts PRIVATE ${PROJECT_SOURCE_DIR}/include)
weivel_strict_warnings(layouts)
add_test(NAME layouts_list_every_public_struct_and_field
    COMMAND ${CMAKE_COMMAND} -DLAYOUTS=$<TARGET_FILE:layouts>
        -DHEADERS=${PROJECT_SOURCE_DIR}/include -P ${CMAKE_CURRENT_SOURCE_DIR}/layouts/layouts_check.cmake)
foreach(scenario
        layouts_check_reads_every_shape_of_field
        layouts_check_refuses_a_struct_without_a_layout
        layouts_check_refuses_a_layout_no_header_declares
        layouts_check_refuses_a_struct_defined_after_its_forward_typedef
        layouts_check_refuses_an_anonymous_typedef_struct
        layouts_check_refuses_a_struct_with_a_comment_before_its_brace
        layouts_check_refuses_a_forgotten_field
        layouts_check_refuses_fields_out_of_order
        layouts_check_refuses_fields_without_a_sizeof_line
        layouts_check_refuses_a_malformed_line)
    add_test(NAME ${scenario}
        COMMAND ${CMAKE_COMMAND} -DSCENARIO=${scenario} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/layouts-work
            -DLAYOUTS=$<TARGET_FILE:layouts> -DHEADERS=${PROJECT_SOURCE_DIR}/include
            -DCHECK_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/layouts/layouts_check.cmake
            -P ${CMAKE_CURRENT_SOURCE_DIR}/layouts/layouts_check_test.cmake)
endforeach()

add_test(NAME public_headers_are_ascii
    COMMAND ${CMAKE_COMMAND} -DDIRECTORY=${PROJECT_SOURCE_DIR}/include -P ${CMAKE_CURRENT_SOURCE_DIR}/ascii_check.cmake)
