# Domino Player v0.2.3

## 重点更新

- **TensorRT → D3D11 零拷贝输出**：TensorRT FP16 输出直接写入 D3D11 R16 纹理，去掉 D2H、CPU FP16→RGB 和整帧 RGBA 上传。
- **4K 实时 SR 提升到约 30 FPS**：RTX 4070 SUPER、720p 源、4K SR 实测约 30 FPS；不可用时自动回退 CPU（约 23.6 FPS）。
- **自动降档改为实际呈现 FPS < 15 持续 2 秒**才触发，避免 23–30 FPS 被误降。
- **竖屏 / 超宽比例**零拷贝播放通过回归；竖屏按 4K 高度上限输出。
- **任务中心 HQ**：FullSource 高质量任务和 `realistic_hq`（x4plus）任务均完成 450 帧回归。
- 修复离线编码预览在动态 sws 重建后的 use-after-free，提升 ctest 稳定性。

## 下载

`domino-player-v0.2.3-win64.zip`

解压后双击 `domino_flutter.exe`。需要 64 位 Windows 10/11。

## 零拷贝/运行要求

- 零拷贝需要 NVIDIA GPU；发布包已包含 `cudart64_12.dll`。
- TensorRT-RTX 运行时约 220MB，首次使用需要构建 engine 缓存（约 12–20 秒）。
- 缺少 cudart、D3D11 互操作失败或开启美颜/滤镜时自动回退 CPU，不影响播放。
- 混合显卡机器若 Flutter 无法打开共享纹理，可设置 `DOMINO_D3D_ADAPTER=default` 退回默认 adapter（零拷贝自动降级）。
- 强制 NCNN：`DOMINO_NO_TRT=1`。

## 已知限制

- 4K 约 30 FPS 是 RTX 4070 SUPER + 720p 源实测值，其他 GPU/片源会有差异。
- 竖屏 2424×4320 等高于 16:9 4K 的输出约 24–25 FPS。
- 真人 2x/3x 模型暂未接入；x4plus / x4plus-anime 仍较慢。
- 未签名 exe 首次运行可能触发 SmartScreen。
- 仅提供 Windows x64 预编译包。

## 校验

`domino-player-v0.2.3-win64.zip` SHA256：

```text
16AD93DD909EE6FEA3218E44BE2C7C9BF1B4AB9B6CBEA595D4346F47FCAD5584
```
