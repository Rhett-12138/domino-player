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

### 进行中 / 待办（按优先级）
1. [ ] **任务中心端到端人工验收**：建任务 → 编码 → 进度 → 断点续编 → 边播放（自动化难驱动 Flutter 控件，需人工过一遍）
2. [ ] **GPU 着色器版 `IFrameOutput`**：替换 CPU 美颜（大分辨率/开 SR 时的性能瓶颈），接口已就绪
3. [ ] **实时 SR 播放**：当前 `setNoSR(true)`；评估开 SR 后的帧率与降级策略
4. [ ] 打包分发：安装包/绿色版、图标、关于页
5. [ ] 移动端验证：Android/iOS 注入 `AudioSink` 实现（引擎已可移植）

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
