# AGENTS.md — Domino Player 项目宪章

> 所有 agent 开工前必读。**核心目标与约束不得偏离**；若需求与之冲突，先停下确认。

## 1. 核心目标（North Star）

做一个**跨平台的视频超分 + 美颜播放器**：Flutter 前端（先 Windows，后移动端）+ **可移植的 C++ 引擎**。

- 实时/离线**模型超分**（NCNN Vulkan，RealESRGAN 4x）+ 基础美颜（磨皮/美白/滤镜）。
- 超分**任务中心**：离线编码、断点续编、边播放（直播）、转存。
- 引擎与前端解耦：引擎不含任何平台依赖，可被任意前端链接。

## 2. 硬约束

- **引擎可移植**：`domino_core`（config/decode/encode/task/sr/frame_output）不得依赖 D3D11/WASAPI；`Player` 只依赖 `AudioSink` 接口（默认 WASAPI 工厂在 `audio.cpp`，由各前端链接）；平台相关只能放前端或 `flutter/native` 桥。
- **视频输出**一律经 `IFrameOutput`；**音频**一律经 `AudioSink`（Windows=WASAPI，其他平台注入实现）。
- **不随意新增依赖**：Flutter 插件优先用 FFI 自实现（如系统文件框 `IFileOpenDialog`）；确需引入先说明理由。
- 依赖预编译在 `libs/`（ffmpeg/ncnn 本地放置、git 忽略）。模型在 `assets/models/sr/ncnn/`。
- 开发环境：Windows + VS2022 + CMake；Flutter SDK 见 `flutter/`。

## 3. 架构地图

```
Flutter 前端 (flutter/lib)  ──FFI──▶  domino_engine.dll (flutter/native/engine_bridge.cpp)
   main.dart 播放页                     │ 链接
   core/engine.dart FFI 绑定            ▼
   core/format.dart 纯函数        domino_core + Player + AudioSink
   ui/task_center.dart            (src/, 可移植引擎)
```

关键文件：
- 引擎契约：`src/engine.h`（IFrameOutput）、`src/audio_interface.h`（AudioSink）
- 播放核心：`src/player.{h,cpp}`；帧输出/美颜：`src/frame_output.{h,cpp}` + `src/beauty_math.h`
- 任务：`src/task.{h,cpp}`；离线编码：`src/encode.{h,cpp}`；超分：`src/sr.{h,cpp}`
- 桥与导出：`flutter/native/engine_bridge.{h,cpp}`
- Dart：`flutter/lib/core/engine.dart`（FFI）、`flutter/lib/main.dart`、`flutter/lib/ui/task_center.dart`
- 测试：`tests/`（C++）、`flutter/test/`（Dart）
- 文档：`docs/roadmap.md`（当前状态/里程碑）、`docs/project-structure.md`

## 4. 构建 / 运行 / 测试

```bash
# C++ 引擎 + 单测（Windows，MSVC）
cmake -S . -B build
cmake --build build --target domino_tests
ctest --test-dir build -C Release --output-on-failure      # ~5s，应 0 failed

# Flutter 前端
cd flutter
flutter test                       # 纯 Dart，无需 DLL
flutter build windows --debug      # 产物 build/windows/x64/runner/Debug/domino_flutter.exe
flutter run -d windows             # 开发运行（热重载）
```

## 5. 完成定义（DoD）

一个改动算"完成"必须**全部**满足：

1. `ctest` 全过（涉及引擎/桥时），且新增逻辑有测试。
2. `cd flutter && flutter test` 全过（涉及前端时）。
3. `flutter build windows --debug` 成功（涉及桥/前端时）。
4. **实机冒烟**：涉及播放/桥/引擎的改动，必须启动 `domino_flutter.exe` 确认——播放正常、关键交互生效（不能只看"进程存活"）。
5. 相关文档更新（`docs/`、`tests/README.md`）。
6. 提交信息风格见 `git log`（`feat:` / `fix:` / `refactor:` / `perf:` / `test:` + 中文要点）。

## 6. 约定

- 注释用中文；**不加无关注释**；遵循既有风格（Dart 用 `EngineApi` 注入 `FakeEngine` 测试）。
- C++ 测试用 `tests/test_util.h` 轻量框架（`TEST`/`EXPECT_*`）；Flutter 测试用 `FakeEngine` 桩。
- 引擎改动优先补 headless 测试（可注入哑 `AudioSink`，不依赖 WASAPI）。

## 7. 已知坑（勿重蹈）

- **构建产物必须真正更新**：曾因 `flutter/native/CMakeLists.txt` 的 `if (TARGET ${BINARY_NAME})` 在 `add_subdirectory` 顺序下恒假，`domino_engine.dll` 从未拷到 runner 目录 → app 跑旧 DLL → 缺符号 → `_pump` 每帧抛异常 → UI 冻结。改桥/引擎后**务必确认 runner 目录 DLL 时间戳已更新**。
- Flutter 引擎头是**平铺**包含（`<flutter_export.h>`，非 `flutter/` 前缀）；导出需 `__declspec(dllexport)`（`DOMINO_EXPORT`）；runner 需 `/utf-8`。
- `add_custom_command(TARGET ...)` 只能在同一目录；`$<CONFIG>` 在 VS 的 POST_BUILD 里不展开。
- 播放冻结/无反应先查：`_pump` 是否抛异常（FFI 符号缺失）、DLL 是否最新、`_opening` 是否卡住。

## 8. 协作模式（角色框架）

- **driver（主 agent）**：读本文件 + `docs/roadmap.md` → 拆解（必要时写 `docs/plan-*.md`）→ 实现 → 跑 DoD。
- **reviewer（子 agent）**：改动涉及引擎/桥/并发/FFI、或 >100 行、或提交前，调用它做只读检视 + 跑测试，按其结论修正。见 `.opencode/agent/reviewer.md`（该目录仅本地保留，不随仓库发布）。
- 交接用**文件**（plan/review），不要只靠口头。
- **north star 是 `docs/roadmap.md`**：每次开工先读，收工更新状态。
