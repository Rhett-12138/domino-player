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
- 自动/兼容/性能三档已提前纳入 M0（切换在下次 open 重建后端）；M1 再做持久化与安装器默认值。
- 混合显卡真实笔记本验证（需要外机，先保留诊断字段和逃生开关）
- 4K tile、2x/3x 模型（M2）

## 接口草案

```json
{
  "version": "0.2.4",
  "backend": "tensorrt",
  "mode": "auto",
  "modeActive": "auto",
  "modePending": false,
  "output": "d3d11",
  "sr": {"inited": true, "failed": false, "beauty": false, "targetW": 3840},
  "zeroCopy": {"active": true, "gpuSink": true, "fatal": false, "reason": ""},
  "gpu": {"adapter": "NVIDIA GeForce RTX 4070 SUPER", "driver": "32.0.16.1088", "mode": "nvidia", "vendorId": 4318, "memoryMB": 4096, "featureLevel": "11_0"},
  "decode": {"h264Hw": true, "inUse": false, "reason": ""},
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

## 实机记录（2026-10-25，RTX 4070 SUPER）

- 用户提供的 1080p H.264 长片 → 4K SR：`backend=tensorrt`、`output=d3d11`、`zeroCopy.active=true`，实测约 19.8–23.3 FPS；播放位置保持 1x（10 分钟观察与 seek 后均正常）。
- 驱动 `32.0.16.1088`；D3D11VA `h264Hw=true`，但当前仍走软件解码（`inUse=false`）。
- 美颜 65%：零拷贝关闭，`reason=beauty/filter active`，FPS 约 15.1；恢复 0% 后零拷贝重新 active，FPS 约 20.7–21.3。
- 删除 cudart：`cudart=false`、`zeroCopy.active=false`、reason=`cudart/D3D interop failed`，视频继续播放。
- 1 小时长稳：**通过**，60.1 分钟、358 个采样；起始/结束均 `targetW=3840 + zeroCopy.active=true`，期间 5 次 seek、循环开启；工作集 957.6–999.9 MB、私有内存 2146.7–2458.2 MB（末值 2178 MB）、句柄 1463–1570（末值 1466）、线程 153–173（末值 155）、GPU 总计 3424–3548 MB（末值 3427 MB），无持续增长；结束时约 23.4 FPS。

## 自动验证（2026-10-25）

- `ctest --test-dir build -C Release`：89.0s，通过（新增 backend mode / performance failure / diagnostics JSON 用例）。
- `cd flutter && flutter test`：46 passed（新增诊断页与后端策略待生效测试）。
- `flutter build windows --debug`：成功；runner 下 `domino_engine.dll` 时间戳已更新。
- 1h soak：`tests/gui/soak_4k.ps1`，开始/结束诊断均为 `targetW=3840 + zeroCopy.active=true`。

## 验收矩阵

| 场景 | 结果 |
|---|---|
| NVIDIA + TensorRT + 4K 零拷贝 | ✅ 本机 RTX 4070 SUPER，19.8–23.3 FPS，1x |
| 缺 cudart → CPU | ✅ 视频继续，诊断显示原因 |
| 美颜开/关 | ✅ 65% 时零拷贝关闭；0% 后恢复 |
| seek | ✅ 4K 目标下跳到 87:59，继续播放 |
| 自动降档 | ✅ 单元测试（<15 FPS 2s） |
| 自动/兼容/性能 | ✅ 实测：auto=tensorrt/4K/zeroCopy；compat=ncnn/1080p/zeroCopy off；performance=tensorrt/4K/zeroCopy |
| 1 小时长稳 | ✅ 60.1 分钟，4K 零拷贝，5 次 seek；资源无持续增长 |
| 混合显卡笔记本 | ⏳ 需要外机；当前桌面 `DOMINO_D3D_ADAPTER=default` 实测仍选 RTX 4070 SUPER |
| 任务边播 | ⏳ 未纳入本轮长稳 |

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
- [x] 诊断加入驱动版本（D3D UMD）和 D3D11VA H.264 能力；当前解码仍为软件（`inUse=false`）
- [x] 后端策略 `自动 / 兼容 / 性能`：FFI + 诊断页下拉；切换在下次 `open` 重建后端
- [x] 长稳脚本 `tests/gui/soak_4k.ps1`：4K SR、循环、周期 seek、进程/GPU 资源采样；启动/结束校验 4K 零拷贝
- [x] 1 小时 4K 长稳：通过（60.1min，358 samples，5 seeks，资源稳定）
- [ ] 真实混合显卡笔记本（当前桌面 `DOMINO_D3D_ADAPTER=default` 已验证适配器选择；核显默认笔记本仍待外机）
- [ ] 任务边播纳入长稳（美颜开关已在 4K 播放中单独验证）
