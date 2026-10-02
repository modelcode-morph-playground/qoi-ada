# Differential tests (reference qoi.h, optional Ada) and the decode fuzz target.
#
# Included from tests/CMakeLists.txt after qoi_add_test() is defined, so
# CMAKE_CURRENT_SOURCE_DIR is the tests/ directory.

# ===========================================================================
# 1. Reference implementation (vendored qoi.h) and the qoi_diff_ref test
# ===========================================================================
# tests/third_party/qoi.h is third-party, MIT licensed, test-only code. It is
# compiled exactly once, in the wrapper translation unit, into a static
# library with ALL warnings off. That keeps the vendored header away from the
# project's -Wconversion -Werror profile (and from clang's implicit-conversion
# sanitizer, which would trap on its intentional signed char wrap-around), and
# test code only ever sees the plain buffer functions of the wrapper header.
set(QOI_REFERENCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

include(CheckLanguage)
check_language(C)
if(CMAKE_C_COMPILER)
    enable_language(C)
endif()

add_library(qoi_reference STATIC ${QOI_REFERENCE_DIR}/qoi_reference_wrapper.c)
if(NOT CMAKE_C_COMPILER)
    # The wrapper and qoi.h are written in the common subset of C and C++.
    set_source_files_properties(${QOI_REFERENCE_DIR}/qoi_reference_wrapper.c
        PROPERTIES LANGUAGE CXX)
endif()
if(MSVC)
    target_compile_options(qoi_reference PRIVATE /w)
else()
    target_compile_options(qoi_reference PRIVATE -w)
endif()
if(QOI_SANITIZE)
    # Only AddressSanitizer: the vendored code relies on signed char wrap-around
    # and must not be built with the recover-less UBSan / implicit-conversion
    # checks that the first-party code uses.
    if(MSVC)
        target_compile_options(qoi_reference PRIVATE /fsanitize=address)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(qoi_reference PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    endif()
endif()

qoi_add_test(qoi_diff_ref
    SOURCES differential_ref_test.cpp
    LABELS differential)
target_link_libraries(qoi_diff_ref PRIVATE qoi_reference)

# ===========================================================================
# 2. Optional differential test against the original Ada implementation
# ===========================================================================
# qoi_ada_diff is the C++ half (inputs, C++ results, comparison). The Ada half,
# tests/ada/qoi_driver.adb, is built and run by tests/ada/run_ada_diff.cmake
# only if GNAT and GPRBuild are installed; otherwise the test reports SKIPPED.
# It is registered unconditionally, so a skip is visible in every ctest run.
#
# QOI_ADA_PROJECT_DIR: directory holding qoi.gpr and src/ of the Ada package.
# The default is the repository root while the Ada sources still live there,
# and legacy/ada afterwards. (If the cached value no longer holds qoi.gpr, the
# script also tries legacy/ada.)
if(EXISTS "${PROJECT_SOURCE_DIR}/qoi.gpr")
    set(_qoi_ada_default_dir "${PROJECT_SOURCE_DIR}")
else()
    set(_qoi_ada_default_dir "${PROJECT_SOURCE_DIR}/legacy/ada")
endif()
set(QOI_ADA_PROJECT_DIR "${_qoi_ada_default_dir}" CACHE PATH
    "Directory with qoi.gpr and src/ of the Ada implementation (optional Ada differential test)")
unset(_qoi_ada_default_dir)

add_executable(qoi_ada_diff ada_diff_test.cpp)
target_link_libraries(qoi_ada_diff PRIVATE qoi::qoi qoi_warnings qoi_sanitizers)
target_include_directories(qoi_ada_diff PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(qoi_ada_diff PROPERTIES CXX_EXTENSIONS OFF)

# The file protocol and the comparison logic, exercised without Ada: the helper
# plays the Ada driver itself and must also notice deliberately damaged results.
add_test(NAME qoi_ada_diff_selftest
    COMMAND qoi_ada_diff selftest ${CMAKE_CURRENT_BINARY_DIR}/ada_diff_selftest)
set_tests_properties(qoi_ada_diff_selftest PROPERTIES
    LABELS "differential"
    PASS_REGULAR_EXPRESSION "QOI_ADA_DIFF_SELFTEST_OK")

add_test(NAME qoi_ada_diff
    COMMAND ${CMAKE_COMMAND}
        -DHELPER=$<TARGET_FILE:qoi_ada_diff>
        -DADA_PROJECT_DIR=${QOI_ADA_PROJECT_DIR}
        -DADA_FALLBACK_DIR=${PROJECT_SOURCE_DIR}/legacy/ada
        -DDRIVER_SRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}/ada
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/ada_diff
        -P ${CMAKE_CURRENT_SOURCE_DIR}/ada/run_ada_diff.cmake)
set_tests_properties(qoi_ada_diff PROPERTIES
    LABELS "differential ada"
    SKIP_RETURN_CODE 77
    SKIP_REGULAR_EXPRESSION "QOI_ADA_DIFF_SKIPPED")

# ===========================================================================
# 3. Decode fuzz target and seed-corpus replay
# ===========================================================================
# fuzz/decode_fuzz.cpp defines LLVMFuzzerTestOneInput. It is linked into
#   qoi_fuzz_replay   always built; replays files and directories, any compiler;
#   qoi_decode_fuzz   a libFuzzer binary, only with a toolchain that supports
#                     -fsanitize=fuzzer (clang), option QOI_BUILD_FUZZ.
# CTest test qoi_fuzz_corpus replays the checked-in seed corpus (valid and
# malformed streams) through qoi_fuzz_replay.
option(QOI_BUILD_FUZZ "Build the libFuzzer decode fuzz target when the toolchain supports it" ON)

add_executable(qoi_fuzz_replay
    fuzz/decode_fuzz.cpp
    fuzz/replay_main.cpp)
target_link_libraries(qoi_fuzz_replay PRIVATE qoi::qoi qoi_warnings qoi_sanitizers)
target_include_directories(qoi_fuzz_replay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(qoi_fuzz_replay PROPERTIES CXX_EXTENSIONS OFF)

add_test(NAME qoi_fuzz_corpus
    COMMAND qoi_fuzz_replay ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus)
set_tests_properties(qoi_fuzz_corpus PROPERTIES
    LABELS "fuzz"
    PASS_REGULAR_EXPRESSION "QOI_FUZZ_REPLAY_OK")

if(QOI_BUILD_FUZZ)
    include(CheckCXXSourceCompiles)
    include(CMakePushCheckState)

    if(MSVC)
        set(_qoi_fuzzer_flag "/fsanitize=fuzzer")
    else()
        set(_qoi_fuzzer_flag "-fsanitize=fuzzer,address")
    endif()

    cmake_push_check_state(RESET)
    # CMAKE_REQUIRED_FLAGS reaches both the compile and the link step.
    set(CMAKE_REQUIRED_FLAGS "${_qoi_fuzzer_flag}")
    check_cxx_source_compiles("
#include <cstddef>
#include <cstdint>
extern \"C\" int LLVMFuzzerTestOneInput(const std::uint8_t*, std::size_t) { return 0; }
" QOI_HAVE_LIBFUZZER)
    cmake_pop_check_state()

    if(QOI_HAVE_LIBFUZZER)
        # The library is compiled a second time with coverage instrumentation,
        # which is what makes the fuzzer effective (the normal qoi target is
        # not instrumented). Nothing else uses this copy.
        add_library(qoi_fuzz_instrumented STATIC ${PROJECT_SOURCE_DIR}/src/qoi.cpp)
        target_include_directories(qoi_fuzz_instrumented PUBLIC ${PROJECT_SOURCE_DIR}/include)
        target_compile_features(qoi_fuzz_instrumented PUBLIC cxx_std_17)
        set_target_properties(qoi_fuzz_instrumented PROPERTIES CXX_EXTENSIONS OFF)
        target_link_libraries(qoi_fuzz_instrumented PRIVATE qoi_warnings)
        if(MSVC)
            target_compile_options(qoi_fuzz_instrumented PRIVATE /fsanitize=fuzzer /fsanitize=address)
        else()
            target_compile_options(qoi_fuzz_instrumented PRIVATE -fsanitize=fuzzer-no-link,address -fno-omit-frame-pointer)
        endif()

        add_executable(qoi_decode_fuzz fuzz/decode_fuzz.cpp)
        target_link_libraries(qoi_decode_fuzz PRIVATE qoi_fuzz_instrumented qoi_warnings)
        target_include_directories(qoi_decode_fuzz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        set_target_properties(qoi_decode_fuzz PROPERTIES CXX_EXTENSIONS OFF)
        if(MSVC)
            target_compile_options(qoi_decode_fuzz PRIVATE /fsanitize=fuzzer /fsanitize=address)
        else()
            target_compile_options(qoi_decode_fuzz PRIVATE -fsanitize=fuzzer-no-link,address -fno-omit-frame-pointer)
            target_link_options(qoi_decode_fuzz PRIVATE -fsanitize=fuzzer,address)
        endif()
        message(STATUS "qoi: libFuzzer decode fuzz target enabled (qoi_decode_fuzz)")
    else()
        message(STATUS "qoi: libFuzzer is not supported by this toolchain; "
                       "qoi_decode_fuzz is not built (qoi_fuzz_replay and the "
                       "qoi_fuzz_corpus test still are)")
    endif()
    unset(_qoi_fuzzer_flag)
else()
    message(STATUS "qoi: QOI_BUILD_FUZZ is OFF; qoi_decode_fuzz is not built")
endif()
