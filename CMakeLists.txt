cmake_minimum_required(VERSION 3.20)
project(instant_replay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/third_party/ffmpeg" CACHE PATH "FFmpeg shared build root")

if(NOT EXISTS "${FFMPEG_ROOT}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR
        "FFmpeg not found at ${FFMPEG_ROOT}. "
        "Download BtbN win64 lgpl-shared build into third_party/ffmpeg "
        "(expected include/, lib/, bin/). Prefer lgpl-shared over gpl-shared for distribution.")
endif()

set(FFMPEG_RUNTIME_DLLS
    avcodec-62.dll
    avformat-62.dll
    avutil-60.dll
    swresample-6.dll
    swscale-9.dll
)

# Core app (lives in bin\ next to FFmpeg DLLs).
add_executable(instant_replay WIN32
    src/main.cpp
    src/app.rc
    src/util/log.cpp
    src/util/paths.cpp
    src/util/settings.cpp
    src/util/devices.cpp
    src/util/capability_probe.cpp
    src/input/hotkey.cpp
    src/capture/desktop_duplicator.cpp
    src/capture/gpu_nv12_converter.cpp
    src/capture/capture_session.cpp
    src/audio/wasapi_loopback.cpp
    src/encode/nvenc_encoder.cpp
    src/encode/aac_encoder.cpp
    src/buffer/encoded_ring_buffer.cpp
    src/buffer/audio_ring_buffer.cpp
    src/export/mp4_exporter.cpp
    src/export/region_clip_exporter.cpp
    src/gui/gdi_utils.cpp
    src/gui/clip_list.cpp
    src/gui/clip_thumbnail.cpp
    src/gui/region_selector.cpp
    src/gui/app_window.cpp
)

set_target_properties(instant_replay PROPERTIES OUTPUT_NAME "UniyaOverCore")

target_include_directories(instant_replay PRIVATE
    src
    "${FFMPEG_ROOT}/include"
)

target_link_directories(instant_replay PRIVATE "${FFMPEG_ROOT}/lib")

target_link_libraries(instant_replay PRIVATE
    user32
    gdi32
    gdiplus
    shell32
    ole32
    propsys
    d3d11
    dxgi
    comctl32
    avcodec
    avformat
    avutil
    swscale
    swresample
)

if(MSVC)
    target_compile_options(instant_replay PRIVATE /W4 /permissive- /utf-8)
endif()

foreach(dll ${FFMPEG_RUNTIME_DLLS})
    add_custom_command(TARGET instant_replay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:instant_replay>/bin
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFMPEG_ROOT}/bin/${dll}"
            $<TARGET_FILE_DIR:instant_replay>/bin/${dll}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:instant_replay>
            $<TARGET_FILE_DIR:instant_replay>/bin/$<TARGET_FILE_NAME:instant_replay>
        COMMENT "Stage bin/ runtime for ${dll}"
    )
endforeach()

# Root launcher (UniyaOver.exe) → bin\UniyaOverCore.exe
add_executable(uniyaover_launcher WIN32
    src/launcher/main.cpp
    src/launcher/launcher.rc
)
set_target_properties(uniyaover_launcher PROPERTIES OUTPUT_NAME "UniyaOver")
target_include_directories(uniyaover_launcher PRIVATE src)
target_link_libraries(uniyaover_launcher PRIVATE user32 shell32)
add_dependencies(uniyaover_launcher instant_replay)
if(MSVC)
    target_compile_options(uniyaover_launcher PRIVATE /W4 /permissive- /utf-8)
endif()

# Friend / store-ready folder layout.
add_custom_command(TARGET uniyaover_launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/dist/UniyaOver/bin"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/dist/UniyaOver/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:uniyaover_launcher>
        "${CMAKE_SOURCE_DIR}/dist/UniyaOver/UniyaOver.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:instant_replay>
        "${CMAKE_SOURCE_DIR}/dist/UniyaOver/bin/UniyaOverCore.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/assets/uniyaover.ico"
        "${CMAKE_SOURCE_DIR}/dist/UniyaOver/assets/uniyaover.ico"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/dist/README.txt"
        "${CMAKE_SOURCE_DIR}/dist/UniyaOver/README.txt"
    COMMENT "Staging dist/UniyaOver layout"
)
foreach(dll ${FFMPEG_RUNTIME_DLLS})
    add_custom_command(TARGET uniyaover_launcher POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFMPEG_ROOT}/bin/${dll}"
            "${CMAKE_SOURCE_DIR}/dist/UniyaOver/bin/${dll}"
    )
endforeach()

# Minimal custom installer (Riot-inspired UI). SFX payload assembled by pack_dist.ps1.
add_executable(uniyaover_setup WIN32
    src/setup/main.cpp
    src/setup/setup.rc
    src/gui/gdi_utils.cpp
)
set_target_properties(uniyaover_setup PROPERTIES OUTPUT_NAME "UniyaOverSetup")
target_include_directories(uniyaover_setup PRIVATE src)
target_link_libraries(uniyaover_setup PRIVATE
    user32 gdi32 gdiplus shell32 ole32 uuid oleaut32
)
if(MSVC)
    target_compile_options(uniyaover_setup PRIVATE /W4 /permissive- /utf-8)
    target_link_options(uniyaover_setup PRIVATE
        "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
endif()
