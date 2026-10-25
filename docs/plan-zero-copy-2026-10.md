# TensorRT → D3D11 零拷贝输出（2026-10）

> 状态：**已接入可选零拷贝路径**，App 4K 从约 23.6 FPS 提升到约 **30.0 FPS**；
> 连续 5 次 4K 冒烟通过，推理约 31–33 ms/帧，cudart copy 约 0.5 ms。

## 目标

去掉 TensorRT `process()` 里的 D2H、CPU FP16→RGB 转换和整帧 RGBA 上传：

```
旧: TensorRT(dOut) -> D2H -> CPU FP16->RGB -> GpuFrameOutput 上传 RGBA -> D3D
新: TensorRT(dOut) -> cudart D2D -> 3×R16 纹理 -> D3D shader(BGRA) -> Flutter
```

## 架构

### 1. core 不依赖 D3D11

`src/engine.h` 新增可选接口 `IGpuFrameSink`：

- `acquireGpuSrFrame(w,h,slot,textures[3])`：取 3 个不透明纹理指针；
- `releaseGpuSrFrame(slot)`：帧被丢弃/seek/暂停时归还；
- `presentGpuSrFrame(slot,w,h)`：由前端做 GPU 呈现。

`Player` 通过 `dynamic_cast<IGpuFrameSink*>` 探测输出实现；`EngineFrameOutput` / 测试桩
不实现时自动保持原 CPU 路径。core 不包含 D3D11 头或库。

### 2. CUDART 互操作封装

`src/trt/gpu_interop.{h,cpp}`：

- 运行期动态加载 `cudart64_12.dll`（也可用 `DOMINO_CUDART_PATH` 指定）；
- 用 CUDA Runtime 的 `cudaGraphicsD3D11RegisterResource` / `cudaGraphicsMapResources` /
  `cudaGraphicsSubResourceGetMappedArray` / `cudaMemcpy2DToArrayAsync`；
- 只接收 `void*` D3D11 texture，不依赖 D3D11 头；
- 固定 2 个输入 slot，每个 slot 一组 `cudaGraphicsResource`；切档/分辨率变化时
  先 `TensorRtSR::resetGpuInterop()` 注销旧资源，再由前端重建 D3D 纹理池，
  不能先销毁 D3D 纹理再 unregister，也不能无界累积 entries；
- 每次仅做 3 个平面的 D2D copy；
- cudart 缺失/注册失败时返回 false，Player 一次性进入 CPU fallback，不每帧重试。

为什么不用 CUDA driver API：实测 driver primary context 下
`cuGraphicsSubResourceGetMappedArray` 返回空、copy 报 `201`；`cuD3D11CtxCreate`
虽然能映射，但 TensorRT-RTX 在该 context 反序列化 engine 报
`cudaErrorIncompatibleDriverContext (49)`。CUDA Runtime 与 TensorRT 使用同一 primary
context，D3D11 资源才能安全互操作。

### 3. D3D11 前端

`flutter/native/gpu_frame_output.cpp`：

- 优先选择 NVIDIA D3D11 adapter（vendor `0x10DE`），避免双显卡机器选到核显；
  混合显卡若 Flutter 打不开 legacy shared handle，可设 `DOMINO_D3D_ADAPTER=default`
  退回默认 adapter（零拷贝自动降级 CPU，显示链路保持原生）；
- 2 个输入 slot × 3 张 `DXGI_FORMAT_R16_FLOAT` 平面纹理（4K 约 100MB）；
- 输入纹理必须带 `D3D11_RESOURCE_MISC_SHARED`；实测去掉后多 slot 会在
  `nvwgf2umx.dll` 偶发 `c0000409`；
- `cudart` 写入与 D3D 采样通过 `IGpuFrameSink::lockGpuCopy/unlockGpuCopy`
  串行化（只包 copy/present，不包 32ms 推理），避免跨 API 同时访问纹理；
- `ps_gpu` 直接采样 R/G/B 三个 R16 平面，输出 BGRA shared texture；
- slot 由 worker acquire、UI present 后释放；所有权唯一，避免 present 失败时双重释放；
- present query 有 250ms 超时；超时视为 fatal，保留 slot busy 并让 Player 回退 CPU；
- 播放时输入缩放用 `SWS_FAST_BILINEAR`（离线仍用 `SWS_BILINEAR`），4K 输入准备从约
  4.0ms 降到约 2.1ms；
