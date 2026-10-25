# Domino Player

跨平台视频**超分 + 美颜**播放器：**Flutter 前端**（当前 Windows 桌面）+ **可移植 C++ 引擎**（FFmpeg 解码 / 编码 + NCNN Vulkan 超分；NVIDIA RTX 可用 TensorRT-RTX 加速）。

当前重点是把「实时播放」和「离线超分任务」做成一套可用的桌面工具：播放页支持沉浸式全屏、侧边美化 / 播放历史、实时 SR；任务中心支持新建、暂停、断点续编、边播放和删除任务。

![Domino Player 播放界面](docs/screenshots/main.png)

## 功能一览

### 播放与交互

- **本地视频播放**：FFmpeg 解码，音频经 WASAPI 输出，支持播放 / 暂停、seek、音量、循环、动态宽高比。
- **沉浸式全屏**：隐藏首页标签栏和播放页顶栏，只保留视频；控制条 3 秒无操作自动隐藏，移动鼠标 / 点击画面重新显示，`Esc` 或右下角按钮退出。
- **顶部文件栏**：当前文件名和「打开」按钮就在「播放 / 任务中心」标签旁；长文件名自动省略。
- **控制条**：播放 / 暂停、循环、选项、实时 SR、时间、可拉伸 seek、音量、全屏；全屏按钮固定在控制条最右侧，窄窗口也保持可见。
- **侧边选项面板**：从右侧滑入，合并「美化」和「播放历史」两个页签，不打断视频播放。
- **播放历史**：自动记录最近 20 个影片及播放位置，点击历史项重新打开并续播；数据保存在 `%APPDATA%\DominoPlayer\history.json`。
- **系统文件选择框**：通过 FFI 调用 Windows `IFileOpenDialog`，不额外引入文件选择插件。
- **FPS / 状态提示**：左上角显示 FPS；SR 失败、自动降档、打开失败都有界面反馈。

### 美颜与滤镜

- **GPU 着色器美颜**：D3D11 HLSL 两遍高斯磨皮 + 美白 / 滤镜，经 Flutter GPU surface 直接上屏；无 D3D11 或设置 `DOMINO_CPU_BEAUTY=1` 时回退 CPU 路径。
- **磨皮 / 美白**：各自独立滑杆和百分比，互不影响。
- **7 种滤镜**：原图、黑白、日常清新、暖色、净色、复古胶片、粉调；三列网格展示，缩略图按当前画面实时生成。
- **实时预览**：打开面板或调整磨皮 / 美白后自动刷新滤镜缩略图，方便对比效果。

### 实时超分（实验性）

- 播放页可切换 **SR 关闭 / 1080p / 2K / 4K**；NVIDIA RTX 上自动使用 TensorRT-RTX，其他 Vulkan 显卡走 NCNN。
- 解码和 SR 已移到原生 VideoWorker，UI 线程只负责取帧和呈现；seek 带 generation，避免旧帧回跳。
- RTX 4070 SUPER、720p 源：TensorRT-RTX 下 1080p / 2K 约 **29.6 FPS**（源 30fps 上限）；4K 已接入 **TensorRT→D3D11 零拷贝输出**，Flutter App 实机约 **30 FPS**（无 D2H/CPU 转换/整帧上传）。NCNN 回退约 30 / 21.7 / 9 FPS。零拷贝不可用时自动回退 CPU 路径（4K 约 23.6 FPS）。
- **自动降档**：VideoWorker 实际 SR 产出 FPS **低于 15** 持续约 2 秒才降档；4K / 2K 先降到 1080p，1080p 仍低于 15 才关闭 SR。手动选档会清除自动降档状态并重试；启动前 3 秒不触发。
- **SR 慢时保持播放速度**：当 SR 产出低于片源帧率时，VideoWorker 会跳过旧的源帧而不是让媒体时间变慢；保持 1x 播放（可能出现跳帧），避免“低帧率导致视频慢放”。
- 开启 SR 后仍可叠加 GPU 美颜和滤镜。
- 强制兼容模式：设置环境变量 `DOMINO_NO_TRT=1` 可关闭 TensorRT，全部走 NCNN。

### 离线超分任务中心

