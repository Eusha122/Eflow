cmake_minimum_required(VERSION 3.24)
project(Eflow LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- shaders: compiled offline with fxc into headers ------------------------------------------
find_program(FXC fxc REQUIRED DOC "HLSL compiler from the Windows SDK")
set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})
set(MASCOT_HLSL ${CMAKE_CURRENT_SOURCE_DIR}/src/mascot/Mascot.hlsl)
add_custom_command(
    OUTPUT ${SHADER_OUT}/mascot_vs.h ${SHADER_OUT}/mascot_ps.h
    COMMAND ${FXC} /nologo /O3 /T vs_5_0 /E vs_main /Vn g_mascotVS /Fh ${SHADER_OUT}/mascot_vs.h ${MASCOT_HLSL}
    COMMAND ${FXC} /nologo /O3 /T ps_5_0 /E ps_main /Vn g_mascotPS /Fh ${SHADER_OUT}/mascot_ps.h ${MASCOT_HLSL}
    DEPENDS ${MASCOT_HLSL}
    COMMENT "Compiling mascot shaders"
    VERBATIM)

# ---- app ---------------------------------------------------------------------------------------
# Packaged (installer) builds turn on "Start with Windows" the first time they run. Local builds never
# touch the user's startup settings, whatever their configuration.
option(EFLOW_PACKAGED "Build for distribution" OFF)

add_executable(eflow WIN32
    src/main.cpp
    src/App.cpp
    src/core/Gfx.cpp
    src/core/Image.cpp
    src/core/Log.cpp
    src/island/IslandWindow.cpp
    src/mascot/Mascot.cpp
    src/mascot/MascotAsset.cpp
    src/mascot/MascotRenderer.cpp
    src/mascot/cgltf_impl.cpp
    src/services/MediaService.cpp
    src/app.manifest
    ${SHADER_OUT}/mascot_vs.h
    ${SHADER_OUT}/mascot_ps.h
)
target_include_directories(eflow PRIVATE src ${SHADER_OUT})
target_include_directories(eflow SYSTEM PRIVATE third_party/cgltf)
# C++/WinRT projection headers ship with the Windows SDK.
if(DEFINED ENV{WindowsSdkDir} AND DEFINED ENV{WindowsSDKVersion})
    target_include_directories(eflow SYSTEM PRIVATE "$ENV{WindowsSdkDir}Include/$ENV{WindowsSDKVersion}cppwinrt")
endif()
target_compile_definitions(eflow PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
if(EFLOW_PACKAGED)
    target_compile_definitions(eflow PRIVATE EFLOW_PACKAGED)
endif()
target_compile_options(eflow PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /external:W0)
# Third-party implementation file (its header is external, so /external:W0 silences it): CRT deprecation off.
set_source_files_properties(src/mascot/cgltf_impl.cpp PROPERTIES
    COMPILE_DEFINITIONS "_CRT_SECURE_NO_WARNINGS")
target_link_libraries(eflow PRIVATE d3d11 dxgi d2d1 dwrite dcomp dxguid windowscodecs windowsapp shell32 user32
    advapi32 shcore ole32)

# Runtime assets live next to the executable.
add_custom_command(TARGET eflow POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:eflow>/assets/mascot
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/mascot/eflow.glb $<TARGET_FILE_DIR:eflow>/assets/mascot/eflow.glb
    VERBATIM)
