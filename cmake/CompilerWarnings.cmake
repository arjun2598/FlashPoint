include_guard(GLOBAL)

function(flashpoint_define_warning_target target_name)
    add_library(${target_name} INTERFACE)

    set(clang_warnings
        -Wall
        -Wextra                 
        -Wpedantic              
        -Wshadow                
        -Wnon-virtual-dtor      
        -Wold-style-cast        # C-style casts hide const/reinterpret casts.
        -Wcast-align            # Casts that increase required alignment: UB, and slow on some ISAs.
        -Wunused
        -Woverloaded-virtual    # Overload that hides a virtual, rather than overriding it.
        -Wconversion            # Implicit narrowing. Deliberate: prices and quantities are
        -Wsign-conversion       # fixed-width integers, and silent narrowing is a correctness bug.
        -Wdouble-promotion      # Accidental float -> double.
        -Wformat=2
        -Wimplicit-fallthrough  # Missing break in a switch.
        -Wnull-dereference)

    set(gcc_warnings
        ${clang_warnings}
        -Wmisleading-indentation
        -Wduplicated-cond
        -Wduplicated-branches
        -Wlogical-op            # Bitwise operator used where logical was likely intended.
        -Wuseless-cast)

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(warnings ${clang_warnings})
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(warnings ${gcc_warnings})
    else()
        message(WARNING
            "FlashPoint has no warning policy for compiler '${CMAKE_CXX_COMPILER_ID}'; "
            "building without additional warnings.")
        set(warnings "")
    endif()

    if(FLASHPOINT_WARNINGS_AS_ERRORS)
        list(APPEND warnings -Werror)
    endif()

    target_compile_options(${target_name} INTERFACE ${warnings})
endfunction()
