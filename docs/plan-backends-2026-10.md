# 可选推理后端评估（2026-10）

> 目标：**普适性优先、用户零额外安装**，在不破坏现有 NCNN Vulkan 回退和便携架构的前提下评估后端。
> 当前基线：RTX 4070 SUPER，NCNN Vulkan，1080p/2K/4K 直接 SR 约 20/44/109ms，离线 2s 720p→1080 FullSource 约 11.3s。

## 候选对比

| 后端 | 平台/硬件 | Batch | 性能潜力 | 依赖/分发 | 结论 |
|---|---|---|---|---|---|
| **NCNN Vulkan**（现状） | Windows/Linux/Android，Vulkan 厂商 | 无（Mat 无 batch 维） | 基准 | 已随包，用户零额外安装 | **默认/首选** |
| **ONNX Runtime DirectML** | Windows 10/11，DX12 全厂商 | 支持，建议静态 batch | 中；通常低于 TensorRT，需实测 | ORT + DirectML，可随包分发，无需 CUDA | **免 CUDA fallback** |
| **ONNX Runtime CUDA** | NVIDIA，CUDA 12 + cuDNN | 支持 | 中高 | 需用户安装 CUDA/cuDNN | 仅高级可选 |
| **ONNX Runtime TensorRT EP** | NVIDIA，CUDA + TensorRT 10.x | 支持 | 高；通常 1.5–3x | 需用户安装 CUDA + TensorRT，引擎缓存/硬件绑定 | 仅高级可选 |
| **Native TensorRT** | NVIDIA | 支持 | 最高，控制最细 | 自写执行/引擎缓存/绑定，体积大 | 不采用 |
| **WinML** | Windows，自动选择 EP | 视 EP | 中 | Windows ML 运行时/打包要求 | 观察，不先做 |
| **OpenVINO** | Intel CPU/GPU | 支持 | Intel 上高 | Intel 运行时 | 不作为跨厂商方案 |
| **DirectML 原生 API** | Windows DX12 | 支持 | 中 | 自写 D3D12/DirectML 调度 | 工作量大于 ORT，不采用 |

## 关键判断

### 1. 先做 ONNX 导出，而不是直接再写一个 NCNN 路径

NCNN 当前瓶颈是推理本身；继续在 NCNN 内做线程/batch 已验证不可行：

- NCNN `Mat` 没有 batch 维度；
- 多 extractor/worker 会触发 `vkQueueSubmit failed -4`；
- 单队列串行 batch 没有吞吐意义。

因此任何真正的 batch/更高 GPU 利用率都需要换成支持 batch 的 ONNX/TRT 路径。

### 2. 普适性优先：NCNN 默认，DirectML 只做免 CUDA fallback

普通用户不应该为了超分安装 CUDA、cuDNN 或 TensorRT。当前 NCNN 已经随包分发，支持 Vulkan 的 NVIDIA/AMD/Intel 显卡都能直接用，这是最低门槛路径。

- **NCNN Vulkan（默认）**：零额外安装；性能已经优化到单帧瓶颈；继续保留。
- **DirectML（可选 fallback）**：Windows DX12 全厂商、无需 CUDA；可以随包分发，但会增加包体和运行时；应自动探测，不能要求用户手动配置。DirectML EP 已进入维护模式，因此只作为兼容性后备，不作为唯一主路径。
- **CUDA/TensorRT（高级可选）**：只面向愿意安装 NVIDIA 运行时的用户，不能成为默认安装的前置条件；不能因为缺少 CUDA 阻止播放或普通超分。
- **WinML**：Windows 的长期方向值得关注，但当前打包/集成成本高于 ORT DirectML，先观察。

推荐路线：

1. **保留 NCNN Vulkan 默认**，把“用户无需安装任何 GPU SDK”作为硬约束。
2. 导出 ONNX 模型，做 **ORT DirectML 可选 fallback**，随包分发或作为可选组件；启动时自动探测，失败回退 NCNN/关闭 SR。
3. 在检测到 NVIDIA 且用户主动选择“性能模式”时，才尝试 ORT CUDA/TensorRT EP；TensorRT 仅作为可选性能包。
4. 普通用户界面不出现 CUDA/cuDNN/TensorRT 选择，只显示“自动 / 兼容模式 / 性能模式（可选下载）”。
5. 没有 Vulkan/DX12 的机器仍可播放；SR 不可用时给出明确提示，不阻塞核心播放功能。

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

**默认继续用 NCNN Vulkan：用户不需要安装 CUDA/TensorRT，兼容 Vulkan 的 NVIDIA/AMD/Intel 都能直接播放和超分。**

下一阶段只做 **ORT DirectML 免 CUDA fallback 原型**，用来覆盖 Vulkan 不可用或 NCNN 初始化失败的 Windows 机器；TensorRT/CUDA 仅作为 NVIDIA 高级可选性能包，不作为默认前置条件。

不建议：
- 为追求极限性能把 TensorRT 设为默认依赖；
- 让用户手动安装 CUDA/cuDNN；
- 继续在 NCNN 内追 batch/多线程。

这样能同时满足“普适性、低门槛”和“高性能可扩展”。
