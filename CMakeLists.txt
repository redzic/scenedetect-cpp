cmake_minimum_required(VERSION 3.26)

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)

project(DiViEn)

# set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_BUILD_TYPE Debug)
# set(CMAKE_BUILD_TYPE Release)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
set(CMAKE_COLOR_MAKEFILE TRUE)
set(CMAKE_COLOR_DIAGNOSTICS ON)


# apparently the leak detection has to do with -fno-omit-frame-pointer in libavcodec.
add_compile_options(-fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer)
add_link_options(-fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer)

# required for no dependencies on Windows (other than ffmpeg shared libs)

# compile options needed for self-contained binary
# add_compile_options(-static-libstdc++ -static-libgcc -static)
# add_link_options(-static-libstdc++ -static-libgcc -static)

# Turning off exceptions causes the program to abort
# if an exception is thrown.
# add_compile_options(-fno-exceptions -fno-rtti)
# add_link_options(-fno-exceptions -fno-rtti)

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

# TODO can we isolate the code so that we turn on exceptions
# for only part of the code? like the part that deals with ASIO.
# I think it's possible.

add_executable(DiViEn main.cpp decode.cpp segment.cpp)

target_compile_options(DiViEn PRIVATE -Wall -Wextra -Wformat -Wno-c99-designator )
target_link_libraries( DiViEn PkgConfig::LIBAV )
