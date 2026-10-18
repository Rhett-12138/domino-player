# SR 模型来源与许可

本目录包含离线超分使用的 NCNN 模型：

- `RealESRGAN_General_x4_v3.*`
- `realesr-animevideov3-x2.*` / `x3.*` / `x4.*`（动画标准，按目标比例自动选）
- `realesrgan-x4plus.*`（高质量真人/通用）
- `realesrgan-x4plus-anime.*`（高质量动画）

模型来源：

- Real-ESRGAN：https://github.com/xinntao/Real-ESRGAN
- NCNN Vulkan 模型包：https://github.com/xinntao/Real-ESRGAN/releases/tag/v0.2.5.0

许可：

- Real-ESRGAN 代码与模型：BSD 3-Clause（见 `LICENSE-Real-ESRGAN-ncnn-vulkan.txt`；
  模型权重按其上游发布说明使用）。
- NCNN：BSD 3-Clause。

高质量模型体积较大（x4plus 约 33MB，x4plus-anime 约 9MB），且推理明显慢于标准模型；
离线默认仍使用标准模型，用户可在任务中心选择“高质量”。
