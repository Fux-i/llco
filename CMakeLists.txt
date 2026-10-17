cmake_minimum_required(VERSION 3.20)

set(PROJECT_NAME "llco")

project(${PROJECT_NAME} VERSION 1.0 LANGUAGES CXX)

# Enforce the use of C++23 standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Disable compiler-specific extensions like __attribute__((...))
# set(CMAKE_CXX_EXTENSIONS OFF)
# Use libc++ standard (with clang)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++ -fcoroutines")
# build cache for clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS True)

include_directories(include)

# Auto find all source files on files in src/ directory change
file(GLOB_RECURSE SOURCES "src/*.c" "src/*.cc" "src/*.cpp")
set_property(DIRECTORY PROPERTY CMAKE_CONFIGURE_DEPENDS ${SOURCES})

add_executable(${PROJECT_NAME} ${SOURCES})
