include_guard(GLOBAL)

function(flashpoint_define_sanitizer_target target_name)
    add_library(${target_name} INTERFACE)

    if(NOT FLASHPOINT_ENABLE_SANITIZERS)
        return()
    endif()

    if(NOT (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU"))
        message(FATAL_ERROR
            "FLASHPOINT_ENABLE_SANITIZERS is ON but compiler "
            "'${CMAKE_CXX_COMPILER_ID}' is not supported. Use Clang or GCC.")
    endif()

    set(flags
        -fsanitize=address,undefined
        -fno-sanitize-recover=all
        -fno-omit-frame-pointer)

    target_compile_options(${target_name} INTERFACE ${flags})

    target_link_options(${target_name} INTERFACE ${flags})

    message(STATUS "Sanitizers enabled: address, undefined")
endfunction()
