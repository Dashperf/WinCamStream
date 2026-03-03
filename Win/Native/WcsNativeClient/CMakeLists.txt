cmake_minimum_required(VERSION 3.20)
project(wcs_native_client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(wcs_native_client
    src/main.cpp
)

target_compile_definitions(wcs_native_client PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(wcs_native_client PRIVATE
    ws2_32
    shell32
)
