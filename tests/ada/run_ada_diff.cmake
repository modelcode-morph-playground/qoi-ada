# Runs the optional Ada differential test (CTest test qoi_ada_diff).
#
#   cmake -DHELPER=<qoi_ada_diff> -DADA_PROJECT_DIR=<dir with qoi.gpr>
#         -DDRIVER_SRC_DIR=<tests/ada> -DWORK_DIR=<scratch dir> -P run_ada_diff.cmake
#
# Steps: build tests/ada/qoi_driver.adb with the ORIGINAL Ada package (gprbuild),
# let `qoi_ada_diff gen` write the inputs and the C++ results, run the Ada
# driver over them, then `qoi_ada_diff check` compares both sets of results.
#
# The test is SKIPPED, not failed, when gprbuild or an Ada compiler is missing
# or when the Ada project cannot be found: the script prints QOI_ADA_DIFF_SKIPPED
# (CTest's SKIP_REGULAR_EXPRESSION) and, with CMake >= 3.29, exits with 77
# (CTest's SKIP_RETURN_CODE). A toolchain that is present but fails to build or
# run the driver is a real failure.
#
# Nothing is written into the source tree: qoi.gpr and src/ are copied into the
# scratch directory, together with config/qoi_config.gpr (generated, in the
# absence of the Alire-created one, because qoi.gpr says
# `with "config/qoi_config.gpr"` and that file is not under version control).
cmake_minimum_required(VERSION 3.20)

macro(qoi_ada_skip reason)
    message(STATUS "QOI_ADA_DIFF_SKIPPED: ${reason}")
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
        cmake_language(EXIT 77)
    endif()
    return()
endmacro()

foreach(_required HELPER ADA_PROJECT_DIR DRIVER_SRC_DIR WORK_DIR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "run_ada_diff.cmake: -D${_required}=... is required")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Toolchain
# ---------------------------------------------------------------------------
find_program(QOI_GPRBUILD NAMES gprbuild)
find_program(QOI_GNATLS NAMES gnatls)
if(NOT QOI_GPRBUILD)
    qoi_ada_skip("gprbuild not found (install GNAT and GPRBuild, for example with Alire)")
endif()
if(NOT QOI_GNATLS)
    qoi_ada_skip("no GNAT Ada compiler found (gnatls missing); gprbuild alone cannot build Ada")
endif()

# ---------------------------------------------------------------------------
# Ada project
# ---------------------------------------------------------------------------
set(_project "${ADA_PROJECT_DIR}")
if(NOT EXISTS "${_project}/qoi.gpr" OR NOT EXISTS "${_project}/src/qoi.adb" OR NOT EXISTS "${_project}/src/qoi.ads")
    qoi_ada_skip("no Ada project (qoi.gpr, src/qoi.ads, src/qoi.adb) in ${ADA_PROJECT_DIR}")
endif()

# ---------------------------------------------------------------------------
# Scratch copy of the Ada project, plus the driver
# ---------------------------------------------------------------------------
set(_ada "${WORK_DIR}/ada")
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${_ada}/qoi/src" "${_ada}/driver" "${WORK_DIR}/cases")

file(COPY "${_project}/qoi.gpr" DESTINATION "${_ada}/qoi")
file(GLOB _ada_sources "${_project}/src/*.ads" "${_project}/src/*.adb")
file(COPY ${_ada_sources} DESTINATION "${_ada}/qoi/src")
if(EXISTS "${_project}/config/qoi_config.gpr")
    file(COPY "${_project}/config/qoi_config.gpr" DESTINATION "${_ada}/qoi/config")
else()
    file(MAKE_DIRECTORY "${_ada}/qoi/config")
    file(WRITE "${_ada}/qoi/config/qoi_config.gpr"
"--  Minimal replacement for the Alire-generated configuration project.
abstract project Qoi_Config is
   Crate_Version := \"0.1.0\";
   Crate_Name := \"qoi\";
end Qoi_Config;
")
endif()

file(COPY "${DRIVER_SRC_DIR}/qoi_driver.adb" "${DRIVER_SRC_DIR}/qoi_driver.gpr" DESTINATION "${_ada}/driver")

# ---------------------------------------------------------------------------
# Build the driver
# ---------------------------------------------------------------------------
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "GPR_PROJECT_PATH=${_ada}/qoi"
            "${QOI_GPRBUILD}" -P qoi_driver.gpr -p
    WORKING_DIRECTORY "${_ada}/driver"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "gprbuild failed (${_rc}) for the Ada driver\n${_out}\n${_err}")
endif()

if(EXISTS "${_ada}/driver/qoi_driver.exe")
    set(_driver "${_ada}/driver/qoi_driver.exe")
elseif(EXISTS "${_ada}/driver/qoi_driver")
    set(_driver "${_ada}/driver/qoi_driver")
else()
    message(FATAL_ERROR "gprbuild succeeded but produced no qoi_driver executable in ${_ada}/driver\n${_out}")
endif()

# ---------------------------------------------------------------------------
# gen -> Ada driver -> check
# ---------------------------------------------------------------------------
set(_cases "${WORK_DIR}/cases")

execute_process(COMMAND "${HELPER}" gen "${_cases}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "qoi_ada_diff gen failed (${_rc})\n${_out}\n${_err}")
endif()
message(STATUS "${_out}")

execute_process(COMMAND "${_driver}" "${_cases}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
message(STATUS "${_out}")
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "the Ada driver failed (${_rc})\n${_err}")
endif()

execute_process(COMMAND "${HELPER}" check "${_cases}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
message(STATUS "${_out}")
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "the Ada and C++ results differ (${_rc})\n${_err}")
endif()

message(STATUS "QOI_ADA_DIFF_PASSED")
