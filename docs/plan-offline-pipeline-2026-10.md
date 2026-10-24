# 离线 SR 流水线与 Vulkan 推理优化（2026-10）

> 状态：**已完成**。离线编码把 SR 放到独立线程，并启用 NCNN Vulkan 的 FP16 算术 / cooperative matrix / shader local memory 优化。
> 目标：在不改变输出帧序、A/V 同步和断点续编语义的前提下提高离线任务吞吐。

## 实现

### 1. SR 独立线程 + 有界队列

`encodeOffline` 在 `needSR`（源宽小于目标宽）路径中：

- 主线程继续负责 demux、解码、音频包、Lanczos 缩放、x264 和 mux。
- `SR::process` 在独立线程执行，主线程与其重叠。
- `srJobs` 最多缓存 2 帧，`srResults` 按 FIFO 取回，保证视频 PTS 顺序不变。
- 源 RGB 缓冲使用 `freeSrc` 池复用，避免每帧重新分配 1080p/4K 大块内存；SR 输出缓冲目前仍随结果分配，4K 任务需关注峰值内存。
- 主线程在提交帧前会 drain 已完成的 SR 结果；队列满时等待 SR 取帧，不会无界读入。
- 取消 / 写入失败走统一的 `finishSr(abort)`，等待 worker 退出后再清理/冲刷编码器。
- SR 失败仍回退源帧并最终判任务失败，`sr_fallback` 计数和失败语义保持原样。
- 预览回调仍在主编码线程调用，输入的源/输出尺寸与原来一致。

### 2. 输出转换融合 + 并行 + 输出缓冲池

原实现先逐像素 `*= 255`，再 `to_pixels`，4K 输出会额外读写整块 RGB。现在：

- 从 NCNN 的 float planar 输出直接转换到 RGB24，把乘 255、裁剪和写 RGB 合成一遍。
- 大输出按行分给最多 8 个线程；小输出单线程，避免线程创建开销。
- 转换结果与旧 `to_pixels` 路径逐字节比较：`diff=0`。
- `SrJob` 携带输出缓冲，主线程编码后归还 `freeOut` 池，复用大块 SR 输出内存。

本机 profile（同一构建）：

| 目标输入 | 旧后处理 | 新后处理 | SR 总耗时 |
|---|---:|---:|---:|
| 480×270 | 5.1ms | 1.7ms | 24.3→20.7ms |
| 640×360 | 9.3ms | 2.2ms | 50.8→43.4ms |
| 960×540 | 21.7ms | 5.6ms | 124.3→108.2ms |
| 1280×720 | 41.5ms | 10.5ms | 220.5→189.3ms |

输出缓冲池相对无池再省约 2%，主要收益是减少大块分配抖动和峰值内存。

### 3. NCNN Vulkan 选项

`SR::init` 额外启用：

```cpp
use_fp16_arithmetic = true;
use_cooperative_matrix = true;
use_shader_local_memory = true;
use_packing_layout = true;
```

原有 `use_fp16_packed` / `use_fp16_storage` 保留。`use_packing_layout` 在 RTX 4070 SUPER 上实测直接 SR 1080p/2K/4K 约 **20.7→20.0 / 44.4→43.3 / 108.8→107.1ms**，离线 FullSource 约 **11.7→11.5s**，作为小幅优化保留。驱动不支持时由 NCNN 自动退回普通 Vulkan 路径；CPU 路径仍可正常回退。若现场驱动出现异常，可设置 `DOMINO_NO_VULKAN_FASTPATH=1` 关闭这些快路径。

### 4. infer / post 两级流水线

在单 SR worker 基础上进一步拆分：

- `SR::infer` 只负责输入转换 + GPU 推理，返回 `ncnn::Mat`。
- `SR::convert` 是静态的 float→RGB24 转换，可放到独立线程。
- 推理 worker 完成一帧后立刻取下一帧，后处理 worker 并行转换上一帧；主线程继续编码。
- 推理/后处理队列各限制 2 帧，结果按 seq 排序后交给编码；不改变 PTS/续编语义。
- 取消时同时中止推理和后处理线程，最后统一 join。

实测（2s 720p→1080 FullSource）：**11.51s→11.26s**；4K FullSource：**11.80s→11.33s**。提升约 **2%–4%**，主要来自 GPU 推理期间重叠 CPU 输出转换。

### 5. 单队列 batch 推理验证（不采用）

验证结论：**当前 NCNN 模型/运行时无法做单队列 batch**。