- **新建任务**：选择源视频、输出分辨率、超分质量（快速 / 平衡 / 高质量）、内容类型（真人/实拍 / 动画）和模型档位（标准 / 高质量）。
- **任务管理**：开始、暂停、断点续编、边播放（编码产物边写边看）、删除；任务状态和进度实时刷新。
- **持久化**：任务信息写入 `meta.tsv`，包括质量档、内容类型、模型和 SR 输入策略；重启或续编时沿用，不会悄悄换模型。
- **断点续编**：取消或失败后保留续编分段，下次启动先合并再继续；主文件替换有备份和回滚。
- **编码设置**：输出 MP4 / fMP4，`faststart`；x264 CRF18 + preset slow，关闭 B 帧并启用 `zerolatency`，避免 fMP4 音视频起点偏移。
- **输出质量**：默认高质量 FullSource 输入（源分辨率，最高 1920 宽），4x 模型推理后用 Lanczos 缩放到目标分辨率；高质量模型档细节更好但更慢。
- **离线流水线**：SR 推理与 float→RGB 后处理拆成两级 worker，并与解码、Lanczos 缩放和 x264 编码重叠；使用有界队列和源/输出缓冲池，按 seq 取回保证 PTS 顺序。NCNN 启用 FP16 算术、cooperative matrix、shader local memory 和 packing layout（不支持时自动回退；设置 `DOMINO_NO_VULKAN_FASTPATH=1` 可关闭快路径）。
- **SR 输出后处理**：float planar → RGB24 的乘 255、裁剪和写出融合为一遍，大图最多 8 线程并行；4K 后处理明显快于旧的 `to_pixels` 双遍路径，输出逐字节一致。
- **TensorRT-RTX**：RTX 机器上标准/HQ 模型自动走 FP16 ONNX + TensorRT-RTX（免 CUDA Toolkit，便携运行时）；2s 720p→1080 FullSource 约 **5.1s**，x4plus 720p→4K 约 **31.9s**；失败自动回退 NCNN。

### 模型库

| 模型 | 用途 | 倍率 | 说明 |
|---|---|---:|---|
| `RealESRGAN_General_x4_v3` | 真人/实拍标准 | 4x | 通用 Real-ESRGAN，实时与离线默认标准模型 |
| `realesr-animevideov3-x2` | 动画标准 | 2x | 目标 ≤ 2x 源宽时自动选择 |
| `realesr-animevideov3-x3` | 动画标准 | 3x | 目标 ≤ 3x 源宽时自动选择 |
| `realesr-animevideov3-x4` | 动画标准 | 4x | 更大放大倍率 |
| `realesrgan-x4plus` | 真人/实拍高质量 | 4x | 输入上限 960，慢但细节更强 |
| `realesrgan-x4plus-anime` | 动画高质量 | 4x | 输入上限 960，动画线条更干净 |
| `realesr-general-x4v3-fp16.onnx` | TensorRT 真人标准 | 4x | 与 NCNN 标准模型同源，TensorRT 用 |
| `realesrgan-x4plus-fp16.onnx` | TensorRT 真人高质量 | 4x | 与 NCNN HQ 模型同源，TensorRT 用 |

- 动画标准会按 `目标宽 / 源宽` 自动选 x2 / x3 / x4；真人标准目前只有 x4。
- 高质量档 `x4plus` / `x4plus-anime` 的输入宽限制为 960，超出会先缩放，避免显存溢出。
- 模型文件和许可说明在 [`assets/models/sr/ncnn/`](assets/models/sr/ncnn/)；任务续编会沿用创建时选择的模型。

### 可移植引擎

- `domino_core` 不依赖 D3D11 / WASAPI：视频经 `IFrameOutput` 输出，音频经 `AudioSink` 注入，任意前端都可链接。
- Windows 使用 WASAPI + Flutter 纹理；后续移动端只需注入对应 `AudioSink` / 视频输出实现。
- Flutter 通过 `domino_engine.dll` 的 `extern "C"` FFI 桥调用引擎；GPU 美颜使用 D3D11 共享纹理上屏。
- A/V 同步按音频时钟和 PTS 呈现；暂停 / 恢复 / seek 都会重置时钟锚点。

## 截图

<p align="center">
  <img src="docs/screenshots/main.png" width="49%" alt="播放页：顶栏文件、控制条和实时 FPS" />
  <img src="docs/screenshots/beauty.png" width="49%" alt="美化面板：磨皮/美白滑杆与三列滤镜" />
