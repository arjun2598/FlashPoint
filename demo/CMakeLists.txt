# The tour is embedded in the binary so that the demo runs correctly from any
# working directory. scenarios/tour.txt stays the single source of truth; this
# generates a header from it at configure time.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/scenarios/tour.txt" FLASHPOINT_DEFAULT_SCENARIO)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/default_scenario.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/default_scenario.hpp"
    @ONLY)

add_executable(flashpoint_demo
    generator.cpp
    main.cpp
    render.cpp
    session.cpp)

target_link_libraries(flashpoint_demo
    PRIVATE
        FlashPoint::flashpoint
        flashpoint_warnings)

target_include_directories(flashpoint_demo
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}")

set_target_properties(flashpoint_demo PROPERTIES CXX_EXTENSIONS OFF)
