# TensorRT ONNX 模型来源

本目录包含 TensorRT-RTX 使用的 FP16 ONNX 模型：

- `realesr-general-x4v3-fp16.onnx`
  - 来源：Real-ESRGAN `realesr-general-x4v3.pth`
  - 上游：https://github.com/xinntao/Real-ESRGAN/releases/tag/v0.2.5.0
- `realesrgan-x4plus-fp16.onnx`
  - 来源：Real-ESRGAN `RealESRGAN_x4plus.pth`
  - 上游：https://github.com/xinntao/Real-ESRGAN/releases/tag/v0.1.0

转换方式：

```bash
python tools/export_onnx.py --arch compact \
  --pth realesr-general-x4v3.pth \
  --out assets/models/sr/onnx/realesr-general-x4v3-fp16.onnx --fp16
python tools/export_onnx.py --arch rrdb \
  --pth RealESRGAN_x4plus.pth \
  --out assets/models/sr/onnx/realesrgan-x4plus-fp16.onnx --fp16
```

Real-ESRGAN 代码与模型权重按 BSD 3-Clause 使用；再分发时保留原始署名。NCNN 模型仍在
`assets/models/sr/ncnn/`，TensorRT 不可用时回退到 NCNN。
