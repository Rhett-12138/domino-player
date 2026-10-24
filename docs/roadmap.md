# Roadmap（North Star）

> 每次开工先读本文件；收工更新状态。目标与约束见 `AGENTS.md`。
> 历史规划（Qt 时代）见 `docs/native-player-roadmap.md`，已作废。

## 当前阶段：Flutter 前端（Windows 桌面）

目标：前端以 Flutter 为主（跨平台铺垫），引擎保持可移植，功能不缩水。

### 已完成
- [x] 引擎抽取：`domino_core` 可移植库 + `IFrameOutput` 视频输出抽象
- [x] 音频抽象：`AudioSink` 接口 + WASAPI 实现，Player 与平台解耦
- [x] Flutter PoC：视频经引擎 → `FlutterTextureRegistrar` 像素纹理播放
- [x] 任务中心：Dart UI（新建/开始/停止/续编/边播放/删除）+ 任务 FFI
- [x] 播放页：控制条（播放/暂停/seek/音量/循环）、磨皮美白滑杆、滤镜缩略图、FPS 角标
- [x] 系统文件选择框（`IFileOpenDialog`，FFI 自实现，无新依赖）
- [x] 播放链路调优：美颜移出锁、无美颜快路径、RGBA 双缓冲 + 快速转换
- [x] 测试补齐：C++（decode/encode/task/player/config/beauty/frame_output/sr）+ Flutter（format/widget，FakeEngine 桩）
- [x] 修复：`domino_engine.dll` 未拷到 runner 导致 UI 冻结；`Player::eof_` 从未赋值；`MediaStream::close` 未复位
- [x] 交互修复：播放页常驻（切标签不重开默认视频/不丢进度/不残留音频）、动态宽高比、路径框回车打开、任务中心浏览/删除确认/状态色、缩略图按需生成
- [x] 修复：任务中心完成时 UI 不刷新（卡在最后一帧）——改为任何状态/进度变化都刷新；启动不自动打开视频（占位提示 + 手动打开）；循环默认关闭
- [x] 美颜/滤镜改为呼出式浮层（控制条按钮切换，不占布局）；修复滤镜缩略图不显示（RGB24→RGBA 索引错位 + 尾部透明）
- [x] 性能/体验：1080p 开滤镜不再掉帧（仅滤镜时跳过肤色判定 + 多线程分块）；帧驱动改 Ticker（vsync 对齐更平滑）；移除常显路径栏（改为控制条文件名）；新增全屏（桥层 SetWindowPos 顶层窗口）
- [x] 清理 ImGui 旧前端（`src/main.cpp`、`src/ui/`、`src/render.*`、`libs/imgui`）
- [x] 稳定化（2026-10，见 `docs/plan-bugfix-2026-10.md`）：模型路径拼接、失败任务重试崩溃、暂停恢复快进、续编取消丢分段、GPU 实例生命周期、WASAPI/解码资源释放
- [x] A/V 同步与 PTS（2026-10，见 `docs/plan-av-sync-2026-10.md`）：音频时钟驱动视频、按 PTS 呈现、音频 seek 线程安全、编码保留 29.97/23.976 精确帧率、关闭 B 帧消除 66ms 起点偏移
- [x] GPU 着色器美颜（2026-10，见 `docs/plan-gpu-beauty-2026-10.md`）：D3D11 HLSL 两遍高斯 + 磨皮/美白/滤镜，Flutter GPU surface 上屏；CPU 路径保留为回退
- [x] 实时 SR 评估（2026-10，见 `docs/plan-sr-playback-2026-10.md`）：播放页 SR 关闭/1080p/2K/4K 档位 + `domino_sr_bench`；实测 1080p App 21fps、2K 12fps、4K 5.5fps，结论是同步 UI 线程路径不达标
- [x] SR/解码移出 UI 线程（2026-10，见 `docs/plan-sr-worker-2026-10.md`）：VideoWorker + 2 帧队列 + seek generation + 停摆跳帧；1080p App 提升到 29.6fps
- [x] SR 自动降档（2026-10，见 `docs/plan-auto-degrade-2026-10.md`）：持续 A/V 落后时 4K/2K→1080p，1080p 仍落后→关闭；实测 4K 约 2s 后回到 29.98fps，手动 1080p 不降档
- [x] 离线 SR 质量优先（2026-10，见 `docs/plan-offline-sr-quality-2026-10.md`）：离线默认 FullSource 输入（不再 target/4 预降采样），4x 后用 Lanczos 缩到目标；CRF18/preset slow。720p→1080 FullSource 15.3s/2s，1080p→2K FullSource 32.6s/2s
- [x] 任务中心质量下拉（2026-10）：快速/平衡/高质量，默认高质量；质量档写入 `meta.tsv`，续编沿用；旧任务缺字段按 Fast 兼容
- [x] 真人/动画模型选择（2026-10，见 `docs/plan-model-library-2026-10.md`）：复用现有 General x4 / animevideov3 x4；内容类型写入 meta，任务卡片显示模型；动画模型实测约快 1.3–1.4x
- [x] 高质量模型档（2026-10）：x4plus（真人，33.4MB）/ x4plus-anime（动画，8.9MB）接入；输入上限 960；模型档位写入 meta、续编沿用；旧任务默认标准
- [x] 动画 2x/3x 自动提速（2026-10）：animevideov3 x2/x3 接入；按 target/src 自动选 x2（≤2x）/x3（≤3x）/x4；通用 scale 替换 encode 硬编码 4x；720p→2K x2 约 1.9s/0.5s，720p→4K x3 约 2.7s/0.5s
- [x] 沉浸式播放 UI（2026-10）：文件名/打开移到顶部标签栏，全屏按钮移到控制条最右；右侧选项面板合并美化与播放历史（历史持久化、点击续播）；美化面板磨皮/美白各占一行、滤镜三列网格；全屏隐藏标签栏与页面顶栏，控制条 3s 自动隐藏
- [x] 离线 SR 流水线与 Vulkan 推理优化（2026-10，见 `docs/plan-offline-pipeline-2026-10.md`）：SR 推理/后处理两级 worker + 有界队列 + 源/输出帧池；启用 FP16 算术/cooperative matrix/shader local memory/packing layout；SR float→RGB 输出融合并并行转换；受控 A/B 的 2 秒 720p→1080 FullSource 约 19.6s→17.8s（约 -9%），后处理再降约 3–4 倍，最新 11.5→11.3s（4K 11.8→11.3s）；多 SR worker 实测会触发 Vulkan 队列错误，已回退

