# 架构审视（2026-10）

> 范围：`src/` 可移植引擎、`flutter/native` 桥、Flutter 前端。结论用于下一阶段重构，不追求一次性重写。

## 做得好的

- **契约清晰**：`IFrameOutput` / `AudioSink` 把平台相关部分隔离，`domino_core` 无 D3D11/WASAPI 依赖。
- **任务模型完整**：持久化、断点续编、合并、失败恢复、文件占用处理都有，且有 headless 测试。
- **测试习惯好**：C++ 轻量测试 + Flutter `FakeEngine`，DoD 明确。
- **GPU 美颜**：D3D11/HLSL 通过 Flutter GPU surface 上屏，CPU 回退完整。
- **A/V 基础正确**：音频时钟优先、PTS 呈现、暂停/seek 锚点、编码精确帧率。

## 主要不合理点

### 1. `Player` 是“上帝对象”，且视频重活跑在 Flutter UI 线程（本轮修复）

`pump()` 在 Dart Ticker 里同步执行：解码 → NCNN SR → `IFrameOutput::presentFrame`（GPU 上传统/美颜）→ 纹理标记。
SR 24–123ms/帧直接阻塞 UI 线程，导致：

- 1080p SR App 仅 ~21fps；
- 2K/4K 时 UI 完全无响应；
- 模型加载 0.3–0.6s 也在 UI 线程。

**本轮**：视频解码 + SR 移到原生工作线程，UI 只做“取最新帧 + 呈现/标记”。
音频仍独立线程；GPU 美颜暂留 UI（下一阶段可再移到渲染线程）。

### 2. `IFrameOutput` 只有 RGB24 契约

GPU 路径每帧都要 CPU 做 RGB24→RGBA 上传；4K 下这是明显成本。
后续可增加像素格式/能力协商，或让解码直接输出 RGBA/BGRA；本轮不动接口。

### 3. GPU surface 每帧 `QUERY_EVENT` 忙等

为保证跨设备共享纹理内容可见，`GpuFrameOutput` 每帧等待 GPU 完成。正确但保守，
与 SR 串行后进一步压低帧率。后续用 fence + 双帧流水/keyed mutex 优化。

### 4. 任务中心与 Player 共享全局 NCNN GPU 实例

目前由桥层统一销毁，退出顺序正确；但“多任务并行 SR”仍受 GPU 资源限制。
建议后续显式限制同时运行任务数，或让任务中心串行化 SR。

### 5. 同步 FFI / UI 状态耦合

`domino_pump` 仍是 UI 线程同步调用；桥层全局单例 `g_engine` 只适合单窗口。
本轮只把重活移走；后续可考虑一次 FFI 返回“最新帧元数据”，减少每帧多次查询。

### 6. 其他小问题

- 模型失败/日志已有回退，但错误提示主要还是 stderr；发布版用户看不到。
- `TaskCenter` 的 stop/start 在 UI 线程 join，长任务退出可能短暂卡顿。
- 前端 `main.dart` 已超过 600 行，播放页/任务中心/引擎绑定可以继续拆分。
- CPU/GPU 输出、SR、美颜三个开关组合较多，缺少统一的“能力/状态”对象。

## 本轮决策（已完成）

1. 做 **VideoWorker**：解码 + SR 移出 UI 线程，UI 只消费最多 2 帧的队列。
2. 工作线程 seek 用 generation 丢弃在途旧帧；Ticker 停摆用 `dropUntilPts` 解码跳过
   且**不跑 SR**，避免长时间卡 UI。
3. 实机结果：720p 源 SR 1080p App **21 → 29.6 FPS**；2K ≈15、4K ≈5.9 FPS（受推理限制，
   低于实时时是慢放/音画逐渐分离），但 UI 线程不再被单帧 50–123ms 的 SR 阻塞。
4. 追加 **SR 自动降档**：持续 A/V 落后时 4K/2K→1080p，1080p→关闭；用于兜底
   2K/4K 低于实时导致的慢放/音画分离（见 `docs/plan-auto-degrade-2026-10.md`）。
5. 仍保留：`IFrameOutput` RGB24 契约、GPU surface 每帧等待、任务中心 SR 并发策略；
   这些列入后续计划。
