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
set(DOMINO_TRT_RTX_ROOT "${PLAYER_DIR}/libs/tensorrt-rtx" CACHE PATH "TensorRT-RTX SDK root")
set(DOMINO_HAS_TRT_RTX FALSE)
if (EXISTS "${DOMINO_TRT_RTX_ROOT}/include/NvInfer.h" AND
    EXISTS "${DOMINO_TRT_RTX_ROOT}/lib/tensorrt_rtx_1_7.lib")
  set(DOMINO_HAS_TRT_RTX TRUE)
endif()

# ---- domino_core：可移植引擎库（无 D3D11/WASAPI 依赖，任意前端可链接） ----
add_library(domino_core STATIC
  src/config.cpp
  src/decode.cpp
  src/encode.cpp
  src/task.cpp
  src/sr.cpp
  src/trt_sr.cpp
  src/trt/gpu_interop.cpp
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
target_compile_definitions(domino_core PUBLIC
  "DOMINO_DEFAULT_ONNX_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/assets/models/sr/onnx/\"")
if (DOMINO_HAS_TRT_RTX)
  target_sources(domino_core PRIVATE src/trt/convert_avx2.cpp)
  if (MSVC)
    set_source_files_properties(src/trt/convert_avx2.cpp PROPERTIES
      COMPILE_OPTIONS "/arch:AVX2")
  endif()
  target_include_directories(domino_core PRIVATE
    ${DOMINO_TRT_RTX_ROOT}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/trt
  )
  target_link_libraries(domino_core PUBLIC
    ${DOMINO_TRT_RTX_ROOT}/lib/tensorrt_rtx_1_7.lib
    ${DOMINO_TRT_RTX_ROOT}/lib/tensorrt_onnxparser_rtx_1_7.lib
  )
  target_compile_definitions(domino_core PRIVATE DOMINO_HAS_TRT_RTX=1)
endif()

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
if (DOMINO_HAS_TRT_RTX)
  add_custom_command(TARGET domino_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${DOMINO_TRT_RTX_ROOT}/bin/tensorrt_rtx_1_7.dll
      ${DOMINO_TRT_RTX_ROOT}/bin/tensorrt_onnxparser_rtx_1_7.dll
      $<TARGET_FILE_DIR:domino_tests>
  )
endif()

# ---- SR 播放基准（手动运行，不加入 ctest） ----
if (DOMINO_HAS_TRT_RTX)
  add_executable(domino_trt_bench tests/trt_bench.cpp)
  target_include_directories(domino_trt_bench PRIVATE
    src
    ${FFMPEG_ROOT}/include
    ${NCNN_ROOT}/x64/include
  )
  target_link_libraries(domino_trt_bench PRIVATE domino_core)
  target_compile_definitions(domino_trt_bench PRIVATE NCNN_VULKAN)
  add_custom_command(TARGET domino_trt_bench POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${FFMPEG_ROOT}/bin/avcodec-63.dll
      ${FFMPEG_ROOT}/bin/avformat-63.dll
      ${FFMPEG_ROOT}/bin/avutil-61.dll
      ${FFMPEG_ROOT}/bin/swscale-10.dll
      ${FFMPEG_ROOT}/bin/swresample-7.dll
      ${NCNN_ROOT}/x64/bin/ncnn.dll
      ${DOMINO_TRT_RTX_ROOT}/bin/tensorrt_rtx_1_7.dll
      ${DOMINO_TRT_RTX_ROOT}/bin/tensorrt_onnxparser_rtx_1_7.dll
      $<TARGET_FILE_DIR:domino_trt_bench>
  )
endif()

add_executable(domino_sr_bench
  tests/sr_bench.cpp
  src/player.cpp
  src/audio.cpp
)
target_include_directories(domino_sr_bench PRIVATE
  src
  ${FFMPEG_ROOT}/include
  ${NCNN_ROOT}/x64/include
)
target_link_libraries(domino_sr_bench PRIVATE domino_core ole32.lib uuid.lib)
target_compile_definitions(domino_sr_bench PRIVATE NCNN_VULKAN)
target_compile_definitions(domino_sr_bench PRIVATE
  "DOMINO_DEFAULT_MODEL_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/assets/models/sr/ncnn/\"")
add_custom_command(TARGET domino_sr_bench POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${FFMPEG_ROOT}/bin/avcodec-63.dll
    ${FFMPEG_ROOT}/bin/avformat-63.dll
    ${FFMPEG_ROOT}/bin/avutil-61.dll
    ${FFMPEG_ROOT}/bin/swscale-10.dll
    ${FFMPEG_ROOT}/bin/swresample-7.dll
    ${NCNN_ROOT}/x64/bin/ncnn.dll
    $<TARGET_FILE_DIR:domino_sr_bench>
)
if (DOMINO_HAS_TRT_RTX)
  add_custom_command(TARGET domino_sr_bench POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${DOMINO_TRT_RTX_ROOT}/bin/tensorrt_rtx_1_7.dll
      ${DOMINO_TRT_RTX_ROOT}/bin/tensorrt_onnxparser_rtx_1_7.dll
      $<TARGET_FILE_DIR:domino_sr_bench>
  )
endif()

# 测试运行：assets 指向仓库测试视频目录，data 用系统临时目录
add_test(NAME domino_tests COMMAND domino_tests
  --assets ${CMAKE_CURRENT_SOURCE_DIR}/tests/assets
  --data $ENV{TEMP}/domino_tests)
set_tests_properties(domino_tests PROPERTIES TIMEOUT 180)
