cmake_minimum_required(VERSION 3.20)
project(wcs_native_vcam LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/../../ffmpeg-master-latest-win64-gpl-shared")
set(FFMPEG_INCLUDE "${FFMPEG_ROOT}/include")
set(FFMPEG_LIB "${FFMPEG_ROOT}/lib")

if(NOT EXISTS "${FFMPEG_INCLUDE}/libavformat/avformat.h")
    message(FATAL_ERROR "FFmpeg headers not found at ${FFMPEG_INCLUDE}")
endif()

if(NOT EXISTS "${FFMPEG_LIB}/avformat.lib")
    message(FATAL_ERROR "FFmpeg import libs not found at ${FFMPEG_LIB}")
endif()

add_executable(wcs_native_vcam
    src/main.cpp
)

target_compile_definitions(wcs_native_vcam PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
target_include_directories(wcs_native_vcam PRIVATE "${FFMPEG_INCLUDE}")
target_link_directories(wcs_native_vcam PRIVATE "${FFMPEG_LIB}")
target_link_libraries(wcs_native_vcam PRIVATE
    avformat
    avcodec
    avutil
    swscale
    ws2_32
    bcrypt
)
