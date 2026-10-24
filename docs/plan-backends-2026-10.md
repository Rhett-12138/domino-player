# 可选推理后端评估（2026-10）

> 决策：**先做高性能 TensorRT/CUDA，兼容性后续再补。**  
> 当前基线：RTX 4070 SUPER，NCNN Vulkan，1080p/2K/4K 直接 SR 约 20/44/109ms，离线 2s 720p→1080 FullSource 约 11.3s。

## 结论

1. **首选 TensorRT**：在 RTX 上性能最高，支持 FP16、静态/动态 shape、batch 和 engine cache。
2. **CUDA EP 作为中间方案**：如果 TensorRT 集成/版本兼容太麻烦，先用 ONNX Runtime CUDA EP 验证。
3. **NCNN 保留最终回退**：现有功能不能因缺少 CUDA/TensorRT 而不可用。
4. **DirectML 暂缓**：适合以后补 AMD/Intel 兼容性，但当前性能不是最优，且 ORT DirectML 处于维护模式。
5. **不先做 native CUDA 自定义 kernel**：先用 ONNX + TensorRT/CUDA EP，减少维护面。

## 候选对比

| 后端 | 硬件 | Batch | 性能潜力 | 依赖/分发 | 本轮结论 |
|---|---|---:|---|---|---|
| **TensorRT（native 或 ORT EP）** | NVIDIA RTX/数据中心 | 支持 | **最高** | CUDA + TensorRT；ORT EP 版本矩阵、engine cache | **首选** |
| **ONNX Runtime CUDA EP** | NVIDIA | 支持 | 中高 | ORT GPU + CUDA/cuDNN | 次选/中间验证 |
| NCNN Vulkan（现状） | Vulkan 全厂商 | 无 | 基准 | 已随包 | 回退 |
| ORT DirectML | Windows DX12 全厂商 | 支持 | 中 | ORT + DirectML，无 CUDA | 暂缓 |
| OpenVINO | Intel | 支持 | Intel 上高 | Intel 运行时 | 不选 |
| WinML | Windows | 视 EP | 中 | Windows ML 打包 | 后续观察 |

## 推荐路线

### Phase 1：ONNX 导出（已开始）

已增加：

- `tools/export_onnx.py`：把 RealESRGAN compact checkpoint 导出为带动态 batch/H/W 的 ONNX。
- `tools/bench_onnx.py`：按 provider 跑 batch、输入尺寸和循环基准。

已用 `realesr-general-x4v3.pth` 验证 ONNX 导出（输入 `1×3×64×64`，输出 `1×3×256×256`，opset 17）。

### Phase 2：TensorRT / CUDA 基准

在 RTX 4070 SUPER 上安装：

- CUDA 12.9（或与 TensorRT 版本匹配的 CUDA）
- TensorRT 10.9 / 11.x（或 TensorRT for RTX）
- `onnxruntime-gpu`（CUDA/TensorRT EP）

然后跑同一 ONNX 模型：

```bash
python tools/bench_onnx.py --model ...onnx --provider tensorrt --width 960 --height 540 --batch 1
python tools/bench_onnx.py --model ...onnx --provider tensorrt --width 960 --height 540 --batch 2
python tools/bench_onnx.py --model ...onnx --provider cuda --width 960 --height 540 --batch 1
```

对比 NCNN 的 1080p/2K/4K 直接 SR 和 2s 离线 FullSource。

### Phase 3：接入可选后端

新增 `ISrBackend`：

```text
init(model, options)
infer(batchRGB, inputW) -> RGB/planar output
backendName()
```

- `NcnnSrBackend`：现有 `SR`，默认回退。
- `TensorRtSrBackend` / `OrtSrBackend`：可选 DLL/运行时；初始化失败自动回退 NCNN。
- 统一输出 RGB24，复用现有后处理/编码流水线。
- 任务 `meta.tsv` 记录 backend；旧任务默认 NCNN，续编不换后端。
- 普通用户默认自动选择；高级设置才显示 TensorRT/CUDA 状态。

### Phase 4：性能参数

- FP16 优先；INT8 只在画质验证通过后考虑。
- TensorRT engine cache 放在 `%APPDATA%\DominoPlayer\trt-cache`，按 GPU/模型/精度/形状区分。
- 离线 batch=2 先验证；实时保持 batch=1。
- 4K 先不 batch，避免显存峰值。
- 动态 shape 用 profile min/opt/max；固定输出档位可进一步用静态 shape。

## 当前环境阻塞

- 驱动：NVIDIA 610.88。
- 当前 Python `onnxruntime` 只有 CPU provider；没有 CUDA/DirectML。
- 未安装 CUDA Toolkit / TensorRT。
- TensorRT Windows 包和 CUDA 运行时体积较大；需要先安装/下载后才能做真实 GPU 基准。

因此当前已经完成 **ONNX 导出工具和基准脚本**；真实 TensorRT/CUDA 数据必须在安装运行时后补齐，不能用 CPU 数字代替。

## 风险

- TensorRT engine 与 GPU/驱动/TensorRT 版本绑定，首次构建需要缓存和超时处理。
- 运行时体积、许可和 Windows 分发成本。
- 动态 shape 和 batch 的显存峰值；4K 需单独限制。
- ORT TensorRT EP 与 TensorRT 版本矩阵需要固定，否则可能回退 CUDA/CPU。
- 画质必须用真人脸部/发丝/文字/夜景对比，不能只看速度。

## 结论

**先做 TensorRT；CUDA EP 作为后备。**  
当前代码层面已完成 ONNX 导出/基准工具，下一步是安装 CUDA + TensorRT 运行时，在 RTX 4070 SUPER 上得到真实 batch=1/2 数据，再决定是否接 native TensorRT 还是 ORT TensorRT EP。
