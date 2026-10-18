# 真人向 2x/3x 模型寻找与验证（2026-10）

> 目标：为“真人/实拍”标准档找可用的 NCNN 2x/3x 模型，降低 1080→4K、720→2K/4K 的耗时。
> 当前结论：**SRMD 2x/3x 是合适的候选，但与当前 NCNN 版本不兼容；Real-ESRGAN x2plus 的 NCNN 文件暂未拿到。真人标准仍保持 4x。**

## 候选对比

| 候选 | Scale | 体积 | 真人适配 | NCNN 可用性 | 结论 |
|---|---:|---:|---|---|---|
| **SRMD**（nihui/srmd-ncnn-vulkan） | 2x/3x/4x | 3MB/个 | 好（Real-World SR） | 模型是 2022 旧格式；当前 NCNN 2026 `extract` 失败，旧版自带的 exe 可运行 | **暂不集成** |
| **Real-ESRGAN x2plus NCNN**（HuggingFace bpawnzZ） | 2x | 待确认 | 好（非动画） | 理想目标；当前网络无法稳定下载，未验证 | **优先获取** |
| Real-CUGAN 2x/3x（NCNN） | 2x/3x | 3–5MB/个 | 一般，偏动画/降噪 | 官方 NCNN 包可用 | 备选，真人需盲测 |
| RealSR（nihui） | 4x | 64MB 包 | 好 | 有 NCNN，但只有 4x | 不满足 2x/3x |
| SwinIR 2x/3x | 2x/3x | 大 | 好 | 主要是 PyTorch/ONNX，无现成 NCNN | 转换成本高 |
| 现有 4x Fast | 4x | 已有 | 好 | 可用 | 质量较低的快速兜底 |

## Real-CUGAN 实测（真人样本）

用本机 1080p 真人脸片段，目标 4K，输入 1920→2x→3840：

- 模型：`models-pro/up2x-no-denoise`（避免降噪掩盖问题）、`up2x-conservative`
- 结果：
  - 单帧输出因 Real-CUGAN 边界裁剪是 **3768×2088**，不是精确 3840×2160（已用实际输出重建缩放器）；
  - 速度：0.5s 片段 Full 约 **5.8s**（no-denoise）/ **5.6s**（conservative）；
  - 对比：General 4x Full 约 9.6s，General 4x Fast 约 3.7s；
- 画质：
  - 皮肤、头发、牙齿明显**过度平滑/塑料感**；
  - 真人纹理和细碎发丝被抹掉，眼睛/牙齿边缘变软；
  - conservative 也仍有明显“动画修复”痕迹。
- 结论：**不推荐把 Real-CUGAN 作为真人模型**；可以作为动画备选，但当前动画已有 animevideov3。

## SRMD 实测

- 来源：`https://github.com/nihui/srmd-ncnn-vulkan/releases/tag/20220728`
- 模型：`srmd_x2.param/bin`、`srmd_x3.param/bin`，每个约 3MB，许可证随包。
- 自带 2022 版 exe 对单帧输出正常。
- 集成到本项目（NCNN 2026）时：
  - 参数加载成功；
  - `input`/`output` blob 正确；
  - `extract` 失败/崩溃（Vulkan/CPU 均复现）。
- 判断：旧 SRMD 模型与当前 NCNN 运行时不兼容，不能直接上线；需要：
  1. 用当前 ncnn 工具重转模型；或
  2. 单独链接旧 ncnn（不推荐）；或
  3. 找现代 NCNN 的 2x/3x 真人模型。

> 为避免任务误用失败模型，本次没有把 SRMD 加入模型注册表/自动选择。

## 推荐路线

1. **首选 Real-ESRGAN x2plus NCNN**（真人、2x、BSD-3）：
   - 1080p→4K 正好 1920→3840；
   - 720p→2K 正好 1280→2560；
   - 比 4x FullSource 少一半输出像素，且不产生 7680 中间帧。
   - 需要拿到 `.param/.bin`，或从 PyTorch 权重转换。
2. 若拿不到 x2plus：测 Real-CUGAN 2x/3x，但必须用真人/文字/夜景验证，避免“涂抹”。
3. SRMD 仅在完成 NCNN 兼容转换后再考虑。
4. 1.5x 仍用 2x 输出缩放，不做原生模型。

## 工作量

| 工作 | 工作量 |
|---|---:|
| 获取并验证 Real-ESRGAN x2plus NCNN | 2–4 人日（含转换/画质） |
| SRMD 旧模型重转/兼容 | 2–3 人日 |
| Real-CUGAN 2x/3x 接入 + 真人盲测 | 2–4 人日 |
| 通用 scale 选择 | 已完成（动画 x2/x3） |

## 当前状态

- 动画标准：x2/x3/x4 自动选择，已上线；
- 真人标准：仍 4x（General x4）；
- 真人高质量：x4plus，4x；
- Real-CUGAN 2x 已验证但真人效果不合格（过度平滑）；
- 代码已支持任意 `scale`，只差可用真人 2x/3x 模型文件。

## 验证标准

- 速度：同分辨率/同输入策略对比 4x FullSource；
- 画质：真人脸（毛孔/发丝）、文字、夜景各一组；
- 不能只看 PSNR；需要检查过度锐化、塑料感、噪声放大和闪烁；
- 通过后再进入自动选择，模型缺失时回退 4x 并提示。
