# 计划：实时 SR 播放评估（2026-10，第四阶段）

> 状态：**已完成评估**（2026-10-18）。播放页已开放 SR 关闭/1080p/2K/4K 手动档位；
> 新增 `domino_sr_bench`；实测 1080p App 21 FPS、2K 12 FPS、4K 5.5 FPS，
> 结论是当前同步 UI 线程路径不能稳定实时，下一阶段应把 SR 移出 UI 线程。
>
> 目标：开放播放页 SR 开关/目标挡位，量化 720p 源在 1080p/2K/4K 下的推理耗时与呈现 FPS，
> 给出是否可实时、需要什么降级策略的结论；不在本轮实现 GPU 直通/TensorRT。

## 范围

1. **播放 SR 可配置**
   - `Player`：启用 SR 时惰性初始化；初始化失败置失败标志并回退源帧，避免每帧重试。
   - FFI：`domino_set_sr(h, enabled, targetW)` + `domino_sr_active(h)`。
   - Flutter：控制条 SR 下拉（关/1080p/2K/4K），切换后重新设置目标；打开新视频时保持选择。
2. **基准工具**
   - 新增 `domino_sr_bench`（不加入 ctest）：
     - 直接 `SR::process`：720p 源 → 目标 1920/2560/3840，统计 ms/帧、理论 FPS；
     - `Player` 端到端：同样目标下运行数秒，统计实际呈现 FPS。
   - 测试源：`tests/assets/test_long.mp4`（1280×720/30fps）。
3. **实机评估**
   - 在 RTX 4070 SUPER 上运行基准，并在 `domino_flutter.exe` 用 SR 下拉逐档观察 FPS/流畅度。
   - 结论写入本文档与 `docs/roadmap.md`。
4. **降级策略（按数据决定）**
   - 若 4K 不能稳定 30fps，保留手动档位并记录建议；
   - 如需要，再补自动降档（连续 N 秒低于阈值 → 降一档/关闭）。

## 评估结果（RTX 4070 SUPER，720p 源，GPU 美颜关闭）

`domino_sr_bench`（headless）与 App 实测：

| 目标 | 直接 SR ms/帧 | 直接 SR 理论 FPS | Player headless FPS | App 实际 FPS |
|---|---|---|---|---|
| 1080p | 24.1 | 41.5 | 30.0 | **21.0** |
| 2K | 50.1 | 20.0 | 17.3 | **12.1** |
| 4K | 123.5 | 8.1 | 7.6 | **5.5** |

对照：同一 App 关闭 SR 约 30 FPS；CPU 输出 + SR 1080p 约 14.6 FPS。

### 结论

- **1080p 的模型推理本身够快**（24ms，理论 41fps），但当前 App 只有 21fps：
  SR 同步跑在 Flutter UI 线程，叠加 RGB→RGBA 上传、GPU surface 渲染与每帧
  `D3D11_QUERY_EVENT` 等待，超过 33ms 预算。
- **2K/4K 不可实时**（12/5.5fps），只适合离线任务；4K 还会明显卡住 UI 线程。
- **低于源帧率时是“慢放”，不是单纯掉帧**：`playhead_` 只在 `presentFrame` 推进，
  音频仍按 1x 播放，音画会随播放时长持续错开；4K 约 0.18x（5.5/30），
  2K 约 0.4x，1080p 约 0.7x。评估时必须把这点作为“不可实时”的核心证据。
- 当前瓶颈不是 NCNN 推理，而是**同步执行模型 + 每帧等 GPU 完成**。
- Ticker 停摆后的追帧只解码、不跑 SR，并限制每 pump 100ms/总共 2s，
  避免 4K SR 时一次 pump 卡数十秒；但这不解决持续低于实时时的慢放。
- 打开 SR 时保留手动档位即可；默认关闭，不自动开启。

### 建议的下一阶段（另立计划）

1. SR 移出 Flutter UI 线程（原生工作线程 + mailbox 最新帧），先解决 1080p 的 21→30fps；
2. GPU surface 渲染不要每帧忙等，改 keyed mutex/fence 或双帧流水；
3. 评估 NCNN Vulkan 输出直接对接 D3D11 共享纹理，去掉 CPU Mat 往返；
4. 模型预热/异步初始化（首次启用 SR 的 0.3–0.6s load 不应阻塞 UI）；
5. 若仍不达标，1080p 档加“连续低帧率自动降档/关闭”，并在低于实时时丢帧但跳过 SR，
   避免慢放/音画持续分离。

## 不在本轮

- NCNN GPU 直通/零拷贝、TensorRT、硬件解码；
- 实时 SR 与 GPU 美颜同时开启的深度调优（先分别测）。
