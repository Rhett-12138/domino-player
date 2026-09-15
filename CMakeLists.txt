cmake_minimum_required(VERSION 3.20)
project(domino_player CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (MSVC)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /utf-8")
endif()

set(PLAYER_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(FFMPEG_ROOT ${PLAYER_DIR}/libs/ffmpeg/ffmpeg-master-latest-win64-gpl-shared)
set(NCNN_ROOT ${PLAYER_DIR}/libs/ncnn/ncnn-20260526-windows-vs2022-shared)

# ---- domino_core：可移植引擎库（无 D3D11/WASAPI 依赖，任意前端可链接） ----
add_library(domino_core STATIC
  src/config.cpp
  src/decode.cpp
  src/encode.cpp
  src/task.cpp
  src/sr.cpp
  src/frame_output.cpp
)
target_include_directories(domino_core PUBLIC src)
target_include_directories(domino_core PRIVATE
  ${FFMPEG_ROOT}/include
  ${NCNN_ROOT}/x64/include
)
target_link_libraries(domino_core PUBLIC
  ${FFMPEG_ROOT}/lib/avcodec.lib
  ${FFMPEG_ROOT}/lib/avformat.lib
  ${FFMPEG_ROOT}/lib/avutil.lib
  ${FFMPEG_ROOT}/lib/swscale.lib
  ${FFMPEG_ROOT}/lib/swresample.lib
  ${NCNN_ROOT}/x64/lib/ncnn.lib
)
target_compile_definitions(domino_core PUBLIC NCNN_VULKAN)
# 默认模型目录（构建时按仓库位置注入，避免源码硬编码路径）
target_compile_definitions(domino_core PUBLIC
  "DOMINO_DEFAULT_MODEL_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/assets/models/sr/ncnn/\"")

# ---- 单元测试（headless，不依赖窗口/渲染/音频） ----
enable_testing()
add_executable(domino_tests
  tests/test_main.cpp
  tests/test_config.cpp
  tests/test_decode.cpp
  tests/test_encode.cpp
  tests/test_task.cpp
  tests/test_player.cpp
  tests/test_beauty.cpp
  tests/test_frame_output.cpp
  tests/test_sr.cpp
  src/player.cpp
  src/audio.cpp
)

target_include_directories(domino_tests PRIVATE
  src
  ${FFMPEG_ROOT}/include
  ${NCNN_ROOT}/x64/include
)

target_link_libraries(domino_tests PRIVATE domino_core ole32.lib uuid.lib)

target_compile_definitions(domino_tests PRIVATE NCNN_VULKAN)

add_custom_command(TARGET domino_tests POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${FFMPEG_ROOT}/bin/avcodec-63.dll
    ${FFMPEG_ROOT}/bin/avformat-63.dll
    ${FFMPEG_ROOT}/bin/avutil-61.dll
    ${FFMPEG_ROOT}/bin/swscale-10.dll
    ${FFMPEG_ROOT}/bin/swresample-7.dll
    ${NCNN_ROOT}/x64/bin/ncnn.dll
    $<TARGET_FILE_DIR:domino_tests>
)

# 测试运行：assets 指向仓库测试视频目录，data 用系统临时目录
add_test(NAME domino_tests COMMAND domino_tests
  --assets ${CMAKE_CURRENT_SOURCE_DIR}/tests/assets
  --data $ENV{TEMP}/domino_tests)
set_tests_properties(domino_tests PROPERTIES TIMEOUT 180)
