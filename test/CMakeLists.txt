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

add_test(NAME public_headers_are_ascii
    COMMAND ${CMAKE_COMMAND} -DDIRECTORY=${PROJECT_SOURCE_DIR}/include -P ${CMAKE_CURRENT_SOURCE_DIR}/ascii_check.cmake)
add_test(NAME sources_call_nothing_forbidden
    COMMAND ${CMAKE_COMMAND} -DSOURCE_ROOT=${PROJECT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/forbidden_test.cmake)
add_test(NAME library_links_nothing_forbidden
    COMMAND ${CMAKE_COMMAND} -DLIBRARY=$<TARGET_FILE:weivel_core> -P ${CMAKE_CURRENT_SOURCE_DIR}/forbidden_test.cmake)
add_test(NAME no_foreign_names
    COMMAND ${CMAKE_COMMAND} -DSOURCE_ROOT=${PROJECT_SOURCE_DIR} -DWORDS=${CMAKE_CURRENT_SOURCE_DIR}/names.txt
        -P ${CMAKE_CURRENT_SOURCE_DIR}/names_test.cmake)
