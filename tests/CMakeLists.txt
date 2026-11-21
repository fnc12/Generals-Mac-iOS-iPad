# GeneralsX @build fnc12 21/11/2026 Unit tests (Catch2, same setup as sqlite_orm).
# Off by default: configure with -DGENERALSX_BUILD_TESTS=ON, then
#   cmake --build <build dir> --target unit_tests && ctest --test-dir <build dir> --output-on-failure
# Tests link only small engine libraries, never the whole game, and need no game data.
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
