cmake_minimum_required(VERSION 3.14)
project(runner LANGUAGES CXX)

# Define the application target. To change its name, change BINARY_NAME in the
# top-level CMakeLists.txt, not the value here, or `flutter run` will no longer
# work.
#
# Any new source files that you add to the application should be added here.
add_executable(${BINARY_NAME} WIN32
  "flutter_window.cpp"
  "main.cpp"
  "utils.cpp"
  "win32_window.cpp"
  "${FLUTTER_MANAGED_DIR}/generated_plugin_registrant.cc"
  "Runner.rc"
  "runner.exe.manifest"
)

# Apply the standard set of build settings. This can be removed for applications
# that need different build settings.
apply_standard_settings(${BINARY_NAME})

# Add preprocessor definitions for the build version.
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION=\"${FLUTTER_VERSION}\"")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_MAJOR=${FLUTTER_VERSION_MAJOR}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_MINOR=${FLUTTER_VERSION_MINOR}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_PATCH=${FLUTTER_VERSION_PATCH}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_BUILD=${FLUTTER_VERSION_BUILD}")

# Disable Windows macros that collide with C++ standard library functions.
target_compile_definitions(${BINARY_NAME} PRIVATE "NOMINMAX")
target_compile_options(${BINARY_NAME} PRIVATE "/utf-8")

# Add dependency libraries and include directories. Add any application-specific
# dependencies here.
target_link_libraries(${BINARY_NAME} PRIVATE flutter flutter_wrapper_app)
target_link_libraries(${BINARY_NAME} PRIVATE domino_engine)
target_link_libraries(${BINARY_NAME} PRIVATE "dwmapi.lib")
target_include_directories(${BINARY_NAME} PRIVATE "${CMAKE_SOURCE_DIR}")

# Run the Flutter tool portions of the build. This must not be removed.
add_dependencies(${BINARY_NAME} flutter_assemble)

# 引擎桥 DLL + FFmpeg/NCNN 运行库 → exe 同目录（Dart FFI 从 exe 同目录加载 domino_engine.dll）。
# 放在 runner 目标内以便用 $<TARGET_FILE_DIR:${BINARY_NAME}>（native 子目录 add 时 runner 尚不存在）。
set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
set(FFMPEG_BIN "${REPO_ROOT}/libs/ffmpeg/ffmpeg-master-latest-win64-gpl-shared/bin")
set(NCNN_BIN "${REPO_ROOT}/libs/ncnn/ncnn-20260526-windows-vs2022-shared/x64/bin")
add_custom_command(TARGET ${BINARY_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:domino_engine>"
    ${FFMPEG_BIN}/avcodec-63.dll
    ${FFMPEG_BIN}/avformat-63.dll
    ${FFMPEG_BIN}/avutil-61.dll
    ${FFMPEG_BIN}/swscale-10.dll
    ${FFMPEG_BIN}/swresample-7.dll
    ${NCNN_BIN}/ncnn.dll
    "$<TARGET_FILE_DIR:${BINARY_NAME}>"
)
if (EXISTS "${REPO_ROOT}/libs/tensorrt-rtx/bin/tensorrt_rtx_1_7.dll")
  add_custom_command(TARGET ${BINARY_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${REPO_ROOT}/libs/tensorrt-rtx/bin/tensorrt_rtx_1_7.dll"
      "${REPO_ROOT}/libs/tensorrt-rtx/bin/tensorrt_onnxparser_rtx_1_7.dll"
      "$<TARGET_FILE_DIR:${BINARY_NAME}>"
  )
endif()
# 超分模型拷到 exe 同目录（assets/models/sr/ncnn），运行时不依赖仓库路径
add_custom_command(TARGET ${BINARY_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${REPO_ROOT}/assets/models"
    "$<TARGET_FILE_DIR:${BINARY_NAME}>/assets/models"
)
# TensorRT-RTX 许可/致谢（NVIDIA 运行时随包分发时需要）
add_custom_command(TARGET ${BINARY_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${REPO_ROOT}/assets/third_party"
    "$<TARGET_FILE_DIR:${BINARY_NAME}>/assets/third_party"
)
