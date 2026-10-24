# 推理后端：TensorRT-RTX 接入（2026-10）

> 决策：**高性能优先，TensorRT-RTX 已接入；NCNN 保留回退。**  
> 设备：RTX 4070 SUPER，驱动 610.88；TensorRT-RTX 1.7.1.116（CUDA 12.9 版）。

## 结论

- 采用 **TensorRT for RTX 1.7.1**：便携 ZIP，解压约 221MB，**不需要安装 CUDA Toolkit/cuDNN**，不需要管理员权限。
- 仅对 `Realistic` / `RealisticHQ` 的 4x FP16 ONNX 启用；动画、2x/3x 暂走 NCNN。
- 初始化/推理失败自动回退 NCNN；推理中途失败按既有规则回退源帧并最终判任务失败。
- 用户默认不用选后端；高级用户可用 `DOMINO_NO_TRT=1` 关闭。
- DirectML 暂不做；等 TensorRT/CUDA 路径稳定后再补兼容性。

## 实测（RTX 4070 SUPER）

### 直接推理：NCNN vs TensorRT-RTX FP16

| 模型输入 → 目标 | NCNN | TensorRT-RTX | 加速 |
|---|---:|---:|---:|
| 480×270 → 1080p | 21.1ms | 4.7ms | 4.5x |
| 640×360 → 2K | 45.4ms | 9.9ms | 4.6x |
| 960×540 → 4K | 110.9ms | 25.1ms | 4.4x |

- TensorRT-RTX 首次需要 JIT/engine 缓存；上面是稳定值。
- 默认 compact 模型 4K 输入 960×540，输出 3840×2160；峰值上下文约 185MB。
- batch=2 在 compact/1080p 上约 214ms/2 帧，**没有吞吐收益**；暂不启用离线 batch。

### Player 端到端

| 目标 | 优化前 NCNN | TensorRT-RTX |
|---|---:|---:|
| 1080p | 30.0 FPS | 29.6 FPS（源 30fps 上限） |
| 2K | 21.7 FPS | 29.6 FPS（源 30fps 上限） |
| 4K | 9.0 FPS | **22.65 FPS** |

### 离线编码（2s 720p→1080，CRF18）

| 模式 | NCNN | TensorRT-RTX |
|---|---:|---:|
| Fast | 1.42s | 1.81s |
| Balanced | 6.62s | 3.18s |
| FullSource | 11.26s | **5.11s** |

Fast 由固定编码/初始化开销主导，TensorRT 没有优势；Balanced/FullSource 约 2x。

### x4plus HQ（2s 720p→4K）

- NCNN 直接推理 960×540→4K：约 1378ms/帧（60 帧约 83s，估算）。
- TensorRT-RTX FullSource 离线：**31.9s**（约 2.6x）。
- HQ 仍只在离线/任务中心使用；实时播放继续用标准模型。

## 实现要点

- `src/trt_sr.{h,cpp}`：TensorRT-RTX 后端，封装 engine 构建/缓存、CUDA driver API（`nvcuda.dll`）、输入 resize、FP16 推理、RGB 输出。
- `Config`：`onnxBase`、`useTrt`、`srOnnx()`、`trtCacheDir()`；默认 `%APPDATA%/DominoPlayer/trt-rtx`。
- `Player`：可用时走 TensorRT，失败/模型不匹配回退 NCNN；4K 档位不再依赖自动降档。
- `encodeOffline`：同一 backend，保留原 infer/post 两段流水线；TensorRT 分支在 post 线程直接消费 RGB。
- `TaskCenter`：ONNX/engine 缓存路径由桥层注入；任务续编沿用同一模型/质量档；后端按当前机器可用性优先 TensorRT、否则回退 NCNN（meta 不持久化 backend）。
- `DOMINO_NO_TRT=1` 强制 NCNN，供驱动问题排查。
- CMake：`libs/tensorrt-rtx/`（本地放置，git 忽略）自动启用；没有 SDK 时编译为空后端，功能回退 NCNN。
- ONNX 资产：`assets/models/sr/onnx/`
  - `realesr-general-x4v3-fp16.onnx`（2.4MB）
  - `realesrgan-x4plus-fp16.onnx`（33.8MB）

## 引擎缓存/启动

- 动态 profile：compact `320×180 ~ 1920×1080`，HQ `320×180 ~ 960×720`。
- 首次构建：compact 约 12s，HQ 约 20s；之后加载缓存约 5ms。
- 缓存文件按 `模型-v1-最大宽x最大高.engine` 命名；构建临时文件后 rename，失败可重建。
- 首次使用会做一次 960×540 warmup，避免第一帧 JIT 卡顿。

## 待验证/风险

- 竖屏/超宽比例超过 profile 上限时当前会回退源帧；后续可扩大 profile 或按实际 shape 建缓存。
- 4K FullSource（1080p 源→7680×4320 中间帧）显存/内存峰值仍需实机确认。
- TensorRT-RTX 引擎与 GPU/驱动/版本有兼容性要求；缓存失效需重建。
- 分发包增加约 220MB；需接受 NVIDIA TensorRT-RTX 许可并附 notices。
- 画质仍需真人脸部/发丝/文字/夜景 A/B；FP16 与 NCNN 输出已做基础目视检查。

## 下一步

1. 补 4K FullSource、竖屏、超宽、seek/暂停/自动降档的回归。
2. 任务中心 HQ 端到端（建任务→续编→边播放）人工验收。
3. 评估预构建 engine / 首次构建进度提示，避免 12–20s 黑场。
4. 决定发布包是否默认带 TensorRT-RTX，或做成可选高性能组件。
5. 之后再评估 DirectML/AMD/Intel 兼容性。
