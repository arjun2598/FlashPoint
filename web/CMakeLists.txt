# The WebAssembly build.
#
# Only configured under `emcmake cmake`, so a normal build never sees this and
# DD-005's "clone and build with nothing else installed" is untouched. There is
# no Emscripten dependency unless you ask for one.

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR
        "FLASHPOINT_BUILD_WEB requires Emscripten. Configure with:\n"
        "  emcmake cmake -S . -B build/web -DCMAKE_BUILD_TYPE=Release")
endif()

# Generated here rather than reused from the demo target, so the web build does
# not require FLASHPOINT_BUILD_DEMO. Both read the same scenario file.
file(READ "${CMAKE_SOURCE_DIR}/demo/scenarios/tour.txt" FLASHPOINT_DEFAULT_SCENARIO)
configure_file(
    "${CMAKE_SOURCE_DIR}/demo/default_scenario.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/default_scenario.hpp"
    @ONLY)

add_executable(flashpoint_web
    bindings.cpp
    ${CMAKE_SOURCE_DIR}/demo/generator.cpp
    ${CMAKE_SOURCE_DIR}/demo/render.cpp
    ${CMAKE_SOURCE_DIR}/demo/session.cpp)

target_link_libraries(flashpoint_web
    PRIVATE
        FlashPoint::flashpoint
        flashpoint_warnings)

# The demo's headers, plus the generated default_scenario.hpp that the demo
# target produces. Both front ends embed the same scenario file.
target_include_directories(flashpoint_web
    PRIVATE
        "${CMAKE_SOURCE_DIR}/demo"
        "${CMAKE_CURRENT_BINARY_DIR}")

set_target_properties(flashpoint_web PROPERTIES
    CXX_EXTENSIONS OFF
    OUTPUT_NAME flashpoint
    SUFFIX ".js")

target_link_options(flashpoint_web PRIVATE
    # A module rather than a script, so the page controls when it loads and the
    # global namespace stays clean.
    "-sMODULARIZE=1"
    "-sEXPORT_NAME=createFlashPoint"
    "-sEXPORT_ES6=1"
    "-sENVIRONMENT=web"

    # ccall marshals the string arguments and return values; the rest is plain C.
    "-sEXPORTED_RUNTIME_METHODS=['ccall','UTF8ToString']"
    "-sEXPORTED_FUNCTIONS=['_fp_run_script','_fp_generate','_fp_default_scenario','_fp_version','_malloc','_free']"

    # A two-million-order run allocates a large book, and the default heap is
    # too small for it. Growth is enabled rather than reserving the maximum up
    # front so a small run does not pay for a large one.
    "-sALLOW_MEMORY_GROWTH=1"
    "-sINITIAL_MEMORY=64MB"

    # No filesystem: nothing here reads or writes files.
    "-sFILESYSTEM=0"

    "-O3")

# Copy the page next to the built module so the output directory is servable
# as-is.
foreach(asset index.html app.js style.css)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/${asset}"
                   "${CMAKE_CURRENT_BINARY_DIR}/${asset}" COPYONLY)
endforeach()
