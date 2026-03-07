cmake_minimum_required(VERSION 3.20)
project(wcs_native_winui_launcher LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(wcs_native_winui_launcher WIN32
    src/main.cpp
    src/launcher.rc
)

target_compile_definitions(wcs_native_winui_launcher PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(wcs_native_winui_launcher PRIVATE
    user32
    shell32
)