### 进行中 / 待办（按优先级）
1. [x] **任务中心端到端人工验收**：建任务 → 编码 → 进度 → 断点续编 → 边播放；用户已确认通过
2. [ ] **可选推理后端（普适性优先）**：NCNN Vulkan 保持默认、零额外安装；只做 ONNX Runtime DirectML 免 CUDA fallback；TensorRT/CUDA 仅作为 NVIDIA 高级可选性能包（见 `docs/plan-backends-2026-10.md`）
3. [ ] **2K/4K 实时 SR 优化**：优化后约 2K≈22、4K≈9 FPS；自动降档已兜底；真正 2K/4K 30fps 仍需后端/轻量模型
4. [ ] **高质量模型盲测/模型包**：验证 x4plus / x4plus-anime 在真人、动画、文字上的收益；若继续增加模型，做可选模型包/下载（见 `docs/plan-model-library-2026-10.md`）
5. [ ] **真人 2x/3x 模型**：动画 x2/x3 已接入；SRMD 与当前 NCNN 不兼容，Real-CUGAN 真人实测过度平滑（不建议），Real-ESRGAN x2plus/BSRGAN2X 实测速度不适合替代 General 4x（见 `docs/plan-realistic-2x3-models-2026-10.md`）
6. [ ] **离线 SR 大图优化**：FullSource 1080p→4K 中间帧 7680×4320、峰值约 1.9GB；评估 tile + overlap
7. [ ] 打包分发：安装包/绿色版、图标、关于页
8. [ ] 移动端验证：Android/iOS 注入 `AudioSink` 实现（引擎已可移植）

## 里程碑
| 里程碑 | 内容 | 状态 |
|---|---|---|
| M1 | 引擎抽取 + 可移植（`domino_core`/`IFrameOutput`/`AudioSink`） | ✅ 完成 |
| M2 | Flutter 桌面功能对齐（播放 + 任务中心 + 美颜/滤镜） | ✅ 完成 |
| M3 | 实机验收 + 性能达标（全屏、SR 播放、GPU 美颜） | 🚧 进行中 |
| M4 | 打包分发 | ⬜ 未开始 |

## 风险 / 注意
- 每次改桥/引擎后确认 runner 目录 `domino_engine.dll` 时间戳已更新（见 `AGENTS.md` 已知坑）。
- CPU 美颜是当前帧路径主要开销；开滤镜/磨皮时 FPS 会下降，GPU 化是后续重点。
- 移动端音频需自实现 `AudioSink`（引擎接口已备）。
