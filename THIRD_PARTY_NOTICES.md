# 第三方组件与许可

本项目源码以 [MIT](LICENSE) 发布。以下第三方组件各有其许可，分发时请一并遵守。

| 组件 | 用途 | 许可 | 位置 |
|---|---|---|---|
| [FFmpeg](https://ffmpeg.org/) | 解码 / 编码 / 封装 | **GPLv3**（本项目使用 `gpl-shared` 预编译版） | `libs/ffmpeg/`（本地放置，**未提交**） |
| [NCNN](https://github.com/Tencent/ncnn) | Vulkan 推理（超分） | BSD-3-Clause | `libs/ncnn/`（本地放置，**未提交**） |
| [Flutter](https://flutter.dev/) | 前端框架 | BSD-3-Clause | `flutter/` |
| [RealESRGAN_General_x4_v3](https://github.com/xinntao/Real-ESRGAN) | 通用 4x 超分模型 | BSD-3-Clause | `assets/models/sr/ncnn/` |
| [realesr-animevideov3-x4](https://github.com/xinntao/Real-ESRGAN) | 动漫视频 4x 超分模型 | BSD-3-Clause | `assets/models/sr/ncnn/` |
| [MediaPipe face_landmarker](https://developers.google.com/mediapipe) | 人脸关键点（规划中） | Apache-2.0 | `assets/models/face_landmarker.task` |

## ⚠️ 分发注意（FFmpeg GPL）

本项目默认使用 FFmpeg 的 **GPL 共享库构建**（`ffmpeg-master-latest-win64-gpl-shared`）。
FFmpeg 的 GPL 构建带有传染性：**若分发与 GPL FFmpeg 链接的可执行文件，整个分发需以 GPLv3 兼容方式提供**（含相应源码）。

- 仅发布**源码**（不含 FFmpeg 二进制）：本 MIT 许可即可，克隆者自行获取 FFmpeg。
- 发布**二进制**：请改用 FFmpeg 的 **LGPL** 构建（`*-lgpl-shared`）并遵守 LGPL（动态链接 + 可替换），或按 GPLv3 提供完整对应源码。

模型权重的再分发请保留其原始署名（Real-ESRGAN 论文/仓库、Google MediaPipe）。
