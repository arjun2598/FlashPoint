cmake_minimum_required(VERSION 3.25)

project(FlashPoint
    VERSION 0.1.0
    DESCRIPTION "A low-latency limit order book and matching engine"
    HOMEPAGE_URL "https://github.com/arjun2598/FlashPoint"
    LANGUAGES CXX)

# Sanity guard
if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not permitted. Configure into a separate directory, e.g.\n"
        "  cmake -S . -B build")
endif()

# Options
option(FLASHPOINT_BUILD_TESTS "Build the FlashPoint unit tests" ${PROJECT_IS_TOP_LEVEL})

# Off by default. Benchmarks are only meaningful in a Release build (rule 1 in
# docs/PERFORMANCE.md), so the release presets turn this on and the dev preset
# leaves it off to keep the everyday configure fast.
option(FLASHPOINT_BUILD_BENCHMARKS "Build the FlashPoint benchmarks" OFF)
option(FLASHPOINT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ${PROJECT_IS_TOP_LEVEL})
option(FLASHPOINT_ENABLE_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)


# Global build settings

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

get_property(_flashpoint_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _flashpoint_is_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
    message(STATUS "CMAKE_BUILD_TYPE was unset; defaulting to Release")
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)

flashpoint_define_warning_target(flashpoint_warnings)
flashpoint_define_sanitizer_target(flashpoint_sanitizers)

# Generated headers

set(FLASHPOINT_GENERATED_INCLUDE_DIR "${PROJECT_BINARY_DIR}/generated/include")
configure_file(
    "${PROJECT_SOURCE_DIR}/include/flashpoint/version.hpp.in"
    "${FLASHPOINT_GENERATED_INCLUDE_DIR}/flashpoint/version.hpp"
    @ONLY)

# Engine library

add_library(flashpoint
    src/order_book.cpp
    src/version.cpp)
add_library(FlashPoint::flashpoint ALIAS flashpoint)

target_include_directories(flashpoint
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${FLASHPOINT_GENERATED_INCLUDE_DIR}>)

target_compile_features(flashpoint PUBLIC cxx_std_20)

target_link_libraries(flashpoint
    PRIVATE flashpoint_warnings
    PUBLIC  flashpoint_sanitizers)

set_target_properties(flashpoint PROPERTIES
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)


# Tests
if(FLASHPOINT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(FLASHPOINT_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()


# Configuration summary
message(STATUS "")
message(STATUS "FlashPoint ${PROJECT_VERSION}")
message(STATUS "  Compiler ............ ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Build type .......... ${CMAKE_BUILD_TYPE}")
message(STATUS "  Tests ............... ${FLASHPOINT_BUILD_TESTS}")
message(STATUS "  Benchmarks .......... ${FLASHPOINT_BUILD_BENCHMARKS}")
message(STATUS "  Warnings as errors .. ${FLASHPOINT_WARNINGS_AS_ERRORS}")
message(STATUS "  Sanitizers .......... ${FLASHPOINT_ENABLE_SANITIZERS}")
message(STATUS "")
