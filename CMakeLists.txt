cmake_minimum_required(VERSION 3.20)
project(fatp-ecs VERSION 0.1.0 LANGUAGES CXX)

# ==============================================================================
# C++ Standard
# ==============================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ==============================================================================
# FAT-P Dependency
# ==============================================================================
# FAT-P is header-only. We create an interface library pointing to its include dir.
# Users must set FATP_INCLUDE_DIR or have FAT-P in a sibling directory.

if(NOT DEFINED FATP_INCLUDE_DIR)
    # Try sibling directory layout (common for development)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../FatP/FatP/include")
        set(FATP_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../FatP/FatP/include")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/FatP/include")
        set(FATP_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/FatP/include")
    else()
        message(FATAL_ERROR
            "FAT-P include directory not found. Set FATP_INCLUDE_DIR or place FAT-P in ../FatP/FatP/include")
    endif()
endif()

message(STATUS "FAT-P include directory: ${FATP_INCLUDE_DIR}")

add_library(fat_p INTERFACE)
target_include_directories(fat_p INTERFACE "${FATP_INCLUDE_DIR}")

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

    add_executable(test_ecs tests/test_ecs.cpp)
    target_link_libraries(test_ecs PRIVATE fatp_ecs)

    # Compiler warnings
    if(MSVC)
        target_compile_options(test_ecs PRIVATE /W4 /WX)
    else()
        target_compile_options(test_ecs PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()

    add_test(NAME test_ecs COMMAND test_ecs)
endif()
