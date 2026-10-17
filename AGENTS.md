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
- **提交前扫敏感信息**：勿把个人绝对路径（`C:\Users\<你>\…`）、密钥/令牌/密码、个人文件路径写进源码或文档。提交前用 `git status` / `git diff` 自检，并扫描：`git grep -n -E "C:/Users|C:\\\\Users|C:/Repos|api[_-]?key|secret|token|password"`。**历史里的敏感信息删提交也删不掉**（需改写历史 + 强推，或删库重建），发布前务必确认。配置/密钥优先走环境变量（如 `DOMINO_MODEL_DIR`）或本地未跟踪文件（`.gitignore`）。
- **模型目录末尾分隔符**：前端注入的 `model_dir` 可能不带 `\`；一律用 `joinModelPath()`（`src/config.h`）拼接，禁止 `base + "文件名"`。
- **`TaskCenter::start` 必须先 join 旧线程**：编码线程自然结束后仍 joinable，直接 `t->thread = std::thread(...)` 会 `std::terminate`。失败任务重试是真实触发路径。
- **暂停恢复必须重置呈现基准**：`setPaused(false)` 要 `resetClockAnchors()`，否则墙钟/音频时钟锚点在暂停期间失效，恢复后会跳变或快进。
- **GPU 实例只由桥层销毁**：`Player`/`encodeOffline` 不得调用 `ncnn::destroy_gpu_instance()`；`domino_destroy` 先 `tasks->shutdown()`、再 `player->shutdown()`，最后统一销毁，避免与并行 SR 任务冲突。
- **音频时钟单位**：`IAudioClock::GetPosition` 的单位由 `GetFrequency()` 定义，**不能直接除以采样率**；否则会出现视频 2x/0.5x 播放。
- **播放呈现按 PTS**：`Player::readyToPresent` 用音频时钟（不可用时回退墙钟）对比 `pendingPts_`；暂停/seek/恢复必须 `resetClockAnchors()`，否则会跳变或快进。
- **编码不要开 B 帧 / 关前瞻**：B 帧让首个 DTS 为负，fMP4 会把视频起点整体后移约 2 帧（约 66ms）；x264 前瞻会让视频包晚于音频输出，muxer 把视频整体后移。离线编码设 `max_b_frames=0` + `tune=zerolatency`。
- **音频不能领先视频太多**：编码循环里音频包要进队列，最多领先视频 100ms 再写，否则 fMP4 交错会把后续视频 DTS 整体推后。
- **muxer time_base 会变**：`avformat_write_header` 后音频 stream 可能从 1/90000 变为 1/44100；所有时间偏移（如源起点归一）必须在 header 之后按最终 time_base 计算。
- **.ts 的 AAC 需要 `aac_adtstoasc`**：直接复制 ADTS 到 mp4 会 malformed；用 BSF，且 BSF 输出要按 `time_base_out` 再转回输出流时间基。
- **非 0 起点归一**：视频 PTS 从 0 重建时，音频包也要减去同一 `videoStart`（并且丢掉落点在视频起点之前的音频），否则 .ts（start_time≈2.8s）会有固定唇音错位。
- **解码 send EAGAIN 不能丢包**：`avcodec_send_packet/frame` 返回 EAGAIN 时必须先 receive 再重试，否则 29.97 等含重排的片源会少帧。
- **续编分段不能乱删**：续编取消/失败时保留 `task.mp4.resume`，下次启动先合并再继续；替换主文件必须先备份、失败回滚，禁止静默忽略 `remove/rename` 错误。
  - `.resume.merging` = 合并中的续编段；`.resume.cont` = 续编段标记（主文件丢失时禁止当首段提升）；`.bak` = 主文件替换备份，启动时先恢复。
  - 合并幂等按“分段**末**帧是否已被主文件覆盖”判断；不要用首帧（检查点可能落后约 1s，首帧早于主文件末尾是合法重叠）。
  - 合并/替换前若播放器正打开产物，桥层会先 `Player::close()`；外部程序占用时保留分段并报失败，关闭后重试。

## 8. 协作模式（角色框架）

- **driver（主 agent）**：读本文件 + `docs/roadmap.md` → 拆解（必要时写 `docs/plan-*.md`）→ 实现 → 跑 DoD。
- **reviewer（子 agent）**：改动涉及引擎/桥/并发/FFI、或 >100 行、或提交前，调用它做只读检视 + 跑测试，按其结论修正。见 `.opencode/agent/reviewer.md`（该目录仅本地保留，不随仓库发布）。
- 交接用**文件**（plan/review），不要只靠口头。
- **north star 是 `docs/roadmap.md`**：每次开工先读，收工更新状态。
