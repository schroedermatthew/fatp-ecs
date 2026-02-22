cmake_minimum_required(VERSION 3.20)
project(fatp-ecs VERSION 0.1.0 LANGUAGES CXX)

# ==============================================================================
# C++ Standard
# ==============================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ==============================================================================
# Warning Flags (shared by tests and demo)
# ==============================================================================
if(MSVC)
    set(FATP_ECS_WARNING_FLAGS /W4 /WX /Zc:preprocessor)
else()
    set(FATP_ECS_WARNING_FLAGS -Wall -Wextra -Wpedantic -Werror)
endif()

# ==============================================================================
# FAT-P Dependency
# ==============================================================================
# FAT-P is header-only. We create an interface library pointing to its include dir.
# Users must set FATP_INCLUDE_DIR or have FAT-P in a sibling directory.

if(NOT DEFINED FATP_INCLUDE_DIR)
    # Try sibling directory layout (common for development)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../FatP/include/fat_p")
        set(FATP_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../FatP/include")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/FatP/include/fat_p")
        set(FATP_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/FatP/include")
    else()
        message(FATAL_ERROR
            "FAT-P include directory not found. Set FATP_INCLUDE_DIR or place FAT-P in ../FatP/include/")
    endif()
endif()

message(STATUS "FAT-P include directory: ${FATP_INCLUDE_DIR}")

add_library(fat_p INTERFACE)
# SYSTEM: Treat FatP as an external dependency. FatP intentionally uses
# alignas(cache_line) on lock-free types (C4324) and has other MSVC
# informational warnings that are by design. FatP's own build props
# disable C4100/C4189 and don't use /WX. SYSTEM prevents these from
# failing our /WX build without suppressing warnings in our own code.
target_include_directories(fat_p SYSTEM INTERFACE "${FATP_INCLUDE_DIR}")

# ==============================================================================
# ECS Library (header-only)
# ==============================================================================
add_library(fatp_ecs INTERFACE)
target_include_directories(fatp_ecs INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(fatp_ecs INTERFACE fat_p)

# ==============================================================================
# Tests
# ==============================================================================
option(FATP_ECS_BUILD_TESTS "Build ECS tests" ON)

if(FATP_ECS_BUILD_TESTS)
    enable_testing()

    # Phase 1 tests
    add_executable(test_ecs tests/test_ecs.cpp)
    target_link_libraries(test_ecs PRIVATE fatp_ecs)
    target_compile_options(test_ecs PRIVATE ${FATP_ECS_WARNING_FLAGS})
    add_test(NAME test_ecs COMMAND test_ecs)

    # Phase 2 tests
    add_executable(test_ecs_phase2 tests/test_ecs_phase2.cpp)
    target_link_libraries(test_ecs_phase2 PRIVATE fatp_ecs)
    target_compile_options(test_ecs_phase2 PRIVATE ${FATP_ECS_WARNING_FLAGS})
    add_test(NAME test_ecs_phase2 COMMAND test_ecs_phase2)

    # Phase 3 tests
    add_executable(test_ecs_phase3 tests/test_ecs_phase3.cpp)
    target_link_libraries(test_ecs_phase3 PRIVATE fatp_ecs)
    target_compile_options(test_ecs_phase3 PRIVATE ${FATP_ECS_WARNING_FLAGS})
    add_test(NAME test_ecs_phase3 COMMAND test_ecs_phase3)

    add_executable(test_exclude_filters tests/test_exclude_filters.cpp)
    target_link_libraries(test_exclude_filters PRIVATE fatp_ecs)
    target_compile_options(test_exclude_filters PRIVATE ${FATP_ECS_WARNING_FLAGS})
    add_test(NAME test_exclude_filters COMMAND test_exclude_filters)

    add_executable(test_patch tests/test_patch.cpp)
    target_link_libraries(test_patch PRIVATE fatp_ecs)
    target_compile_options(test_patch PRIVATE ${FATP_ECS_WARNING_FLAGS})
    add_test(NAME test_patch COMMAND test_patch)

    add_executable(test_runtime_view tests/test_runtime_view.cpp)
    target_link_libraries(test_runtime_view PRIVATE fatp_ecs)
    target_compile_options(test_runtime_view PRIVATE ${FATP_ECS_WARNING_FLAGS})
    add_test(NAME test_runtime_view COMMAND test_runtime_view)

    add_executable(test_observer tests/test_observer.cpp)
    target_link_libraries(test_observer PRIVATE fatp_ecs)
    target_compile_options(test_observer PRIVATE ${FATP_ECS_WARNING_FLAGS})
    add_test(NAME test_observer COMMAND test_observer)

    # Registry::clear() stress test
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_clear.cpp")
        add_executable(test_clear tests/test_clear.cpp)
        target_link_libraries(test_clear PRIVATE fatp_ecs)
        target_compile_options(test_clear PRIVATE ${FATP_ECS_WARNING_FLAGS})
        add_test(NAME test_clear COMMAND test_clear)
    endif()
endif()

# ==============================================================================
# Demo Application (Phase 4)
# ==============================================================================
option(FATP_ECS_BUILD_DEMO "Build Phase 4 demo application" ON)

if(FATP_ECS_BUILD_DEMO)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/demo/main.cpp")
        add_executable(demo demo/main.cpp)
        target_link_libraries(demo PRIVATE fatp_ecs)
        target_compile_options(demo PRIVATE ${FATP_ECS_WARNING_FLAGS})
    else()
        message(STATUS "Demo source not found (demo/main.cpp) -- skipping demo target")
    endif()
endif()

# ==============================================================================
# Visual Demo (Phase 4b — SDL2)
# ==============================================================================
option(FATP_ECS_BUILD_VISUAL_DEMO "Build SDL2 visual demo (requires SDL2, SDL2_ttf)" OFF)

if(FATP_ECS_BUILD_VISUAL_DEMO)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/demo/visual_main.cpp")
        find_package(SDL2 REQUIRED)
        find_package(SDL2_ttf REQUIRED)

        add_executable(visual_demo demo/visual_main.cpp)
        target_link_libraries(visual_demo PRIVATE fatp_ecs)
        target_compile_options(visual_demo PRIVATE ${FATP_ECS_WARNING_FLAGS})

        # SDL2 linking varies by platform and find module version
        if(TARGET SDL2::SDL2)
            target_link_libraries(visual_demo PRIVATE SDL2::SDL2 SDL2::SDL2main)
        else()
            target_include_directories(visual_demo PRIVATE ${SDL2_INCLUDE_DIRS})
            target_link_libraries(visual_demo PRIVATE ${SDL2_LIBRARIES})
        endif()

        if(TARGET SDL2_ttf::SDL2_ttf)
            target_link_libraries(visual_demo PRIVATE SDL2_ttf::SDL2_ttf)
        else()
            target_include_directories(visual_demo PRIVATE ${SDL2_TTF_INCLUDE_DIRS})
            target_link_libraries(visual_demo PRIVATE ${SDL2_TTF_LIBRARIES})
        endif()
    else()
        message(STATUS "Visual demo source not found -- skipping")
    endif()
endif()

# ==============================================================================
# Benchmark Suite (FAT-P ECS vs EnTT)
# ==============================================================================
option(FATP_ECS_BUILD_BENCH "Build benchmark suite (requires EnTT via vcpkg)" OFF)

if(FATP_ECS_BUILD_BENCH)
    find_package(EnTT CONFIG REQUIRED)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bench/benchmark.cpp")
        add_executable(benchmark bench/benchmark.cpp)
        target_link_libraries(benchmark PRIVATE fatp_ecs EnTT::EnTT)
        # Relaxed warnings for benchmark target — EnTT headers may trigger
        # warnings under /W4 /WX that are not our responsibility to fix.
        if(MSVC)
            target_compile_options(benchmark PRIVATE /W3 /Zc:preprocessor)
        else()
            target_compile_options(benchmark PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    else()
        message(STATUS "Benchmark source not found (bench/benchmark.cpp) -- skipping")
    endif()
endif()
