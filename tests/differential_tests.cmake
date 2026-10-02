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
