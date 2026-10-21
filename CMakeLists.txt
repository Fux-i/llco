cmake_minimum_required(VERSION 3.25)

project(llco VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Every executable lands in build/bin, so targets are addressable by name alone.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-stdlib=libc++)
    add_link_options(-stdlib=libc++)
endif()

enable_testing()

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.18.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(googletest)

FetchContent_Declare(
    atomic_queue
    GIT_REPOSITORY https://github.com/max0x7ba/atomic_queue.git
    GIT_TAG v1.9.4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(atomic_queue)

include(GoogleTest)

# Header-only core.
add_library(llco INTERFACE)
add_library(llco::llco ALIAS llco)
target_include_directories(llco INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(llco INTERFACE cxx_std_23)
target_link_libraries(llco INTERFACE atomic_queue::atomic_queue)

option(LLCO_TRACE "Emit Chrome Trace slices from the engine" OFF)
if(LLCO_TRACE)
    target_compile_definitions(llco INTERFACE LLCO_TRACE=1)
endif()

# One executable per tests/*.cpp. CONFIGURE_DEPENDS re-globs at build time, so
# a new test file needs no CMake edit.
file(GLOB LLCO_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
set(LLCO_LINT_SOURCES ${LLCO_TEST_SOURCES})
foreach(source IN LISTS LLCO_TEST_SOURCES)
    get_filename_component(name "${source}" NAME_WE)
    add_executable(${name} "${source}")
    target_link_libraries(${name} PRIVATE llco GTest::gtest_main)
    gtest_discover_tests(${name})
endforeach()

# Same for examples and benches; both land in build/bin.
foreach(subdir examples bench)
    file(GLOB LLCO_SUBDIR_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${subdir}/*.cpp")
    foreach(source IN LISTS LLCO_SUBDIR_SOURCES)
        get_filename_component(name "${source}" NAME_WE)
        add_executable(${name} "${source}")
        target_link_libraries(${name} PRIVATE llco)
        list(APPEND LLCO_LINT_SOURCES ${source})
    endforeach()
endforeach()
# clang-tidy over every TU; needs >= 21 for modernize-use-scoped-lock.
# [a-z]*/ keeps build/_deps (third-party) out of the header filter.
find_program(LLCO_CLANG_TIDY NAMES clang-tidy-22 clang-tidy-21 clang-tidy
             HINTS $ENV{HOME}/.local/bin)
if(LLCO_CLANG_TIDY)
    add_custom_target(lint
        COMMAND ${LLCO_CLANG_TIDY} -p ${CMAKE_BINARY_DIR} ${LLCO_LINT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMAND_EXPAND_LISTS
        USES_TERMINAL)
endif()
