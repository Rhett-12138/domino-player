# V0.3 M1：Windows 分发（草案，2026-10）

> 前置：M0 诊断/降级路径可用。M1 目标不是增加播放功能，而是让普通用户能安装、启动、卸载，并在缺运行时/缺 GPU 时得到明确提示。

## 决策

- 保留绿色 zip；新增 per-user 安装包（无需管理员权限）。
- V0.3 先不拆 TensorRT-RTX 220MB；完整包保证开箱即用，拆包放 V0.3.x。
- 安装器优先 Inno Setup（构建机需安装 `ISCC.exe`）；未安装时仍可产出 zip，不阻塞 CI。
- 代码签名证书待组织提供；未签名包必须在 README/安装器首屏提示 SmartScreen。

## 安装器内容

```
DominoPlayer/
  domino_flutter.exe
  domino_engine.dll
  flutter_windows.dll
  avcodec-63.dll / avformat-63.dll / avutil-61.dll
  swscale-10.dll / swresample-7.dll
  ncnn.dll
  tensorrt_rtx_1_7.dll / tensorrt_onnxparser_rtx_1_7.dll
  cudart64_12.dll              # 可选；缺失时自动 CPU 回退
  assets/models/sr/ncnn/       # 实时 NCNN 模型
  assets/models/sr/onnx/       # TensorRT 模型（可选组件先随包）
```

不写入用户目录；模型/任务数据继续在 `%APPDATA%/DominoPlayer`。安装器不删除用户数据。

## 运行时检测

1. **VC++ Runtime**
   - 优先检测 `VCRUNTIME140.dll` / `msvcp140.dll` 是否可由 exe 目录或系统目录加载。
   - 缺失时安装器提示并提供 VC_redist.x64 链接/随包；不静默失败。
2. **cudart**
   - 缺失不阻止启动；诊断页显示 `cudart=false` + CPU 回退原因。
3. **TensorRT**
   - 缺 DLL/ONNX 时自动 NCNN；性能档明确 SR 失败。
4. **GPU adapter**
   - 混合显卡默认 `DOMINO_D3D_ADAPTER=auto`；提供 `default` 启动参数/配置项做逃生。

## 验收

- 干净 Win10 22H2 / Win11 虚拟机：安装 → 首次启动 → 打开 1080p/4K 视频 → 卸载。
- 缺 VC++ Runtime：安装器提示明确；补齐后启动。
- 删除 cudart/TRT：仍可播放，诊断页原因可见。
- 安装包和 zip 版本号/SHA256 一致；卸载不删除 `%APPDATA%/DominoPlayer`。
- 首次 TensorRT engine 构建时间有提示，不显示“卡死”。

## 当前进度（2026-10-25）

- [x] 锁定 Inno Setup 6.7.3；脚本 `installer/domino.iss`
- [x] `installer/build.ps1`：构建 release、绿色 zip、安装包、SHA256
- [x] per-user 安装（`{localappdata}\Programs\DominoPlayer`），无需管理员
- [x] VC++ Runtime 检测：缺 `VCRUNTIME140/MSVCP140/VCRUNTIME140_1` 时提示并打开官方下载；中止安装
- [x] 安装/卸载冒烟：静默安装到临时目录、4K/TensorRT 启动、卸载后目录清空、`%APPDATA%\DominoPlayer` 保留
- [x] 安装版实测：`version=0.3.0`、`backend=tensorrt`、`targetW=3840`、`zeroCopy.active=true`、约 30 FPS
- [x] 未签名提示文本 + SHA256 输出
- [ ] 干净 Windows 10/11 虚拟机、缺 VC++ Runtime 场景
- [ ] 文件关联（当前应用不解析命令行文件参数，暂不绑定）
- [ ] 代码签名证书（待组织提供）

## 产物

```text
dist/domino-player-v0.3.0-win64-setup.exe
dist/domino-player-v0.3.0-win64-setup.exe.sha256
dist/domino-player-v0.3.0-win64.zip
dist/domino-player-v0.3.0-win64.zip.sha256
```

安装包约 151MB，绿色 zip 约 235MB；两者共用同一份运行时、模型和 `使用说明.txt`。`dist/` 被 Git 忽略，不提交二进制。

本地构建校验（2026-10-25）：

```text
setup SHA256 82595E9DDE53125F5B03F00D79DD971579AF6DF0D2150A1624869524044D7ED1
zip   SHA256 28883C7DA2F030C23FF4714C779CBF89317209426CE7810F3BE83102CFFEB058
```

安装包静默安装/卸载均 exit 0；安装版 `domino_flutter.exe` FileVersion `0.3.0+7`；安装版打开测试片并切 4K 后诊断：
`backend=tensorrt`、`targetW=3840`、`zeroCopy.active=true`、约 30 FPS。
