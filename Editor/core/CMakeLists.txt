# END editor core: headless C++20 library with the editor logic up to the view-model layer.
# Native clients (macOS Swift first; later C# on Windows, gtkmm on Linux) link it.
# Core -> client: a shared SQLite file (WAL), the core is the only writer.
# Client -> core: command strings through the C ABI in include/end_editor/editor_core_c.h.
# It does not link the game engine and needs no vcpkg.
cmake_minimum_required(VERSION 3.25)
project(end_editor_core VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(END_EDITOR_BUILD_CLI "Build the headless command line harness" ON)
option(END_EDITOR_BUILD_TESTS "Build the unit tests" ON)

include(FetchContent)

# sqlite_orm is header only; SOURCE_SUBDIR points at a folder without a CMakeLists.txt so its own
# build (which needs find_package(SQLite3)) is not added.
FetchContent_Declare(sqlite_orm
    URL https://github.com/fnc12/sqlite_orm/archive/refs/tags/v1.9.1.tar.gz
    URL_HASH SHA256=de2db80e4f716a27c4e1f4cb8a356394e428676c98c90b0577b0431107d3cccf
    SOURCE_SUBDIR do-not-build
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(sqlite_orm)

# SQLite: the system library on Apple platforms (the Swift client links the same one),
# the amalgamation everywhere else (Windows has no SQLite development files).
if(APPLE)
    find_package(SQLite3 REQUIRED)
    set(END_EDITOR_SQLITE SQLite::SQLite3)
else()
    FetchContent_Declare(sqlite_amalgamation
        URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
        URL_HASH SHA256=77823cb110929c2bcb0f5d48e4833b5c59a8a6e40cdea3936b99e199dbbe5784
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(sqlite_amalgamation)
    add_library(end_editor_sqlite3 STATIC ${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c)
    target_include_directories(end_editor_sqlite3 PUBLIC ${sqlite_amalgamation_SOURCE_DIR})
    find_package(Threads REQUIRED)
    target_link_libraries(end_editor_sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
    set(END_EDITOR_SQLITE end_editor_sqlite3)
endif()

add_library(end_editor_core STATIC
    src/big_archive.cpp
    src/ini_objects.cpp
    src/game_files.cpp
    src/game_data.cpp
    src/w3d.cpp
    src/texture.cpp
    src/model_loader.cpp
    src/compression.cpp
    src/data_chunk.cpp
    src/map_file.cpp
    src/terrain_ini.cpp
    src/map_scene.cpp
    src/map_document.cpp
    src/map_editor.cpp
    src/map_preview.cpp
    src/game_launch.cpp
    src/editor_core.cpp
    src/editor_core_c.cpp
)
target_include_directories(end_editor_core
    PUBLIC include
    PRIVATE src)
# Plain -I, not SYSTEM: Apple clang searches /usr/local/include before -isystem paths, so a
# sqlite_orm installed there would silently replace the pinned one.
target_include_directories(end_editor_core BEFORE PRIVATE ${sqlite_orm_SOURCE_DIR}/include)
target_link_libraries(end_editor_core PUBLIC ${END_EDITOR_SQLITE})
# zlib only unpacks the rare "ZL" compressed maps; without it they are reported as unsupported.
find_package(ZLIB)
if(ZLIB_FOUND)
    target_link_libraries(end_editor_core PUBLIC ZLIB::ZLIB)
    target_compile_definitions(end_editor_core PRIVATE END_EDITOR_HAS_ZLIB=1)
endif()
if(MSVC)
    target_compile_options(end_editor_core PRIVATE /W4 /utf-8)
else()
    target_compile_options(end_editor_core PRIVATE -Wall -Wextra)
endif()
# The Swift client links the static library directly; keep it at a known path.
set_target_properties(end_editor_core PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if(END_EDITOR_BUILD_CLI)
    add_executable(end-editor-cli cli/main.cpp)
    target_link_libraries(end-editor-cli PRIVATE end_editor_core)
    # The round-trip check reads the original map bytes with the core's own file and unpacking code.
    target_include_directories(end-editor-cli PRIVATE src)
    target_include_directories(end-editor-cli BEFORE PRIVATE ${sqlite_orm_SOURCE_DIR}/include)
endif()

if(END_EDITOR_BUILD_TESTS)
    enable_testing()
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.8.0
        GIT_SHALLOW TRUE
        FIND_PACKAGE_ARGS 3 CONFIG)
    FetchContent_MakeAvailable(Catch2)
    add_executable(end_editor_tests tests/big_archive_tests.cpp tests/ini_objects_tests.cpp tests/editor_core_tests.cpp
        tests/model_tests.cpp tests/map_tests.cpp tests/map_edit_tests.cpp
        tests/map_flow_tests.cpp)
    target_include_directories(end_editor_tests PRIVATE src)
    target_include_directories(end_editor_tests BEFORE PRIVATE ${sqlite_orm_SOURCE_DIR}/include)
    target_link_libraries(end_editor_tests PRIVATE end_editor_core Catch2::Catch2WithMain)
    add_test(NAME end_editor_tests COMMAND end_editor_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()