- NCNN `Mat` 只有 `w/h/d/c` 维度，没有 batch/n 维度；`Extractor::input` 一次只接受一个样本。
- RealESRGAN compact 首层卷积固定 `in_channels=3`，不能把两帧拼成 6 通道或 2 个样本直接送进网络。
- 把两帧横向拼成一张大图会改变空间语义，接缝和跨帧上下文会污染结果，不能作为等价 batch。
- 多 extractor/多 worker 已在上一项验证中触发 `vkQueueSubmit failed -4`，单队列串行又不会带来吞吐收益。

需要 batch 时的正确路径是 **ONNX Runtime / TensorRT / DirectML 的 batch 输入**，或修改模型/后端支持 batch；这不是当前 NCNN 栈的一行优化。

### 6. 多 SR worker 实验（未采用）

尝试过 2 个独立 `SR`/Net 的 worker：

- 单次 bench 曾出现 FullSource 11.7s→10.4s 的表面提升。
- 但 ctest 压测出现 `vkQueueSubmit failed -4`，部分帧回退源帧，任务失败。
- NCNN Vulkan 当前配置下并发 Net 的 GPU 队列不可靠；不能为了吞吐牺牲正确性。

因此保留单 SR worker，多 worker 方案回退。

### 7. 没有改动的部分

- 视频帧 PTS 仍由 `startFrame + count` 生成，续编偏移不变。
- 音频起点归一、音频最多领先视频 100ms、AAC ADTS BSF 逻辑不变。
- 失败任务不能静默丢帧；SR worker 失败会回退源帧并最终判任务失败。
- 文件读写没有做特殊优化：基准显示文件 IO 不是离线瓶颈。

## 基准（RTX 4070 SUPER）

受控 A/B（同一台 RTX 4070 SUPER、同一 2 秒 / 60 帧 720p→1080p FullSource、CRF18 preset slow，各跑 3 次）：

| 指标 | 优化前 | 优化后 |
|---|---:|---:|
| 耗时 | 19.6s | **17.8s（约 -9%）** |
| 峰值工作集 | 841MB | 982MB（3 帧队列 A/B 实测；当前上限已降为 2 帧，4K 仍需实测） |

直接 SR 的绝对毫秒数受 GPU 频率/后台负载影响较大，本表不把它作为结论。SR 独立 worker 的主要收益来自 GPU 推理与主线程解码/缩放/x264 的重叠；测试环境应固定同一驱动和 GPU 状态。

完成输出转换优化后，本机同一 `domino_sr_bench` 的参考值：

| 项目 | 结果 |
|---|---:|
| 离线 Fast（2s 720p→1080） | 1.67s |
| 离线 Balanced | 6.80s |
| 离线 FullSource | 11.63s |
| Player 1080p / 2K / 4K | 30.0 / 21.7 / 9.0 FPS |

该组数据与上表不是同一热状态，用于观察趋势；绝对数字仍会随 GPU 状态波动。

### 8. TensorRT-RTX 后端（2026-10-24 接入）

- `Realistic` / `RealisticHQ` 走 TensorRT-RTX 1.7.1 FP16 ONNX；动画与 2x/3x 仍走 NCNN。
- 保留 infer / post 两段流水线；TensorRT 在 infer worker 完成推理 + D2H，post worker 继续按 seq 排序后编码。
- engine 缓存在 `%APPDATA%/DominoPlayer/trt-rtx`；初始化失败回退 NCNN，推理失败仍按规则回退源帧并判任务失败。
- 2s 720p→1080（RTX 4070 SUPER）：Fast 1.81s，Balanced 3.18s，FullSource **5.11s**；NCNN 分别为 1.42 / 6.62 / 11.26s。
- x4plus HQ 2s 720p→4K FullSource：TensorRT **31.9s**；NCNN 直接推理约 1378ms/帧（估算约 83s）。
- batch=2 在 compact 1080p 上约 214ms/2 帧，没有吞吐收益，暂不启用。
- 详见 `docs/plan-backends-2026-10.md`。

## 验证

- `ctest --test-dir build -C Release --output-on-failure`：通过（含 `TaskWithSRCompletes`、`ResumeAndMerge`、新增 `EncodePreviewSR`）。
- `flutter test`：43 passed；Windows Debug / Release 构建成功。
- Release 实机启动并播放本地视频正常；断点续编/取消由 C++ 测试覆盖。
- `domino_sr_bench` 可复现本次 A/B 数字。

## 后续可做

1. 对 4K FullSource 加入 **tile + overlap**，主要降低 7680×4320 中间帧的内存峰值。
2. 评估 TensorRT / DirectML，或更轻的 2x/3x 真人模型；这是 2K/4K 实时化更根本的路径。
3. 为 SR 的输入转换/输出归一化做融合或并行化，减少 CPU 侧拷贝。
4. 若后续加入多 GPU 或多队列，再评估多帧并行 SR；当前单 worker + 主线程编码已经覆盖主要重叠收益。
