# domino_engine：C++ 引擎桥（FFI 导出 + Flutter 像素纹理）
# 由 flutter/windows/CMakeLists.txt 以 add_subdirectory(../native) 引入；
# 链接仓库 src/（domino_core + player + audio）与 FFmpeg/NCNN。
cmake_minimum_required(VERSION 3.14)
project(domino_engine LANGUAGES CXX)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(ENGINE_SRC "${REPO_ROOT}/src")
set(FFMPEG_ROOT "${REPO_ROOT}/libs/ffmpeg/ffmpeg-master-latest-win64-gpl-shared")
set(NCNN_ROOT "${REPO_ROOT}/libs/ncnn/ncnn-20260526-windows-vs2022-shared")

add_library(domino_engine SHARED
  engine_bridge.cpp
  gpu_frame_output.cpp
  ${ENGINE_SRC}/config.cpp
  ${ENGINE_SRC}/decode.cpp
  ${ENGINE_SRC}/encode.cpp
  ${ENGINE_SRC}/task.cpp
  ${ENGINE_SRC}/sr.cpp
  ${ENGINE_SRC}/frame_output.cpp
  ${ENGINE_SRC}/player.cpp
  ${ENGINE_SRC}/audio.cpp
)

target_include_directories(domino_engine PRIVATE
  ${ENGINE_SRC}
  ${FFMPEG_ROOT}/include
  ${NCNN_ROOT}/x64/include
)

# flutter 目标（接口库）：提供 Flutter 引擎头（texture registrar 等）与 flutter_windows.lib
target_link_libraries(domino_engine PRIVATE
  flutter
  ${FFMPEG_ROOT}/lib/avcodec.lib
  ${FFMPEG_ROOT}/lib/avformat.lib
  ${FFMPEG_ROOT}/lib/avutil.lib
  ${FFMPEG_ROOT}/lib/swscale.lib
  ${FFMPEG_ROOT}/lib/swresample.lib
  ${NCNN_ROOT}/x64/lib/ncnn.lib
  ole32.lib
  shell32.lib
  d3d11.lib
  dxgi.lib
  d3dcompiler.lib
)

target_compile_definitions(domino_engine PRIVATE NCNN_VULKAN NOMINMAX)
target_compile_definitions(domino_engine PRIVATE
  "DOMINO_DEFAULT_MODEL_DIR=\"${REPO_ROOT}/assets/models/sr/ncnn/\"")
target_compile_features(domino_engine PRIVATE cxx_std_17)
if (MSVC)
  target_compile_options(domino_engine PRIVATE /utf-8)
endif()

# 拷贝 FFmpeg/NCNN 运行库到输出目录（与 DLL/exe 同目录）
add_custom_command(TARGET domino_engine POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${FFMPEG_ROOT}/bin/avcodec-63.dll
    ${FFMPEG_ROOT}/bin/avformat-63.dll
    ${FFMPEG_ROOT}/bin/avutil-61.dll
    ${FFMPEG_ROOT}/bin/swscale-10.dll
    ${FFMPEG_ROOT}/bin/swresample-7.dll
    ${NCNN_ROOT}/x64/bin/ncnn.dll
    $<TARGET_FILE_DIR:domino_engine>
)
# 拷贝到 runner 目录的命令见 runner/CMakeLists.txt（runner 目标存在后才能用 $<TARGET_FILE_DIR>）。
