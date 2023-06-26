cmake_minimum_required(VERSION 3.26)

project(scenedetect)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
set(CMAKE_COLOR_MAKEFILE TRUE)

add_compile_options(-fsanitize=address)
add_link_options(-fsanitize=address)
add_compile_options(-fsanitize=undefined)
add_link_options(-fsanitize=undefined)

# clang-tidy, cppcheck

# hardening

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET
    libavdevice
    libavfilter
    libavformat
    libavcodec
    libswresample
    libswscale
    libavutil
)

# set(CMAKE_CXX_CPPCHECK "cppcheck")

add_subdirectory(third_party/fmt)

add_executable(scenedetect main.cpp)

target_link_libraries( scenedetect PkgConfig::LIBAV fmt )
