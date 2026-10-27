# A project using fynepdf-pdfa: builds examples/convert.cpp against it.
#   cmake -S examples -B build/example && cmake --build build/example
cmake_minimum_required(VERSION 3.16)
project(fynepdf_pdfa_example LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(fynepdf_pdfa
    GIT_REPOSITORY https://github.com/birazzzz/fynepdf-pdfa.git
    GIT_TAG v0.1.1)
# Or, from an installed copy (or vcpkg): find_package(fynepdf_pdfa CONFIG REQUIRED)
FetchContent_MakeAvailable(fynepdf_pdfa)

add_executable(convert convert.cpp)
target_link_libraries(convert PRIVATE fynepdf::pdfa)