</p>
<p align="center">
  <img src="docs/screenshots/history.png" width="49%" alt="播放历史侧栏" />
  <img src="docs/screenshots/task-center.png" width="49%" alt="超分任务中心新建任务" />
</p>

![全屏播放界面](docs/screenshots/fullscreen.png)

> 截图中的演示视频来自 Blender Foundation 的 *Big Buck Bunny*（CC BY 3.0），仅用于界面展示，不随仓库分发。

## 下载（Windows 预编译包）

无需自行编译：到 [**Releases**](https://github.com/Rhett-12138/domino-player/releases) 下载
`domino-player-vX.Y.Z-win64.zip`，解压后双击 `domino_flutter.exe` 即可运行（详见包内 `运行说明.txt`）。

- 需 64 位 Windows 10/11；超分需支持 Vulkan 的独立显卡。
- 若提示缺少 `VCRUNTIME140/MSVCP140`，安装 [Microsoft Visual C++ 可再发行组件 (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe)。
- 未签名 exe 首次运行可能触发 SmartScreen：点「更多信息 → 仍要运行」。

## 前置条件

- **Visual Studio 2022**（MSVC，含「使用 C++ 的桌面开发」+ C++ CMake 工具）
- **CMake** ≥ 3.20
- **Flutter SDK**（stable）
- 支持 **Vulkan** 的独立显卡（NCNN 超分）
- 预编译依赖置于 `libs/`（**git 忽略，需自行放置**）：
  - `libs/ffmpeg/ffmpeg-master-latest-win64-gpl-shared/`
  - `libs/ncnn/ncnn-20260526-windows-vs2022-shared/`
  - 可选：`libs/tensorrt-rtx/`（NVIDIA TensorRT for RTX，Windows x64，CUDA 12.9 版；本地放置，git 忽略）

超分模型放在 `assets/models/sr/ncnn/`，TensorRT 的 FP16 ONNX 放在 `assets/models/sr/onnx/`；包括标准、动画 x2/x3/x4 和高质量模型（见上表）。

TensorRT-RTX 是可选的 NVIDIA 加速后端：没有 `libs/tensorrt-rtx/` 时仍可完整构建和运行（NCNN 回退）。运行时不需要安装 CUDA Toolkit，但需要 NVIDIA RTX 显卡和驱动；从 [NVIDIA TensorRT for RTX](https://developer.nvidia.com/tensorrt-rtx) 下载 Windows x64 CUDA 12.9 包，解压后把 `bin/`、`include/`、`lib/` 放到 `libs/tensorrt-rtx/` 即可。

## 构建与运行

### Flutter 前端（主）

```bash
cd flutter
flutter run -d windows            # 开发运行（热重载）
flutter build windows --debug     # 产物：build/windows/x64/runner/Debug/domino_flutter.exe
```

引擎桥 DLL（`domino_engine.dll`）、FFmpeg/NCNN 运行库与模型由构建自动拷到 runner 目录，直接运行 exe 即可。

### C++ 引擎 + 单元测试

```bash
cmake -S . -B build
cmake --build build --target domino_tests
ctest --test-dir build -C Release --output-on-failure      # ~5s，应 0 failed
```

## 测试

| 层 | 命令 | 说明 |
|---|---|---|
| C++ 引擎 | `ctest --test-dir build -C Release` | decode/encode/task/player/config/beauty/frame_output/sr，headless |
| Flutter | `cd flutter && flutter test` | 纯 Dart，无需 DLL；用 `FakeEngine` 桩驱动 UI |

当前 Flutter 测试覆盖：播放控制、全屏 / 自动隐藏、侧边面板、播放历史、美化布局、窄窗口布局、任务中心和 SR 状态等。测试细节见 [`tests/README.md`](tests/README.md)。

## 目录结构

```
├── src/                  # 可移植 C++ 引擎（domino_core）
│   ├── engine.h          # IFrameOutput 视频输出抽象
│   ├── audio_interface.h # AudioSink 音频抽象（Windows=WASAPI，可注入）
│   ├── player.{h,cpp}    # 播放核心（解码/超分/调度/A·V 同步/seek）
│   ├── frame_output.*    # 视频帧输出 / CPU 美颜
│   ├── beauty_math.h     # 美颜/滤镜纯数学（可单测）
│   └── decode / encode / sr / task / config / log
├── flutter/              # Flutter 前端
│   ├── lib/main.dart             # 播放页、首页、全屏、选项面板
│   ├── lib/core/engine.dart      # EngineApi 抽象 + domino_engine.dll FFI 绑定
│   ├── lib/core/history.dart     # 播放历史持久化
│   ├── lib/core/format.dart      # 展示层纯函数
│   ├── lib/ui/task_center.dart   # 任务中心
│   ├── native/engine_bridge.*    # C++ 桥（FFI + 纹理 + 文件框 + 全屏）
│   └── test/                     # Dart 测试
├── tests/                # C++ 单测 + assets + GUI 自动化脚本
├── libs/                 # 依赖（ffmpeg/ncnn 本地放置，git 忽略）
├── assets/models/sr/ncnn/# 超分模型
├── docs/screenshots/     # README 截图
└── docs/                 # roadmap / project-structure / plans
```

## 安全与隐私

- **离线运行**：播放、超分、美颜和任务编码都在本地完成；代码中没有遥测、账号系统或上传视频的接口。
- **本地数据**：播放历史写到 `%APPDATA%\DominoPlayer\history.json`，离线任务写到 `%APPDATA%\DominoPlayer\sr-tasks\`；删除对应目录即可清空。
- **无密钥**：仓库不包含 API Key、令牌、私钥或云服务凭据；FFmpeg / NCNN 运行库和模型二进制按各自许可获取。
- **路径建议**：本地构建/调试时不要把个人绝对路径写进源码或提交信息；模型/数据目录优先通过环境变量或本地未跟踪文件配置。
- **历史作者邮箱**：Git 历史中的提交作者信息会随仓库公开。如果不想公开个人邮箱，请在首次公开 / 强制推送前用 GitHub noreply 地址改写提交作者，或使用仓库的 `.mailmap` 做映射（`.mailmap` 只影响显示，不改变 Git 对象）。

第三方组件和模型许可见 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。特别注意 FFmpeg GPL 构建的分发约束。

## 运行诊断

播放页右下角 `选项 → 诊断` 可查看当前运行路径：

- D3D11 adapter / VendorId / 显存 / Feature Level
- TensorRT / NCNN 实际 backend
- cudart、TensorRT、Vulkan 可用性
- SR 目标、零拷贝是否 active、GPU fatal、降级原因

诊断 JSON 可一键复制，用于定位“为什么只有 20 FPS”“为什么回退 CPU”等问题。

## 已知限制

- TensorRT-RTX 4K 零拷贝约 30 FPS（RTX 4070 SUPER，720p 源）；复杂片源仍保留小于 15 FPS 自动降档。零拷贝需要 NVIDIA GPU 和随包 `cudart64_12.dll`，不可用时自动回退 CPU（4K 约 23.6 FPS）。
- 竖屏/超宽按 4K 高度/宽度上限输出（如竖屏 2424×4320），像素量更高时帧率会低于 30 FPS。
- TensorRT-RTX 运行时约 220MB，发布包会明显变大；后续可拆成可选高性能组件。首次使用需要构建 engine 缓存（约 12–20s）。
- 真人 2x/3x 暂未接入可用模型；动画 x2/x3 已支持，真人 1.5x 可用 2x 后缩放。
- `x4plus` / `x4plus-anime` 输入宽限制 960，质量更好但明显更慢；1080p→4K FullSource 中间帧较大，峰值内存约 1.9GB。
- 移动端尚未实机验证；引擎接口已按可移植方式拆分。
- 当前只提供 Windows 预编译包；其他平台需自行构建并注入 `AudioSink`。

## 文档

- [`AGENTS.md`](AGENTS.md) — 项目宪章：目标、硬约束、DoD、已知坑
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — 贡献指南（环境/构建/DoD/提交规范）
- [`docs/roadmap.md`](docs/roadmap.md) — 当前状态与里程碑
- [`docs/project-structure.md`](docs/project-structure.md) — 分层与文件说明
- [`tests/README.md`](tests/README.md) — 测试策略

## 许可

本项目源码以 [MIT](LICENSE) 发布。第三方组件（FFmpeg / NCNN / Flutter / TensorRT-RTX / 模型权重）各有其许可，
**分发二进制前请先读** [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)（尤其 FFmpeg GPL 的传染性和 TensorRT-RTX 运行时分发条款）。
