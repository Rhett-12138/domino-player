# 可选推理后端评估（2026-10）

> 目标：在不破坏现有 NCNN Vulkan 回退和便携架构的前提下，评估能让 2K/4K 超分更快的后端。
> 当前基线：RTX 4070 SUPER，NCNN Vulkan，1080p/2K/4K 直接 SR 约 20/44/109ms，离线 2s 720p→1080 FullSource 约 11.3s。

## 候选对比

| 后端 | 平台/硬件 | Batch | 性能潜力 | 依赖/分发 | 结论 |
|---|---|---|---|---|---|
| **NCNN Vulkan**（现状） | Windows/Linux/Android，Vulkan 厂商 | 无（Mat 无 batch 维） | 基准 | 约 13MB NCNN DLL | 保留默认/回退 |
| **ONNX Runtime DirectML** | Windows 10/11，DX12 全厂商 | 支持，建议静态 batch | 中；通常低于 TensorRT，需实测 | ORT + DirectML，无需 CUDA；DirectML EP 处于维护模式 | **首个原型候选** |
| **ONNX Runtime CUDA** | NVIDIA，CUDA 12 + cuDNN | 支持 | 中高 | ORT GPU 包 + CUDA/cuDNN | TensorRT 不可用时备选 |
| **ONNX Runtime TensorRT EP** | NVIDIA，CUDA + TensorRT 10.x | 支持 | 高；通常 1.5–3x | ORT GPU 包 + TensorRT + CUDA/cuDNN | **NVIDIA 性能候选** |
| **Native TensorRT** | NVIDIA | 支持 | 最高，控制最细 | 需自写执行/引擎缓存/绑定，体积大 | 暂不作为第一步 |
| **WinML** | Windows，自动选择 EP | 视 EP | 中 | Windows ML 运行时 | 新 Windows 部署可关注；与现有 C++ 桥集成需额外验证 |
| **OpenVINO** | Intel CPU/GPU | 支持 | Intel 上高 | Intel 运行时 | 不作为跨厂商方案 |
| **DirectML 原生 API** | Windows DX12 | 支持 | 中 | 自写 D3D12/DirectML 调度 | 工作量大于 ORT，暂不选 |

## 关键判断

### 1. 先做 ONNX 导出，而不是直接再写一个 NCNN 路径

NCNN 当前瓶颈是推理本身；继续在 NCNN 内做线程/batch 已验证不可行：

- NCNN `Mat` 没有 batch 维度；
- 多 extractor/worker 会触发 `vkQueueSubmit failed -4`；
- 单队列串行 batch 没有吞吐意义。

因此任何真正的 batch/更高 GPU 利用率都需要换成支持 batch 的 ONNX/TRT 路径。

### 2. DirectML 适合先验证，TensorRT 负责最终性能

- **DirectML**：Windows 原生、AMD/Intel/NVIDIA 都能跑、无需 CUDA；适合先验证 ONNX 模型和 C++ 集成。缺点是 ORT DirectML 已进入维护模式，且性能通常不如 TensorRT。
- **TensorRT EP**：和 DirectML 共用同一个 ONNX 模型，后续只需切换 EP；NVIDIA 上性能潜力最大，但有 CUDA/TensorRT 运行时、引擎缓存和硬件绑定成本。
- **CUDA EP**：如果 TensorRT 引擎构建/兼容性太麻烦，作为 NVIDIA 的中间方案。

推荐路线：

1. 导出 RealESRGAN compact 到 ONNX（支持 batch/动态 H/W）。
2. 用 ORT DirectML 做 **离线** 原型，验证输出质量、速度和 C++ 集成。
3. 同一 ONNX 模型切到 ORT TensorRT EP，在 RTX 4070 SUPER 上对比。
4. 只有 TensorRT EP 仍不够时，才考虑 native TensorRT。
5. NCNN 始终保留为默认回退。

### 3. batch 只在新后端验证

- 离线目标：batch=2（1080p/2K）；4K 先不用 batch，避免显存峰值。
- 实时路径先保持 batch=1，避免增加延迟。
- 只有 batch=2 的吞吐明显优于两次单帧，才保留。

## 集成设计

建议引入一个最小后端接口，不把 ONNX Runtime 变成强制依赖：

```text
ISrBackend
  init(model, options)
  infer(batchRgb, inW, outRgb) -> output dims
  name()/available()
```

- `NcnnSrBackend`：当前 `SR` 包装，默认启用。
- `OrtSrBackend`：可选 DLL/运行时，启动时探测；初始化失败自动回退 NCNN。
- 输出统一为 RGB24 或 planar float，复用现有 `SR::convert`/后处理流水线。
- 任务中心/`meta.tsv` 增加 backend 字段时，需保证旧任务默认 NCNN，续编不换后端。
- ONNX 模型和运行时作为可选发布包，不进入默认安装包；避免体积和许可问题。

## 需要验证的指标

| 指标 | 要求 |
|---|---|
| 正确性 | 与 NCNN 输出逐帧尺寸一致；A/V/PTS/续编不变 |
| 画质 | 人脸毛孔/发丝、文字、夜景；不能出现色偏/棋盘格 |
| 性能 | 1080p/2K/4K 直接 SR；离线 Fast/Balanced/FullSource |
| Batch | batch=2 的帧/秒、显存峰值、首帧延迟 |
| 稳定性 | 取消、seek、断点续编、模型缺失回退 |
| 分发 | DirectML/TensorRT 运行时体积、许可、驱动要求 |

## 环境现状

- 本机有 RTX 4070 SUPER，驱动 610.88。
- Python `onnx` / `onnxruntime` 已有，但当前 ORT 只有 CPU provider，没有 DirectML/CUDA。
- 未检测到 CUDA Toolkit / TensorRT 安装。
- 因此原型阶段需要先安装 `onnxruntime-directml` 或 ORT GPU + TensorRT 运行时。

## 结论

**下一阶段首选：ONNX Runtime DirectML 离线原型；随后用同一模型切 ORT TensorRT EP。**  
不建议继续在 NCNN 内追 batch/多线程，也不建议第一步就写 native TensorRT。
