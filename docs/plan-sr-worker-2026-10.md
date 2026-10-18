# 计划：SR/解码移出 UI 线程（VideoWorker，2026-10）

> 状态：**已完成**（2026-10-18）。720p 源 SR 1080p App 从 **21 → 29.6 FPS**；
> 2K ≈15 FPS，4K ≈5.9 FPS（仍受推理限制），但 UI 不再被单帧 SR 阻塞。
> `ctest` / `flutter test` / `flutter build windows --debug` 通过。
>
> 目标：把视频解码 + NCNN SR 从 Flutter UI 线程移到原生工作线程；
> UI Ticker 只负责从“最新帧队列”取帧、按 PTS 呈现/标记纹理。
> 预期 1080p SR 从 App 21fps 提升到接近源帧率 30fps，并避免 2K/4K 卡死 UI。

## 设计

```
VideoWorker (native thread)
  vid_.nextVideoFrame → (可选 SR) → VideoFrame{rgb,pts,w,h}
       ↓ 队列最多 2 帧（mutex + condvar）
UI Ticker: Player::pump
  processSeek → prefetch(从队列取一帧) → readyToPresent(音频时钟/PTS) → out.presentFrame
Audio thread: 不变
```

- `sr_`、`vid_`、解码/SR 缓冲全部只由 VideoWorker 访问；UI 不再调用 `vid_.seek/nextVideoFrame`。
- SR 配置（`noSR_/srTargetW_/inW_`）、`srInited_/srFailed_` 改为 atomic；UI 只读写开关/查询状态。
- seek：
  - UI 递增 `seekGen`、设置目标、清空队列、通知 worker；
  - worker 执行 `vid_.seek`，之后只接受当前 generation 的帧，丢弃 seek 前在途帧。
- Ticker 停摆：
  - UI 设置 `dropUntilPts = currentMediaClock()` 并清空队列；
  - worker 解码时若 PTS 远落后于目标，**丢弃且不跑 SR**，追到目标后恢复；
  - UI 不阻塞，避免 4K 一次 pump 卡数十秒。
- 队列最多 2 帧；帧缓冲从 `freeBufs_` 复用，避免每帧分配大 vector。
- EOF：worker 置 `videoEof_`；UI 队列空时转成 `pendingEOF_`，沿用现有 `handleEof`。
- 失败：SR 初始化/推理失败回退源帧；不阻塞 UI。

## 实机结果（720p 源，GPU 输出，GPU 美颜关闭）

| 目标 | Worker 前 App | Worker 后 App | Player headless |
|---|---|---|---|
| 1080p | 21.0 | **29.6** | 30.0（源上限） |
| 2K | 12.1 | **15.0** | 19.3 |
| 4K | 5.5 | **5.9** | 8.0 |

> 单次运行数据，GPU/负载波动约 ±10%；headless 为 CPU 输出（无 GPU surface 上传/等待）。

- 1080p 已达到源帧率（30fps）上限，主要瓶颈不再是 SR；
- 2K/4K 仍受 NCNN 推理速度限制，但 UI 保持响应；
- **2K/4K 低于源帧率时仍是“慢放”**：视频按 15/30、6/30 倍速推进，音频 1x；
  现已由 `plan-auto-degrade` 兜底：持续落后约 1–2s 后自动降到 1080p，仍慢则关闭 SR；
  逐帧丢帧保 A/V 曾试过会让 4K 队列饿死，已放弃；
- 首帧 SR 模型加载在 worker，UI 不再停顿。

## 不改

- `IFrameOutput` RGB24 契约；
- GPU surface 的共享纹理/等待策略（下一阶段）；
- 音频线程、任务中心、A/V 调度。

## 验证

- `ctest`：现有 598+ 断言 + 新增“SR 失败回退”“稳态不丢帧”“seek generation”；
- `flutter test`：30 个用例；
- `domino_sr_bench`：Player headless 各档位应维持/略升；
- 实机：720p 源 SR 1080p App FPS 应接近 30；2K/4K 至少 UI 保持响应；
- 暂停/seek/循环/切换 SR 档位/换视频无花屏/旧帧。
