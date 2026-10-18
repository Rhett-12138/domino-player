# 计划：SR 自动降档（Auto Degrade，2026-10）

> 状态：**已完成**（2026-10-18）。720p 源选 4K 约 1–2s 后自动到 1080p，
> 实测 29.98fps；手动选 1080p 不降档（30.0fps）。`ctest` / `flutter test` / Release 实机通过。
>
> 目标：2K/4K SR 低于实时时，不再让视频慢放、音画持续分离；检测持续 A/V 落后后
> 自动从 4K/2K 降到 **1080p**，1080p 仍跟不上则关闭 SR，并跳到当前媒体时钟。
> 手动选择档位随时重置自动状态。

## 判定

在 `Player::pump()`（UI 线程）每 ~200ms 检查一次：

```
lag = currentMediaClock() - (havePending ? pendingPts : playhead)
```

- lag > **0.30s** 开始计时；
- 持续 **1.0s** 仍超阈值 → 降一档：
  - 3840 / 2560 → **1920**
  - 1920 → **0（关闭 SR）**
- 降档后：
  - 设置 `dropUntilPts = min(clock, duration)`，worker 只解码不跑 SR 追到当前时钟；
  - 清空待呈现帧/队列；
  - 4s 冷却，避免连续降档。
- 1080p 仍持续落后（例如 60fps 源）→ 4s 后关闭 SR。
- 不自动升档；用户重新选 4K/2K 时重置自动状态与冷却，重新尝试。

## 状态

- `srCeilingW_`：用户选择的档位（自动降档不会改它）；
- `srTargetW_`：当前实际档位（UI 轮询展示）；
- `srAutoDegraded_`：是否由自动降档产生；
- FFI 新增 `domino_sr_target_w` / `domino_sr_auto`；
- UI 标签显示 `SR 1080p（自动）` / `SR 关闭（自动）`。

## 不做

- 不做逐帧丢帧保 A/V（4K 会饿死队列，已验证）；
- 不做自动升档（避免抖动）；
- 不改 NCNN/GPU surface 零拷贝。

## 验证

- C++：2x 音频时钟模拟持续落后，4K→1080p；1080p→关闭；
- Flutter：自动降档标签更新；
- 实机：720p 源选 4K，约 2s 后自动到 1080p 并恢复流畅；选 1080p 不降档。
