# Standalone consumer of an installed qoi package. It is NOT part of the main
# build: CI installs qoi to a temporary prefix and then configures this project
# with -DCMAKE_PREFIX_PATH=<prefix>.
cmake_minimum_required(VERSION 3.20)

project(qoi_package_consumer LANGUAGES CXX)

find_package(qoi CONFIG REQUIRED)

add_executable(qoi_package_consumer main.cpp)
target_link_libraries(qoi_package_consumer PRIVATE qoi::qoi)

enable_testing()
add_test(NAME qoi_package_consumer COMMAND qoi_package_consumer)
