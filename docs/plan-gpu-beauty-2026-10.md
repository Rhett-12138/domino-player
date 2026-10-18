# 计划：GPU 着色器美颜（2026-10，第三阶段）

> 状态：**已完成**（2026-10-18）。GPU surface 在 RTX 4070 SUPER 实机验证：4K 播放约 22 FPS、
> 黑白滤镜、磨皮 54% 均生效；`DOMINO_CPU_BEAUTY=1` CPU 回退正常。
>
> 目标：把当前 CPU 美颜（`EngineFrameOutput::applyBeautyCpu`）替换为 D3D11 HLSL 着色器，
> 通过 Flutter GPU surface（DXGI shared handle）直接上屏，避免 CPU 美颜与 RGB→RGBA 回读。

## 架构

```
Player::presentFrame(RGB24)
   └─ IFrameOutput
        ├─ GpuFrameOutput（flutter/native，D3D11/HLSL）
        │    ├─ 上传 RGB24 → RGBA 纹理（仍有一次 CPU 打包）
        │    ├─ pass1：水平高斯 → temp
        │    ├─ pass2：垂直高斯 + 肤色磨皮 + 美白 + 滤镜 → 共享 BGRA 纹理
        │    └─ FlutterDesktopGpuSurfaceDescriptor(DxgiSharedHandle)
        └─ EngineFrameOutput（domino_core，CPU 回退/headless 测试）
```

- D3D11 只在 `flutter/native/`，`domino_core` 仍无平台依赖。
- `DominoRegisterPlugin` 先尝试初始化 GPU；失败则注册 pixel buffer 纹理并继续 CPU 路径。
- 双/三缓冲共享纹理 + 引用计数，Flutter 光栅线程取帧不阻塞解码/GPU 渲染。

## 着色器

- 全屏三角形 + 两个 pixel shader：`ps_blur_h`、`ps_final`。
- 参数：`smooth/white/filterIdx/filterAmt/radius/texel/9-tap Gaussian 权重`。
- 数学镜像 `beauty_math.h`：`skinProb`、`blurBlend`、`whiteBlend`、`applyFilter`。
- 磨皮仅在肤色区混合；美白/滤镜与 CPU 顺序一致；输出 alpha=1。
- 无美颜时同样走 copy shader（保证 GPU surface 始终有内容）。

## 兼容/回退

- 无 D3D11、共享纹理创建失败、shader 编译失败 → 记录日志，回退现有 CPU `EngineFrameOutput`。
- 环境变量 `DOMINO_CPU_BEAUTY=1` 强制 CPU 路径，便于对比/排障。
- 滤镜缩略图仍复用 CPU：每帧只保留 80×45 小图，按需生成 7 张预览。

## 验证

- `ctest` / `flutter test` / `flutter build windows --debug`；
- 实机：打开视频，切换磨皮/美白/滤镜，观察 FPS 与效果；1080p 开满效果应不再掉到 CPU 线程瓶颈；
- 回退验证：`DOMINO_CPU_BEAUTY=1` 或强制 GPU 失败仍可播放；
- 对比 CPU/GPU 输出截图的肤色区域是否同向变化（不要求逐像素一致）。
