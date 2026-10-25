# Domino Player v0.2.4

## 修复

- **修复实时超分帧率低时视频变慢**：SR 产出低于片源帧率时，VideoWorker 跳过旧的源帧，保持媒体时间 1x，而不是把所有帧都按原 PTS 慢慢播放。
- 自动降档判断改为使用 **VideoWorker 实际 SR 产出 FPS**，避免跳帧后呈现 FPS 被误判。
- 4K 桌面零拷贝仍约 30 FPS；竖屏/超宽继续正常。
- 保持零拷贝、cudart 回退和任务中心 HQ 行为不变。

## 下载

`domino-player-v0.2.4-win64.zip`

解压后双击 `domino_flutter.exe`。需要 64 位 Windows 10/11。

## 说明

- 低 SR FPS 时优先保证 **1x 播放速度**，可能出现跳帧；若持续低于 15 FPS 会按既有策略自动降档。
- 零拷贝需要 NVIDIA GPU；发布包包含 `cudart64_12.dll`。
- 混合显卡机器可用 `DOMINO_D3D_ADAPTER=default` 退回默认 adapter。

## 校验

`domino-player-v0.2.4-win64.zip` SHA256：

```text
8F3041D9A1D5BDBF88B2FA57CD6469EFB99678F159F3DF2F4FA3B2C49CDB6031
```
