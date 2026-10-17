# 计划：修复已知问题（2026-10）

> 状态：**已完成**（2026-10-17）。`ctest` / `flutter test` / `flutter build windows --debug` 通过；
> 发布版 DLL 探针与实机冒烟通过。后续架构项见文末“明确不做”。
>
> 依据 `docs/roadmap.md` 与本次项目分析；目标是先恢复核心功能与数据安全，再做必要的健壮性清理。
> 架构性改造（音频时钟驱动、pump 移出 UI 线程、GPU 美颜、硬件解码）不在本轮，另立计划。

## 范围

### P0（本轮必须完成）

1. **模型目录分隔符**：`Config` 统一归一化，`srParam/srBin` 与离线编码都不再依赖调用方补 `\`。
   - 文件：`src/config.h`、`src/config.cpp`、`src/encode.cpp`
   - 测试：`tests/test_config.cpp`（无分隔符/有分隔符/环境变量）
2. **失败任务重试崩溃**：`TaskCenter::start` 在创建新线程前 join 旧线程。
   - 文件：`src/task.cpp`
   - 测试：`tests/test_task.cpp`（失败 → 重试不崩、状态为 `ST_FAILED`）
3. **暂停恢复快进**：恢复播放时重置呈现基准；补时序回归测试。
   - 文件：`src/player.cpp`、`src/player.h`
   - 测试：`tests/test_player.cpp`
4. **GPU 全局实例生命周期**：`Player`/`encodeOffline` 不再各自销毁全局实例；由 `EngineHandle` 在“任务停止 + Player 关闭”后统一销毁。
   - 文件：`src/player.cpp`、`src/encode.cpp`、`src/task.{h,cpp}`、`flutter/native/engine_bridge.cpp`
   - 测试：现有 SR/任务测试 + 退出路径人工冒烟
5. **断点续编数据安全**：
   - 续编取消/失败时保留 `.resume` 分段；下次启动先合并旧分段再继续，避免时间轴缺口；
   - 合并/替换文件检查错误；失败置 `ST_FAILED`，不再“假完成”；
   - 首次编码中断时把部分产物提升为主文件。
   - 文件：`src/task.cpp`
   - 测试：`tests/test_task.cpp`（续编取消 → 分段保留 → 再次继续完成）

### P1（本轮尽量完成）

6. **任务错误状态**：取消 → `ST_PAUSED`，真实失败 → `ST_FAILED`；Dart 允许失败任务重试并展示失败。
7. **`meta.tsv` 空描述**：初始化 `desc`，解析失败时安全回退。
8. **磨皮垂直高斯原地混叠**：第二遍使用独立缓冲；补对称性测试。
9. **解码层可重入/失败清理**：`AVPacket/AVFrame` 从函数静态改为成员；`open` 失败统一 `close()`；检查 `sws/swr`。
10. **WASAPI 资源释放**：析构/`stop()` 释放 COM 对象与事件；等待线程改为 `INFINITE`；`init` 前先释放旧资源。
11. **Player 数据竞争**：`paused_`、`volume_` 改为 atomic。
12. **桥层健壮性**：纹理/`out` 空指针保护；`domino_pump` 未注册纹理时不崩溃；退出顺序。
13. **Dart 小泄漏**：`DominoEngine.create` 释放 native 字符串。

### 明确不做（另立计划）

- 音频时钟驱动视频、精确 PTS/VFR、编码非整数帧率；
- `domino_pump` 移出 Flutter UI 线程；
- GPU 着色器美颜、硬件解码、实时 SR；
- 真实「边编码边播放」直播链路；
- 安装包/签名。

## 执行顺序

1. 配置路径 + 测试
2. 播放暂停/seek + 测试
3. 任务线程/状态/续编合并 + 测试
4. GPU 生命周期 + 桥层退出顺序
5. P1 清理（meta、磨皮、解码、WASAPI、atomic、桥/Dart）
6. 文档更新
7. DoD：`ctest`、`flutter test`、`flutter build windows --debug`、实机冒烟

## 验证标准

- `ctest --test-dir build -C Release --output-on-failure` 全过；
- `cd flutter && flutter test` 全过；
- `flutter build windows --debug` 成功；
- 用发布版 DLL 探针复测：
  - 无分隔符模型路径 → 超分任务完成；
  - 失败任务再次 `task_start` 不再崩溃；
  - 暂停 2s 后恢复 0.8s，位置增量 ≈ 0.8s；
- 启动 `domino_flutter.exe`，打开测试视频确认播放/暂停/任务中心可用。

## 评审后补强

- 续编合并非幂等：分段先改名 `.merging`，合并成功后才删除；`mergeToMp4` 用“主文件末尾是否已覆盖分段**末尾**”判断幂等（不能用首帧，检查点可能滞后）；
  `.bak` 在启动时恢复，避免替换主文件的两步 rename 窗口造成重复拼接。
- 续编分段写 `.cont` 标记；主文件丢失时拒绝把续编段当首段提升，避免静默丢掉前半段。
- 续编前把断点对齐到主文件真实末尾，减少检查点滞后造成的接缝重复。
- `TaskCenter` 增加析构兜底 `shutdown()`；`shutdown()` 只处理运行中/仍 joinable 的任务，避免重写已完成任务的 meta。
- 任务开始/停止/删除前若播放器正打开该产物，先 `Player::close()`，避免 Windows 文件占用导致合并失败。
- 去掉“帧数估算到结尾就 DONE”的启发式；改为编码返回 0 新帧且产物时长≈源时长时判定完成。

## 风险

- 改动涉及任务线程与文件替换，需重点保证取消/失败路径不丢数据；
- `std::filesystem::rename` 在 Windows 上不能覆盖已存在文件，统一用“备份 + 替换 + 回滚”；
- 全局 GPU 实例只能在所有 SR 使用者退出后销毁，退出顺序必须固定。
