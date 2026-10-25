# Domino Player v0.3.0-beta.1

> Windows x64 beta。**未签名**，不是 V0.3 稳定版；请用于测试和反馈，不建议作为唯一播放器长期使用。

## 重点

- 新增运行诊断：GPU/adapter、驱动版本、backend、cudart/TensorRT/Vulkan、D3D11VA H.264 能力、零拷贝状态、降级原因。
- 新增实时后端策略：自动 / 兼容 / 性能；切换在下次打开视频时重建后端。
- 安装包：按当前用户安装，不需要管理员权限；开始菜单、可选桌面快捷方式、卸载。
- 绿色 zip 继续保留；安装包和 zip 使用同一份运行时、模型和说明。
- VC++ Runtime 缺失检测：提示并打开 Microsoft 官方下载后中止安装。
- 4K TensorRT 零拷贝长稳：60.1 分钟、5 次 seek、4K 目标与 zero-copy 状态首尾一致，资源无持续增长。

## 安装

推荐安装包：

```text
domino-player-v0.3.0-beta.1-win64-setup.exe
```

或解压绿色包后双击 `domino_flutter.exe`：

```text
domino-player-v0.3.0-beta.1-win64.zip
```

需要 64 位 Windows 10/11（1809+）；缺失 VC++ 2015-2022 x64 运行库时安装器会提示。

已有 `v0.2.4` 用户：本次是 beta，安装器使用独立 per-user 安装目录；不会自动删除 `%APPDATA%\DominoPlayer` 中的历史、任务和 TensorRT 缓存。

## 已知限制

- **未签名**：SmartScreen 可能提示“Windows 已保护你的电脑”；请核对发布页和 SHA256。
- **混合显卡未完成真实笔记本验证**：当前只在 RTX 4070 SUPER 桌面和 `DOMINO_D3D_ADAPTER=default` 上验证；核显为默认 adapter 的笔记本仍需要反馈。
- **任务中心边播尚未纳入 1 小时长稳**：1h 测试覆盖 4K 零拷贝、循环和 seek。
- **H.264 只有能力探测**：诊断显示 `h264Hw=true`，但当前解码仍走 FFmpeg 软件路径（`inUse=false`）。
- **未绑定文件关联**：当前不解析命令行文件参数，避免双击视频出现“只打开播放器不播放”。
- **暂无持久化崩溃日志**：只能通过诊断 JSON 和安装器日志反馈。
- TensorRT-RTX 体积约 220MB，安装包约 151MB；V0.3 不拆可选组件。
- 4K 实际帧率取决于源分辨率和内容；低 SR FPS 时保持 1x 并跳帧，持续低于 15 FPS 会降档。

## 校验

```text
4DCD9B75E58C6EA574C9C34A7130DDA795E2E4DD9CE68C78AA2E92616BF138B7  domino-player-v0.3.0-beta.1-win64-setup.exe
43C4C37AA2C3127B8DF2CFE3EDAAB31BE95C6C9F30E87EF2A07C9CD07FECE29C  domino-player-v0.3.0-beta.1-win64.zip
```

## 反馈

请附上：

- 诊断页 JSON（选项 -> 诊断 -> 复制）
- 安装包/绿色包 SHA256
- GPU 型号、驱动版本
- 是否能稳定复现，以及片源分辨率/帧率

发布页：https://github.com/Rhett-12138/domino-player/releases/tag/v0.3.0-beta.1
