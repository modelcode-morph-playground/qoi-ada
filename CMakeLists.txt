cmake_minimum_required(VERSION 3.20)

project(qoi
    VERSION 0.1.0
    DESCRIPTION "QOI (Quite OK Image) codec, C++17"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Top-level detection and options
# ---------------------------------------------------------------------------
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(QOI_IS_TOP_LEVEL ON)
else()
    set(QOI_IS_TOP_LEVEL OFF)
endif()

option(QOI_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(QOI_SANITIZE "Build the library and tests with AddressSanitizer and UBSan" OFF)
option(QOI_BUILD_TESTS "Build the test suite" ${QOI_IS_TOP_LEVEL})
option(QOI_INSTALL "Generate install and export rules" ${QOI_IS_TOP_LEVEL})
option(QOI_CLANG_TIDY "Run clang-tidy on the library sources when it is available" OFF)

# Default to a Release build for single-config generators.
if(QOI_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

# Build-type flags for GCC/Clang. Release: -O3 -DNDEBUG. Debug: -g -Og, with
# assertions left enabled (no NDEBUG).
if(QOI_IS_TOP_LEVEL AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
    set(CMAKE_CXX_FLAGS_DEBUG "-g -Og")
endif()

# Windows has no RPATH: keep qoi.dll next to the test executables so shared
# builds can run from the build tree.
if(QOI_IS_TOP_LEVEL AND WIN32 AND BUILD_SHARED_LIBS AND NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# MSVC's /RTC1(default in Debug) is incompatible with /fsanitize=address.
if(MSVC AND QOI_SANITIZE)
    string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
endif()

# ---------------------------------------------------------------------------
# Build-only interface targets (never exported)
# ---------------------------------------------------------------------------
add_library(qoi_warnings INTERFACE)
add_library(qoi_sanitizers INTERFACE)

if(MSVC)
    target_compile_options(qoi_warnings INTERFACE /W4)
    if(QOI_WARNINGS_AS_ERRORS)
        target_compile_options(qoi_warnings INTERFACE /WX)
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(qoi_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(QOI_WARNINGS_AS_ERRORS)
        target_compile_options(qoi_warnings INTERFACE -Werror)
    endif()
endif()

if(QOI_SANITIZE)
    if(MSVC)
        target_compile_options(qoi_sanitizers INTERFACE /fsanitize=address)
        target_link_options(qoi_sanitizers INTERFACE /INCREMENTAL:NO)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set(_qoi_san_flags
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            list(APPEND _qoi_san_flags -fsanitize=implicit-conversion)
        endif()
        target_compile_options(qoi_sanitizers INTERFACE ${_qoi_san_flags})
        target_link_options(qoi_sanitizers INTERFACE ${_qoi_san_flags})
        unset(_qoi_san_flags)
    else()
        message(WARNING "QOI_SANITIZE is not supported for compiler "
                        "${CMAKE_CXX_COMPILER_ID}; ignoring")
    endif()
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

add_library(qoi src/qoi.cpp)
add_library(qoi::qoi ALIAS qoi)

target_include_directories(qoi PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(qoi PUBLIC cxx_std_17)

# The build-only targets are wrapped in BUILD_INTERFACE so that they do not
# end up in the export set.
target_link_libraries(qoi PRIVATE
    $<BUILD_INTERFACE:qoi_warnings>
    $<BUILD_INTERFACE:qoi_sanitizers>)

set_target_properties(qoi PROPERTIES
    CXX_EXTENSIONS OFF
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    OUTPUT_NAME qoi
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

if(QOI_CLANG_TIDY)
    find_program(QOI_CLANG_TIDY_EXE NAMES clang-tidy)
    if(QOI_CLANG_TIDY_EXE)
        set_target_properties(qoi PROPERTIES CXX_CLANG_TIDY "${QOI_CLANG_TIDY_EXE}")
    else()
        message(WARNING "QOI_CLANG_TIDY is ON but clang-tidy was not found")
    endif()
endif()

# ---------------------------------------------------------------------------
# Install and export
# ---------------------------------------------------------------------------
if(QOI_INSTALL)
    include(CMakePackageConfigHelpers)

    install(TARGETS qoi
        EXPORT qoiTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp")

    set(QOI_INSTALL_CMAKEDIR ${CMAKE_INSTALL_LIBDIR}/cmake/qoi)

    install(EXPORT qoiTargets
        NAMESPACE qoi::
        DESTINATION ${QOI_INSTALL_CMAKEDIR})

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/qoi-config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/qoi-config.cmake
        INSTALL_DESTINATION ${QOI_INSTALL_CMAKEDIR})

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/qoi-config-version.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/qoi-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/qoi-config-version.cmake
        DESTINATION ${QOI_INSTALL_CMAKEDIR})

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)
        install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
            DESTINATION ${CMAKE_INSTALL_DOCDIR})
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(QOI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
