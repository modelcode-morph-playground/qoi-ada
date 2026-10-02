# Test suite for qoi. One GoogleTest executable per test family, each
# registered with CTest through gtest_discover_tests.

# ---------------------------------------------------------------------------
# GoogleTest: installed package first, FetchContent fallback pinned to a tag
# ---------------------------------------------------------------------------
find_package(GTest CONFIG QUIET)
if(NOT TARGET GTest::gtest_main)
    find_package(GTest QUIET)
endif()

if(NOT TARGET GTest::gtest OR NOT TARGET GTest::gtest_main)
    include(FetchContent)

    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.17.0
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(googletest)

    if(NOT TARGET GTest::gtest)
        add_library(GTest::gtest ALIAS gtest)
    endif()
    if(NOT TARGET GTest::gtest_main)
        add_library(GTest::gtest_main ALIAS gtest_main)
    endif()
endif()

include(GoogleTest)

# ---------------------------------------------------------------------------
# qoi_add_test(<name> SOURCES <files...> [LABELS <labels...>])
#
# Builds the executable <name> from SOURCES, links it against qoi::qoi,
# GoogleTest (with main), and the build-only warning and sanitizer targets,
# and registers its test cases with CTest.
#
# Discovery runs when ctest starts (PRE_TEST), not at build time, so that
# sanitizer builds and cross builds never execute the binary during the build.
#
# LABELS: gtest_discover_tests cannot carry a list-valued property portably
# (semicolons inside PROPERTIES values are mangled differently across CMake
# versions), so the labels are joined with spaces into ONE CTest label string.
# CTest label filters are regular expressions, so `ctest -L smoke` and
# `ctest -L healthcheck` both match a test labelled "smoke healthcheck". Do
# not anchor label regexes with ^ or $.
# ---------------------------------------------------------------------------
function(qoi_add_test name)
    cmake_parse_arguments(PARSE_ARGV 1 QAT "" "" "SOURCES;LABELS")

    if(NOT QAT_SOURCES)
        message(FATAL_ERROR "qoi_add_test(${name}): SOURCES is required")
    endif()

    add_executable(${name} ${QAT_SOURCES})
    target_link_libraries(${name} PRIVATE
        qoi::qoi
        GTest::gtest_main
        qoi_warnings
        qoi_sanitizers)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(${name} PROPERTIES CXX_EXTENSIONS OFF)

    set(_qat_properties)
    if(QAT_LABELS)
        list(JOIN QAT_LABELS " " _qat_labels)
        list(APPEND _qat_properties LABELS "${_qat_labels}")
    endif()

    gtest_discover_tests(${name}
        DISCOVERY_MODE PRE_TEST
        PROPERTIES ${_qat_properties})
endfunction()

# ---------------------------------------------------------------------------
# Test families
# ---------------------------------------------------------------------------
qoi_add_test(qoi_smoke SOURCES smoke_test.cpp LABELS smoke healthcheck)

# ===== BEGIN per-family test registrations =====
# Later work appends one qoi_add_test(...) call per test family here (golden,
# roundtrip, contract, differential, fuzz), or include()s a per-family cmake
# snippet. Only register sources that exist.
# ===== END per-family test registrations =====
