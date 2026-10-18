# 计划：离线 SR 质量优先（2026-10）

> 状态：**已完成**（2026-10-18）。离线超分不再固定 `target/4` 输入；
> 默认 `FullSource` 保留源宽，模型 4x 输出后再缩到目标尺寸；编码改为 CRF 18 + preset slow。
>
> 目标：不考虑实时，优先让离线任务保留源细节、输出高质量 1080p/2K/4K。

## 输入策略

`chooseSRInputWidth(srcW, targetW, mode)`：

| 模式 | 输入宽 | 用途 |
|---|---|---|
| Fast | `targetW / 4` | 旧路径，最快 |
| Balanced | `max(targetW/4, min(srcW, max(srcW*3/4, targetW/4)))` | 质量/耗时折中 |
| **FullSource（默认）** | `max(targetW/4, min(srcW, 1920))` | 尽量保留源宽，同时限制中间帧 |

> 输入宽上限 1920：1080p→4K 会输出 7680×4320 中间帧（实测峰值工作集约 1.9GB）；
> 2K/1440p 源也不会继续放大到 10240 宽，避免离线任务/多任务 OOM。

- 1080p 源 → 2K：输入 **1920**（旧路径 640），4x 输出 7680，再缩到 2560。
- 1080p 源 → 4K：输入 **1920**（旧 960），4x 输出 7680，再缩到 3840。
- 720p 源 → 1080p：输入 **1280**（旧 480），4x 输出 5120，再缩到 1920。
- 源极小时仍用 `target/4`，保证 4x 输出不小于目标。

## 缩放/编码

- 源 YUV→RGB 后，**不做预降采样**（FullSource）；
- SR 输出用 `SWS_LANCZOS` 缩到目标分辨率；
- 预览回调也按目标尺寸缩放（大 SR 输出不会传错尺寸）；
- x264：`CRF 18`、`preset slow`、仍 `max_b_frames=0 + tune=zerolatency`（保持 fMP4/断点续编的 A/V 约束）。

## 实测（2s 720p 源 → 1080p，RTX 4070 SUPER）

| 输入策略 | 耗时 | 说明 |
|---|---:|---|
| Fast | 2.2s | 旧路径，输入 480，边缘有灰晕 |
| Balanced | 8.8s | 输入 960 |
| **FullSource** | 15.3s | 输入 1280，文字/色块边缘明显更干净 |

1080p 源 → 2K（2s）：

| 输入策略 | 耗时 |
|---|---:|
| Fast（640） | 4.2s |
| Balanced（1440，更新后） | 19.4s |
| **FullSource（1920）** | 32.6s |

1080p 源 → 4K：FullSource（1920）约 36s/2s，峰值工作集约 **1.9GB**，输出 3840×2160。
FullSource 约为 Fast 的 4–8 倍耗时；15s 视频可能需要 2–5 分钟，离线可接受。
输出文件在 `%TEMP%\domino_offline_*.mp4`，`domino_sr_bench` 会打印路径。

## 失败处理

- `sr.process` 失败/输出尺寸与预期不一致时**回退源帧**，不丢帧（否则 PTS 前移、任务假完成）；
- 本次编码记录 `sr_fallback` 个数，结束时 `encodeOffline` 返回失败，任务落到 `ST_FAILED`；
- 1080p→4K FullSource 实测输出 3840×2160、60 帧、时长 2.023s、音视频 start=0，无裁切。

## 验证

- `domino_tests`：`chooseSRInputWidth` Fast/Balanced/FullSource/1920 上限断言；`TaskWithSRCompletes` 断言产物 1920×1080；编码 A/V/续编测试仍通过（53s）；
- `domino_sr_bench`：新增 Fast/Balanced/FullSource 离线编码对比，可传源/目标宽；
- 实机/截图：FullSource 的 1080p 输出文字和色块边缘比 Fast 干净，无灰晕；
- 1080p 源 2K FullSource 输出 2560×1440；1080p 源 4K FullSource 输出 3840×2160。

## 后续

- 如果 FullSource 4K/8K 输出内存压力大，再加 **tile + overlap**，保持输入分块但避免整帧 7680 输出；
- 任务中心可增加“质量/平衡/快速”选择（当前默认 FullSource）；
- 离线 CRF/preset 可按用户偏好暴露。
