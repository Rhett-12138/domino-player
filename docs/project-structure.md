# 项目结构

`domino-player`：跨平台视频超分 + 美颜播放器——**Flutter 前端**（当前 Windows 桌面）+ **可移植 C++ 引擎**（FFmpeg 解码 + NCNN Vulkan 超分 + `IFrameOutput`/`AudioSink` 抽象），含超分任务中心（离线编码/断点续编/边播放/转存）。

```
domino-player/
├── CMakeLists.txt        # domino_core（可移植引擎库）+ domino_tests（单测）+ CTest
├── README.md
├── CONTRIBUTING.md
├── AGENTS.md             # 项目宪章（目标/约束/DoD/已知坑）
├── src/                  # 可移植 C++ 引擎源码
│   ├── engine.h          # 引擎契约：IFrameOutput（视频输出抽象）
│   ├── audio_interface.h # AudioSink 接口（可移植；Windows=WASAPI，Linux/移动端可注入）
│   ├── config.{h,cpp}    # Config：路径可注入（模型目录/数据根），测试用临时目录
│   ├── log.h             # 轻量日志（stderr）
│   ├── decode.{h,cpp}    # MediaStream：视频/音频解码 + sws/swr + seek + 幂等 close
│   ├── sr.{h,cpp}        # SR：NCNN Vulkan 4x 超分封装
│   ├── encode.{h,cpp}    # 离线编码：fMP4 + h264 + 音频复制 + 进度/预览/取消 + 转存 + 断点合并
│   ├── task.{h,cpp}      # Task/TaskCenter：任务持久化、编码线程、断点续编、合并协调
│   ├── frame_output.{h,cpp}  # EngineFrameOutput：CPU 美颜 + 帧缓冲（前端上屏用）
│   ├── beauty_math.h     # 美颜/滤镜纯数学（可单测）
│   ├── ffmpeg_raii.h     # FFmpeg 资源 RAII 守卫
│   ├── player.{h,cpp}    # Player：播放核心（解码/超分/调度/A·V 同步/seek）——经 IFrameOutput 输出
│   │                     #   依赖 AudioSink 接口；默认工厂 createDefaultAudioSink() 见 audio.cpp
│   └── audio.{h,cpp}     # WasapiAudioOut：AudioSink 的 WASAPI 实现（Windows）
├── tests/                # C++ 单测（链接 domino_core + player + audio）+ GUI 回归脚本
│   ├── assets/           # 测试视频（test_av.mp4 2s / test_long.mp4 15s）
│   ├── gui/              # killall/rclick/hover/rdrag/capture/pix（Win32 窗口自动化，可用于 Flutter 窗口）
│   └── README.md
├── libs/                 # 依赖（ffmpeg/ncnn 本地放置，git 忽略）
├── flutter/              # Flutter 前端（跨平台；当前 Windows 桌面）
│   ├── lib/
│   │   ├── main.dart             # 首页（播放/任务中心切换）+ 播放页（Texture/控制条/美颜/滤镜缩略图/全屏）
│   │   ├── core/engine.dart      # EngineApi 抽象 + domino_engine.dll FFI 绑定
│   │   ├── core/format.dart      # 展示层纯函数（时间/状态名/滤镜名/RGB 转换）
│   │   └── ui/task_center.dart   # 任务中心（新建/开始/停止/续编/边播放/删除）
│   ├── native/           # C++ 引擎桥
│   │   ├── engine_bridge.{h,cpp}  # extern "C" 导出 + Flutter 像素纹理 + 文件框 + 全屏
│   │   └── CMakeLists.txt        # 构建 domino_engine.dll（链接 domino_core + player/audio + FFmpeg/NCNN + Flutter）
│   ├── windows/          # Windows 桌面 embedder（runner 注册 DominoRegisterPlugin；POST_BUILD 拷 DLL/模型）
│   └── test/             # Dart 测试（FakeEngine 桩，无需 DLL）
├── assets/models/sr/ncnn/
└── docs/                 # roadmap / project-structure
```

## 引擎/前端分层

```
┌─ 前端（可替换）──────────────────────────────┐
│  Flutter（当前，跨平台）/ 未来移动端           │
│  经 IFrameOutput 上屏 + FFI 驱动              │
└──────────────┬───────────────────────────────┘
               │ 引擎 API（engine.h + audio_interface.h）
┌──────────────▼───────────────────────────────┐
│  domino_core 库（可移植，无平台依赖）          │
│  decode / encode / task / sr / config         │
│  frame_output / beauty_math                   │
├───────────────────────────────────────────────┤
│  Player（+ AudioSink 实现）——各前端按需链接    │
│  视频经 IFrameOutput、音频经 AudioSink         │
└───────────────────────────────────────────────┘
```

- **`domino_core`**（无平台依赖）：解码/编码/超分/任务/断点续编/帧输出/美颜数学。
- **Player** 依赖 `AudioSink` 接口；默认工厂 `createDefaultAudioSink()` 在 `audio.cpp`（WASAPI），因此 Player 与平台音频实现一起由各前端链接。
- **视频输出**经 `IFrameOutput`：Flutter 用像素纹理（`EngineFrameOutput` → `FlutterTextureRegistrar`）。
- **音频**经 `AudioSink`：Windows=WASAPI；Linux/移动端注入实现（测试用哑 sink，不依赖 WASAPI）。
- 改桥/引擎后务必确认 runner 目录 `domino_engine.dll` 已更新（见 `AGENTS.md` §7 已知坑）。
- 测试策略见 `tests/README.md`。
