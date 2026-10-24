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
- **GPU 美颜用共享纹理**：`GpuFrameOutput` 在 `DominoRegisterPlugin` 里先建 D3D11 设备；Flutter 用 `kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle` + `IDXGIResource::GetSharedHandle()`。输出纹理必须是 BGRA、`D3D11_RESOURCE_MISC_SHARED`，并等 GPU 完成（query）后再 MarkExternalTexture。
- **D3D11 纹理描述必须初始化 `Format`**：`D3D11_TEXTURE2D_DESC` 默认 `DXGI_FORMAT_UNKNOWN`（0），漏设会返回 `E_INVALIDARG (0x80070057)`；`d3d11.h` + Windows 头下还有 `small` 等宏，成员名避开（用 `smallRgb`），并给桥加 `NOMINMAX`。
- **GPU surface 生命周期**：共享纹理用 3 缓冲 + `release_callback` 引用计数；`acquireDescriptor` 里 AddRef，release 时释放；Flutter 未释放全部纹理时丢帧，不能覆盖。失败/无 D3D11 时自动回退 CPU（可用 `DOMINO_CPU_BEAUTY=1` 强制）。
- **SR/解码在 VideoWorker**：`Player::videoLoop` 线程独占 `vid_`/`sr_`/解码与 SR 缓冲；UI `prefetch` 只从最多 2 帧的队列取帧。禁止在 UI 线程调用 `vid_.seek/nextVideoFrame` 或 `sr_.process`。
- **seek 要带 generation**：UI 递增 `seekGen_`、清队列并请求 worker seek；worker 在解码/SR 后校验 generation，丢弃在途旧帧。Ticker 停摆用 `dropUntilPts` 让 worker **只解码、不跑 SR**追帧；持续慢于实时不能丢帧，否则永不呈现。
- **SR 失败回退**：`srFailed_` atomic；初始化/推理失败回退源帧，不每帧重试；`setNoSR(false)` 可重试。模型加载也应在 worker，不要阻塞 UI。
- **SR 自动降档**：`maybeAutoDegrade` 每 ~200ms 看 `currentMediaClock - pendingPts/playhead`；lag>0.30s 持续 1s 后，4K/2K→1080p，1080p→关闭，设置 `dropUntilPts` 追到当前时钟并冷却 4s。不要在 Ticker 停摆时触发；手动选档必须清 `srAutoDegraded_` 并重试。
- **离线 SR 质量优先**：`encodeOffline` 默认 `SRInputQuality::FullSource`，输入宽 `max(target/4, min(srcW,1920))`，**不要**改回固定 `target/4`；4x 输出用 `SWS_LANCZOS` 缩到目标。编码默认 CRF18 + preset slow，但必须保持 `max_b_frames=0 + tune=zerolatency`（fMP4/断点续编 A/V 约束）。1080p→4K 中间帧 7680×4320、峰值约 1.9GB；SR 失败必须回退源帧并最终判任务失败，不能 `return` 丢帧。后续用 tile。
- **任务质量档要持久化**：`Task::srInput` 写入 `meta.tsv` 第二行 `sr_input=0/1/2`；旧 meta 无字段按 Fast（旧 target/4）兼容；`TaskCenter::start` 必须把它传给 `EncodeOptions`，续编不能换策略。质量下拉：快速/平衡/高质量（默认 FullSource）。
- **内容类型/模型也要持久化**：`Task::model`（`Realistic/Anime/RealisticHQ/AnimeHQ/AnimeX2/AnimeX3`）写入 `meta.tsv` 第三行 `model=...`；旧 meta 默认 Realistic。模型路径/scale 用 `srModelParamName/BinName/Scale`，不要硬编码；任务续编必须沿用。动画标准按 `target/src` 自动选 x2/x3/x4；真人暂只有 x4。HQ 模型（x4plus/x4plus-anime）输入上限 960，超过会 OOM/推理失败。
- **SR 基准**：`domino_sr_bench`（不加入 ctest）测直接推理、Player 端到端和 Fast/Balanced/FullSource 离线编码；改动 SR/呈现后重跑，数据写入 `docs/plan-sr-playback-2026-10.md` / `plan-sr-worker-2026-10.md` / `plan-offline-sr-quality-2026-10.md`。
- **离线 SR 流水线必须走 finishSr**：`needSR` 时 SR 推理和后处理各有一个 worker，所有出口（正常 EOF、取消、writeError）都必须 `finishSr(...)` 后 **分别 join infer/post 两个线程**；推理/后处理队列都会额外占用 SR 输出缓冲，4K FullSource 需实机确认峰值内存。NCNN 快路径可用 `DOMINO_NO_VULKAN_FASTPATH=1` 关闭。
- **TensorRT-RTX 是可选后端**：便携包在 `libs/tensorrt-rtx/`（git 忽略），不要求 CUDA Toolkit；`DOMINO_NO_TRT=1` 强制 NCNN。标准/HQ 模型用 `assets/models/sr/onnx/`，动画与 2x/3x 仍走 NCNN。engine 缓存在 `%APPDATA%/DominoPlayer/trt-rtx`，首次构建约 12–20s；初始化失败回退 NCNN，推理失败仍回退源帧并判任务失败。此代码含 Windows `nvcuda.dll`，只应在 Windows 可选构建中启用。
- **续编分段不能乱删**：续编取消/失败时保留 `task.mp4.resume`，下次启动先合并再继续；替换主文件必须先备份、失败回滚，禁止静默忽略 `remove/rename` 错误。
  - `.resume.merging` = 合并中的续编段；`.resume.cont` = 续编段标记（主文件丢失时禁止当首段提升）；`.bak` = 主文件替换备份，启动时先恢复。
  - 合并幂等按“分段**末**帧是否已被主文件覆盖”判断；不要用首帧（检查点可能落后约 1s，首帧早于主文件末尾是合法重叠）。
  - 合并/替换前若播放器正打开产物，桥层会先 `Player::close()`；外部程序占用时保留分段并报失败，关闭后重试。

## 8. 协作模式（角色框架）

- **driver（主 agent）**：读本文件 + `docs/roadmap.md` → 拆解（必要时写 `docs/plan-*.md`）→ 实现 → 跑 DoD。
- **reviewer（子 agent）**：改动涉及引擎/桥/并发/FFI、或 >100 行、或提交前，调用它做只读检视 + 跑测试，按其结论修正。见 `.opencode/agent/reviewer.md`（该目录仅本地保留，不随仓库发布）。
- 交接用**文件**（plan/review），不要只靠口头。
- **north star 是 `docs/roadmap.md`**：每次开工先读，收工更新状态。
