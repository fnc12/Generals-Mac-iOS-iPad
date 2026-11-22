# GeneralsX @build fnc12 21/11/2026 Unit tests (Catch2, same setup as sqlite_orm).
# Off by default: configure with -DGENERALSX_BUILD_TESTS=ON, then
#   cmake --build <build dir> --target unit_tests && ctest --test-dir <build dir> --output-on-failure
# Tests link only small engine libraries, never the whole game, and need no game data.
#
# The folder is also a project of its own (what CI uses): it builds only the libraries the tests
# need, without vcpkg, SDL3 or the game, in a minute on any platform:
#   cmake -S tests -B build/unit-tests && cmake --build build/unit-tests && ctest --test-dir build/unit-tests
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.25)
    project(generalsx_unit_tests LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    enable_testing()

    set(GENERALSX_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
    include(FetchContent)
    include(${GENERALSX_ROOT}/cmake/lzhl.cmake)
    add_subdirectory(${GENERALSX_ROOT}/Dependencies/Utility utility)

    # The parts of Core/CMakeLists.txt the libraries below need.
    add_library(corei_libraries_include INTERFACE)
    target_include_directories(corei_libraries_include INTERFACE ${GENERALSX_ROOT}/Core/Libraries/Include)
    add_library(corei_always INTERFACE)
    target_link_libraries(corei_always INTERFACE core_utility corei_libraries_include)

    add_subdirectory(${GENERALSX_ROOT}/Core/Libraries/Source/Compression compression)
endif()

include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.8.0
    GIT_SHALLOW TRUE
    # prefer find_package() over building from source
    FIND_PACKAGE_ARGS 3 CONFIG
)
FetchContent_MakeAvailable(Catch2)

add_executable(unit_tests
    compression_tests.cpp
)

target_link_libraries(unit_tests PRIVATE
    core_compression
    corei_libraries_include
    corei_always
    Catch2::Catch2WithMain
)

add_test(NAME "All_in_one_unit_test"
    COMMAND unit_tests
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
