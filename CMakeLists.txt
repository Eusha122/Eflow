cmake_minimum_required(VERSION 3.24)
project(Eflow LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# The C++ runtime is linked in: a clean Windows PC has no Visual C++ Redistributable (MSVCP140.dll), and without it
# eflow.exe would not even start.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

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
    src/AppShoot.cpp
    src/core/Gfx.cpp
    src/core/Image.cpp
    src/core/Json.cpp
    src/core/Log.cpp
    src/core/PackageIdentity.cpp
    src/core/When.cpp
    src/core/Firewall.cpp
    src/core/Download.cpp
    src/island/IslandWindow.cpp
    src/island/CaptureOverlay.cpp
    src/island/MouseHook.cpp
    src/island/RemindersView.cpp
    src/island/SafeShareView.cpp
    src/island/TourView.cpp
    src/mascot/Mascot.cpp
    src/mascot/MascotAsset.cpp
    src/mascot/MascotRenderer.cpp
    src/mascot/cgltf_impl.cpp
    src/services/AudioDevices.cpp
    src/services/CameraService.cpp
    src/services/DeviceReadiness.cpp
    src/services/DeviceWatcher.cpp
    src/services/HookInstaller.cpp
    src/services/CodexTrust.cpp
    src/services/LyricsService.cpp
    src/services/MediaService.cpp
    src/services/NotificationService.cpp
    src/services/NotificationMutes.cpp
    src/services/PermissionServer.cpp
    src/services/SoluService.cpp
    src/services/SoluSetup.cpp
    src/services/ActivityMonitor.cpp
    src/services/AgentMonitor.cpp
    src/services/ShelfStore.cpp
    src/services/ScreenshotCatcher.cpp
    src/core/LoginCode.cpp
    src/core/Clipboard.cpp
    src/services/ShareCrypto.cpp
    src/services/SafeShareService.cpp
    src/services/ShareActivity.cpp
    src/services/PhoneShare.cpp
    src/services/PhoneSessions.cpp
    src/core/Https.cpp
    src/core/Protect.cpp
    src/services/ReminderStore.cpp
    src/services/AlarmSound.cpp
    src/services/SystemMonitor.cpp
    src/app.manifest
    src/app.rc
    ${SHADER_OUT}/mascot_vs.h
    ${SHADER_OUT}/mascot_ps.h
)
target_include_directories(eflow PRIVATE src ${SHADER_OUT})
target_include_directories(eflow SYSTEM PRIVATE third_party/cgltf)
# QR codes for the phone link: Project Nayuki's generator, MIT licence (third_party/qrcodegen/LICENSE), v1.8.0, unchanged.
add_library(qrcodegen STATIC third_party/qrcodegen/qrcodegen.cpp)
target_include_directories(qrcodegen SYSTEM PUBLIC third_party/qrcodegen)
target_compile_options(qrcodegen PRIVATE /utf-8 /EHsc)
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
    advapi32 shcore ole32 oleaut32 pdh iphlpapi wbemuuid setupapi ws2_32 winhttp winmm cfgmgr32 bcrypt crypt32 comctl32 qrcodegen)

# The PermissionRequest hook command run by Claude Code / Codex. Built next to eflow.exe; Eflow copies it to
# ~/.eflow/hook, the path the tools run (it verifies that the pipe it talks to is served by Eflow).
add_executable(eflow-hook src/hook/main.cpp)
target_include_directories(eflow-hook PRIVATE src)
target_compile_definitions(eflow-hook PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(eflow-hook PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(eflow-hook PRIVATE advapi32)
add_dependencies(eflow eflow-hook)

# Unit tests for the parts that need no window (run build\<config>\eflow-tests.exe).
add_executable(eflow-tests tests/when_test.cpp tests/login_code_test.cpp tests/share_test.cpp
    src/core/When.cpp src/core/LoginCode.cpp src/core/Json.cpp src/services/ShareCrypto.cpp src/services/ShareActivity.cpp
    src/services/PhoneSessions.cpp)
target_link_libraries(eflow-tests PRIVATE bcrypt)
target_include_directories(eflow-tests PRIVATE src)
target_compile_definitions(eflow-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(eflow-tests PRIVATE /W4 /permissive- /utf-8 /EHsc)

# Runtime assets live next to the executable.
add_custom_command(TARGET eflow POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:eflow>/assets/mascot
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/mascot/eflow.glb $<TARGET_FILE_DIR:eflow>/assets/mascot/eflow.glb
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:eflow>/assets/agents
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/agents/claude.png $<TARGET_FILE_DIR:eflow>/assets/agents/claude.png
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/agents/codex.png $<TARGET_FILE_DIR:eflow>/assets/agents/codex.png
    # The package's logos: a sparse package reads them from its external location (this folder), not package/.
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/package/assets $<TARGET_FILE_DIR:eflow>/assets
    VERBATIM)
