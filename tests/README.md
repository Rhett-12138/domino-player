# 测试说明

## 单元测试（headless，CI 可跑）
```
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```
`domino_tests.exe` 覆盖：
- `decode`：MediaStream 打开/逐帧解码/seek/资源释放幂等/close 复位
- `encode`：encodeOffline 产物时长、remuxFaststart 进度与时长、断点续编+合并（含时间戳衔接）、检查点滞后时合并不丢尾段；29.97 源保持 30000/1001 且帧数完整；非 0 起点 `.ts`（start_time≈2.8s）音视频归一；解码 EAGAIN 不丢帧
- `task`：任务持久化（meta）/重启扫描/断点恢复/删除；失败状态与重试（joinable 线程回归）；续编取消保留分段并再次完成；已合并分段不重复拼接；`.merging`/`.bak` 恢复；续编段缺主文件拒绝提升；空描述 meta 解析
- `player`：播放状态机冒烟（open/呈现/暂停/seek/EOF，注入哑音频 sink）；暂停恢复不快进；EOF 内部回卷保留 eof、外部 seek 清除 EOF；音频时钟驱动视频（fake clock 2x）；SR 失败回退源帧；暂停中 seek、连续 seek generation；Ticker 停摆后异步追帧
- `config`：默认配置与模型路径拼接（注入目录带/不带末尾分隔符都必须合法）
- `beauty`：肤色判定/磨皮/美白/全部滤镜预设方向
- `frame_output`：CPU 回退路径的美颜快路径/双缓冲交换/滤镜缩略图/垂直高斯上下对称
- `sr`：NCNN 模型冒烟（无 GPU 时自动跳过）

GPU 着色器美颜（`flutter/native/gpu_frame_output.cpp`）依赖真实 D3D11/Flutter GPU surface，
headless 单测覆盖不到；需按 `docs/plan-gpu-beauty-2026-10.md` 做实机冒烟：播放 1080p/4K、
黑白滤镜、磨皮/美白，并验证 `DOMINO_CPU_BEAUTY=1` 回退。

## SR 基准（手动）

```bash
cmake --build build --target domino_sr_bench --config Release
./build/Release/domino_sr_bench.exe ./tests/assets
```

输出：720p 源 → 1080p/2K/4K 的直接 `SR::process` ms/帧、理论 FPS，以及 Player 端到端 FPS。
不加入 ctest（依赖 GPU/模型，耗时长）；结果记录在 `docs/plan-sr-playback-2026-10.md`。

Flutter 测试（`cd flutter && flutter test`，纯 Dart，无需 DLL）：
- `format_test`：时间格式化、任务状态名、滤镜列表（纯函数）
- `widget_test`：任务中心新建/边播放、播放页控制条，用 `FakeEngine` 桩驱动（EngineApi 接口注入）

测试数据用仓库内 `tests/assets/test_av.mp4`（2s/30fps）、`test_long.mp4`（15s/30fps）、
`test_2997.mp4`（约 2s/29.97fps，用于非整数帧率）与 `test_offset.ts`（start_time≈2.8s，用于起点归一/ADTS）；
目标宽取源宽以跳过超分、跑得快；临时数据写入 `%TEMP%/domino_tests`。

## GUI 自动化（手动回归）
`tests/gui/` 下 PowerShell 脚本，用于驱动真实 Win32 窗口做端到端回归（可用于 `domino_flutter.exe`）：
- `killall.ps1`：强制清理所有 domino_flutter/domino_tests 进程（测试结束后务必执行）
- `rclick.ps1 -Hwnd <句柄> -X <x> -Y <y>`：真实鼠标点击（先置前台再 SendInput；坐标为客户区）
- `hover.ps1`：移动鼠标悬停
- `rdrag.ps1`：按住拖动
- `capture.ps1 -Hwnd <句柄> -OutPng <文件>`：截取窗口
- `pix.ps1 -Hwnd <句柄> -X <x> -Y <y>`：采样单像素颜色（用于判断界面状态）

典型回归流程：
```
# 启动 domino_flutter.exe → 点控制条「打开」选视频 → 校验播放/控制条/全屏等
# 用 capture.ps1 + 逐像素比对判断画面是否在动（间隔两帧差异显著=在播）
killall.ps1
```
注意：点击需目标窗口在前台（rclick 已处理）；Flutter 控件坐标随窗口尺寸/DPI 变化，
先用 `capture.ps1` 截图 + 像素采样定位按钮，再 `rclick.ps1` 点击。
