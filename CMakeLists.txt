cmake_minimum_required(VERSION 3.26)

project(scenedetect)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
set(CMAKE_COLOR_MAKEFILE TRUE)
set(CMAKE_COLOR_DIAGNOSTICS ON)


# required for no dependencies on Windows (other than ffmpeg shared libs)
add_compile_options(-static-libstdc++ -static-libgcc -static)
add_link_options(-static-libstdc++ -static-libgcc -static)

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

add_executable(scenedetect main.cpp)

target_compile_options(scenedetect PRIVATE -Wall -Wpedantic -Wextra -Wformat)
target_link_libraries( scenedetect PkgConfig::LIBAV )
target_include_directories(scenedetect PRIVATE ./third_party/ffmpeg_build/include)
