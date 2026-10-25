# V0.3 M0：兼容性探测与诊断（2026-10）

> 目标：先让“每台机器为什么快/慢/黑屏”可见，再继续做安装包和模型包。

## 本轮范围

1. **后端能力探测**
   - D3D11 adapter 名称、VendorId、显存、Feature Level
   - cudart 是否可加载
   - TensorRT engine 是否 ready
   - NCNN/Vulkan 是否可用
   - 当前实际路径：TensorRT / NCNN / CPU
   - 零拷贝是否真正 active，或是否退化为 CPU 输出

2. **降级原因**
   - TensorRT 初始化失败
   - cudart 缺失/互操作失败
   - D3D adapter 不匹配或混合显卡
   - 美颜/滤镜导致 GPU 路径关闭
   - GPU present fatal / query timeout
   - UI 中显示最近一次原因，而不是静默降速

3. **诊断入口**
   - FFI：`domino_diagnostics(h, buf, cap)` 返回 JSON
   - Flutter：右侧面板新增“诊断”页，显示版本、适配器、backend、SR 状态、FPS、降级原因
   - 支持复制诊断 JSON

4. **测试**
   - JSON 生成纯函数单测（不依赖 GPU）
   - Flutter `FakeEngine` 诊断页 widget 测试
   - 实机冒烟：NVIDIA 正常路径、删除 cudart 的 CPU 回退路径、`DOMINO_D3D_ADAPTER=default`

## 暂不做

- 安装包、代码签名、可选 TensorRT 包（M1）
- 自动/兼容/性能三档的后端切换（M0 先显示状态，M1 再做切换）
- 混合显卡真实笔记本验证（需要外机，先保留诊断字段和逃生开关）
- 4K tile、2x/3x 模型（M2）

## 接口草案

```json
{
  "version": "0.2.4+6",
  "backend": "tensorrt",
  "sr": {"inited": true, "failed": false, "targetW": 3840, "producedFps": 30.1},
  "zeroCopy": {"active": true, "gpuSink": true, "fatal": false, "reason": ""},
  "gpu": {"adapter": "NVIDIA GeForce RTX 4070 SUPER", "vendorId": 4318, "memoryMB": 4096, "featureLevel": "11_0"},
  "cuda": {"cudart": true, "tensorrt": true},
  "vulkan": {"available": true, "gpuCount": 1},
  "fallback": {"reason": "", "lastError": ""}
}
```

字段允许后续追加；Flutter 只展示，不依赖固定 schema。

## 验收

- 诊断页在无视频、播放中、SR 关闭/开启、cudart 缺失时都能打开。
- 状态与 `DOMINO_TRT_PROFILE` 日志一致。
- 删除 cudart 后诊断显示 `cudart=false` + CPU 回退原因，视频仍可播放。
- `ctest` / `flutter test` / `flutter build windows --debug` 通过。
- 实机 4K 仍约 30 FPS，不因诊断探测引入启动卡顿。

## 当前进度（2026-10-25）

- [x] `domino_diagnostics(h, buf, cap)` JSON + Flutter FFI
- [x] 右侧“诊断”页 + 刷新/复制
- [x] D3D11 adapter / VendorId / 显存 / Feature Level
- [x] TensorRT / NCNN / cudart / Vulkan 状态
- [x] 零拷贝 active/fatal/fallback reason
- [x] `DiagnosticsInfo` JSON 纯函数 + C++ 单测
- [x] Flutter FakeEngine 诊断页测试
- [x] 实机 NVIDIA 正常路径
- [x] 删除 cudart：`cudart=false`、`zeroCopy.active=false`、reason=`cudart/D3D interop failed`，视频继续播放
- [ ] 混合显卡真实笔记本（外机）
- [ ] 诊断中加入驱动版本/H.264 硬解状态
- [ ] M1 自动/兼容/性能三档切换