- 保留原 `ps_final` 美颜/滤镜路径；有美颜/滤镜时 Player 不走零拷贝，回到 CPU 转换；
- 共享输出纹理仍是 3 缓冲，Flutter release callback 生命周期不变。

### 4. Player 帧生命周期

`VideoFrame` 增加 `gpuSlot`。所有 pending 帧清理统一走 `dropPendingFrame()`，
避免 seek 先清 `havePending_` 导致 slot 永久泄漏。以下路径都会归还 slot：

- seek / generation 变化；
- 追帧丢弃；
- 自动降档、暂停/停摆、clearVideoQueue；
- close/shutdown。

## 实测

设备：RTX 4070 SUPER，TensorRT-RTX 1.7.1，源 1280×720 → 4K（960×540 推理输入）。

| 指标 | 旧 CPU 输出 | 零拷贝 |
|---|---:|---:|
| Player `process` | 40.5 ms | — |
| infer（含输入准备） | 32.0 ms | 31–33 ms |
| ├ 输入 resize | ~4.0 ms | ~2.1 ms |
| ├ RGB→FP16 输入转换 | ~2.1 ms | ~2.1 ms |
| └ TensorRT 推理 | ~27 ms | ~27 ms |
| D2H + FP16→RGB | 8.1 ms | — |
| cudart D2D → 3×R16 | — | **~0.5 ms** |
| 总处理 | 40.5 ms | **~32.5 ms** |
| App 4K 实测 | 23.6 FPS | **30.0 FPS** |
| headless 参考 | 27.6 FPS | — |

- 4K 零拷贝连续 5 次启动冒烟通过（每次打开→SR 4K→播放约 8s）；
- 输出画面、seek/暂停、自动降档冒烟正常；
- 删除 `cudart64_12.dll` 后自动回退 CPU，4K 约 23.8 FPS；
- 自动降档仍为 **FPS < 15 持续 2s**；30 FPS 不会降档；
- NCNN 回退路径 4K 会按既有逻辑降到 1080p。

## 实机回归（2026-10-25）

- 桌面 4K：1280×720 → 3840×2160，**30.0 FPS**，`[trt-gpu]` 持续输出。
- 竖屏：720×1280 → 2424×4320（4K 高度上限），零拷贝持续工作；约 **24.7 FPS**（像素量高于 16:9 4K），画面比例/黑边正常。
- 超宽：1920×800 → 3840×1600，**30.1 FPS**，比例正常。
- 美颜切换：美白 46% 时自动走 CPU，4K 约 **23.4 FPS**；拖回 0% 后恢复零拷贝 **30.0 FPS**，全程不崩溃。
- `DOMINO_D3D_ADAPTER=default` 在当前机器也能正常上屏并保持零拷贝；**真实“核显为默认 adapter”的混合显卡机器仍需外机确认**（当前机器只有 NVIDIA/AMD，未构造驱动级混合输出）。
- 任务中心：新建 **高质量（FullSource, sr_input=2）** 任务，`test_long.mp4` 450 帧完成；输出 `task.mp4`，meta 写入 `sr_input=2` / `model=realistic`。
- 既有用户任务目录（包括 `task-1792330123-0`）未删除、未改写。

## 打包/运行要求

- `libs/cuda-runtime/bin/cudart64_12.dll`（本地、git 忽略，约 570KB）由 CMake 在构建后
  复制到 engine/runner 输出目录；缺省不影响启动；
- `assets/third_party/cuda-runtime/NOTICE.txt` 随包分发；
- 需要 NVIDIA GPU + 驱动；AMD/Intel 仍走 NCNN/CPU 路径。

## 后续

1. 4K 30 FPS 已达成；瓶颈仍是 TensorRT 推理（约 27 ms）和输入准备（约 4 ms）；
2. 需补：竖屏/超宽（输入 texture 尺寸变化时缓存失效）、任务中心 HQ、切换美颜、
   长时间播放的 GPU 纹理/显存回归；
3. 2K/1080p 仍受源 30fps 上限，重点转为低功耗与画质 A/B；
4. 若后续换更快模型，保持 `IGpuFrameSink` + `CudaD3D11Interop` 的分层，不要把 D3D11
   头/库引入 `domino_core`。
