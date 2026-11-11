cmake_minimum_required(VERSION 3.21)  # PROJECT_IS_TOP_LEVEL
project(skqe_papyrus VERSION 0.92 LANGUAGES CXX)

# The version above is the one place to change it for a release (release.yml
# reads it). The library's kVersion must say the same, or nothing configures.
file(STRINGS include/papyrus/pex.hpp SKQE_PAPYRUS_KVERSION REGEX "kVersion = \"")
if(NOT SKQE_PAPYRUS_KVERSION MATCHES "\"${PROJECT_VERSION}\"")
    message(FATAL_ERROR "kVersion in include/papyrus/pex.hpp is not ${PROJECT_VERSION}, the version in CMakeLists.txt")
endif()

option(SKQE_PAPYRUS_BUILD_TOOLS "Build the pex command line program" ${PROJECT_IS_TOP_LEVEL})
option(SKQE_PAPYRUS_BUILD_TESTS "Build and register the unit tests" ${PROJECT_IS_TOP_LEVEL})
option(SKQE_PAPYRUS_FUZZ "Build the libFuzzer target pex_fuzz (separate build dir)" OFF)

if(PROJECT_IS_TOP_LEVEL AND MSVC)
    # The shipped tool should not need a CRT redistributable. Consumers that
    # add this project keep their own setting.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# The library: model, reader, writer, views and analyses. No dependencies.
add_library(skqe_papyrus STATIC
    src/pex_model.cpp
    src/pex_read.cpp
    src/pex_write.cpp
    src/json_writer.cpp
    src/asm_writer.cpp
    src/assembler.cpp
    src/compile/lexer.cpp
    src/compile/parser.cpp
    src/compile/compile.cpp
    src/compile/lower.cpp
    src/compile/codegen.cpp
    src/canon.cpp
    src/layout.cpp
    src/decompile/lift.cpp
    src/decompile/structure.cpp
    src/decompile/psc_writer.cpp
    src/analysis.cpp
    src/audit.cpp
    src/diff.cpp)
add_library(skqe::papyrus ALIAS skqe_papyrus)
target_include_directories(skqe_papyrus PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(skqe_papyrus PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(skqe_papyrus PRIVATE /W4 /permissive- /utf-8 /EHsc)
else()
    target_compile_options(skqe_papyrus PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(SKQE_PAPYRUS_BUILD_TOOLS)
    # bsa-reader (GPL-3.0, github.com/cecell/bsa-reader) reads scripts out of
    # archives for the tool only; the library itself has no dependencies.
    # Pinned to the commit of tag v0.1.1; library changes go to that repo,
    # then bump this pin.
    include(FetchContent)
    FetchContent_Declare(bsa_reader
        GIT_REPOSITORY https://github.com/cecell/bsa-reader.git
        GIT_TAG 88e40b4022050d0ea7d850a7804c56dcca58b77f)  # v0.1.1
    # skqe, the library of Skyrim Query Engine (GPL-3.0,
    # github.com/cecell/skyrim-query-engine): the
    # load order and which archives load, for patch --out and winners, so that
    # the rule lives in one place. Changes go to that repo, then bump this pin.
    # Only the library is built, not its tool or tests.
    FetchContent_Declare(skqe_lib
        GIT_REPOSITORY https://github.com/cecell/skyrim-query-engine.git
        GIT_TAG 29f64145a0bd85bf1f63194b35ba6a4f3c8c40da  # after its own pins were rewritten; the library target is skqe
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(bsa_reader skqe_lib)
    # skqe registers its test ("unit") whether or not it is the top project; it
    # is not built here, and skqe's own repo runs it. ctest applies this to every
    # test of that name, so no test here may be called unit.
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
        set_property(TEST unit DIRECTORY ${skqe_lib_BINARY_DIR} PROPERTY DISABLED TRUE)
    endif()

    add_executable(pex tools/pex.cpp tools/archive_patch.cpp tools/out_patch.cpp)
    target_link_libraries(pex PRIVATE skqe::papyrus bsa::reader skqe)
    if(MSVC)
        target_compile_options(pex PRIVATE /W4 /permissive- /utf-8 /EHsc)
        target_compile_definitions(pex PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    else()
        target_compile_options(pex PRIVATE -Wall -Wextra -Wpedantic)
        find_package(Threads REQUIRED)
        target_link_libraries(pex PRIVATE Threads::Threads)
    endif()
endif()

if(SKQE_PAPYRUS_BUILD_TESTS)
    enable_testing()
    add_executable(skqe_papyrus_tests tests/test_pex.cpp)
    target_link_libraries(skqe_papyrus_tests PRIVATE skqe::papyrus)
    target_include_directories(skqe_papyrus_tests PRIVATE src)
    if(MSVC)
        target_compile_options(skqe_papyrus_tests PRIVATE /W4 /permissive- /utf-8 /EHsc)
    endif()
    add_test(NAME papyrus_unit COMMAND skqe_papyrus_tests)
    if(SKQE_PAPYRUS_BUILD_TOOLS)
        # Needs bsa-reader, which only the tool links.
        add_executable(skqe_archive_patch_tests tests/test_archive_patch.cpp tools/archive_patch.cpp tools/out_patch.cpp)
        target_link_libraries(skqe_archive_patch_tests PRIVATE skqe::papyrus bsa::reader skqe)
        target_include_directories(skqe_archive_patch_tests PRIVATE tools)
        if(MSVC)
            target_compile_options(skqe_archive_patch_tests PRIVATE /W4 /permissive- /utf-8 /EHsc)
        endif()
        add_test(NAME archive_patch COMMAND skqe_archive_patch_tests)
    endif()
endif()

if(SKQE_PAPYRUS_FUZZ)
    # cmake -B build-fuzz -DSKQE_PAPYRUS_FUZZ=ON -DSKQE_PAPYRUS_BUILD_TOOLS=OFF ...
    # then build the target pex_fuzz alone (coverage-instrumented code needs
    # libFuzzer's runtime).
    target_compile_options(skqe_papyrus PRIVATE /Zi /fsanitize=address /fsanitize-coverage=inline-8bit-counters
        /fsanitize-coverage=edge /fsanitize-coverage=trace-cmp)
    add_executable(pex_fuzz tests/fuzz_pex.cpp)
    target_compile_options(pex_fuzz PRIVATE /Zi /fsanitize=address /fsanitize=fuzzer)
    target_link_libraries(pex_fuzz PRIVATE skqe::papyrus)
    target_link_options(pex_fuzz PRIVATE /INCREMENTAL:NO /DEBUG)
endif()

include(GNUInstallDirs)
install(TARGETS skqe_papyrus EXPORT skqe_papyrusTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT skqe_papyrusTargets NAMESPACE skqe:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/skqe_papyrus)
