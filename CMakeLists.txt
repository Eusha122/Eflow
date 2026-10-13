cmake_minimum_required(VERSION 3.24)
project(Eflow LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(eflow WIN32
    src/main.cpp
    src/App.cpp
    src/core/Gfx.cpp
    src/island/IslandWindow.cpp
    src/app.manifest
)
target_include_directories(eflow PRIVATE src)
target_compile_definitions(eflow PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(eflow PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
target_link_libraries(eflow PRIVATE d3d11 dxgi d2d1 dwrite dcomp dxguid shell32 user32 advapi32 shcore ole32)
