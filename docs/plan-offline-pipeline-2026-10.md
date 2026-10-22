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

### 2. NCNN Vulkan 选项

`SR::init` 额外启用：

```cpp
use_fp16_arithmetic = true;
use_cooperative_matrix = true;
use_shader_local_memory = true;
```

原有 `use_fp16_packed` / `use_fp16_storage` 保留。驱动不支持时由 NCNN 自动退回普通 Vulkan 路径；CPU 路径仍可正常回退。若现场驱动出现异常，可设置 `DOMINO_NO_VULKAN_FASTPATH=1` 关闭这三项快路径。

### 3. 没有改动的部分

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
