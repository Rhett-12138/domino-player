# 贡献指南（Contributing）

感谢参与 Domino Player！提交前请先读 [`AGENTS.md`](AGENTS.md)（核心目标 / 硬约束 / 完成定义），它优先于本文件。

## 开发环境

- **Windows** + Visual Studio 2022（MSVC，含「使用 C++ 的桌面开发」+ C++ CMake 工具）
- **CMake** ≥ 3.20
- **Flutter SDK**（stable）
- 支持 **Vulkan** 的独立显卡（NCNN 超分）
- 预编译依赖置于 `libs/`（**未随仓库提交，需自行放置**）：
  - `libs/ffmpeg/ffmpeg-master-latest-win64-gpl-shared/`
  - `libs/ncnn/ncnn-20260526-windows-vs2022-shared/`

## 构建 / 运行 / 测试

```bash
# C++ 引擎 + 单测
cmake -S . -B build
cmake --build build --target domino_tests
ctest --test-dir build -C Release --output-on-failure     # 应 0 failed

# Flutter 前端
cd flutter
flutter run -d windows                 # 开发（热重载）
flutter test                           # 纯 Dart，无需 DLL
flutter build windows --debug          # 产物在 build/windows/x64/runner/Debug/
```

## 完成定义（DoD）

一个改动算「完成」必须**全部**满足：

1. `ctest` 全过（涉及引擎/桥时），且新增逻辑有测试。
2. `cd flutter && flutter test` 全过（涉及前端时）。
3. `flutter build windows --debug` 成功（涉及桥/前端时）。
4. **实机冒烟**：涉及播放/桥/引擎的改动，启动 `domino_flutter.exe` 确认播放正常、关键交互生效（不能只看「进程存活」）。
5. 相关文档更新（`docs/`、`tests/README.md`）。

## 硬约束（务必遵守）

- **引擎可移植**：`domino_core` 不得依赖 D3D11/WASAPI；平台相关只能放前端或 `flutter/native` 桥。
- **视频输出**一律经 `IFrameOutput`；**音频**一律经 `AudioSink`。
- **不随意新增依赖**：Flutter 插件优先用 FFI 自实现；确需引入请先说明理由。

## 代码风格

- 注释用**中文**；**不加无关注释**；遵循既有风格。
- C++ 测试用 `tests/test_util.h` 轻量框架（`TEST` / `EXPECT_*`）；Flutter 测试用 `FakeEngine` 桩（见 `flutter/test/`）。
- 引擎改动优先补 headless 测试（可注入哑 `AudioSink`，不依赖 WASAPI）。

## 提交信息

风格见 `git log`：`feat:` / `fix:` / `refactor:` / `perf:` / `test:` / `docs:` / `chore:` + 中文要点。示例：

```
fix(Flutter): 任务完成卡最后一帧 + 启动不自动打开视频 + 循环默认关
```

## 提交流程

1. Fork 仓库并新建分支：`git checkout -b feat/your-feature`。
2. 实现并跑通 DoD（见上）。
3. **提交前扫敏感信息**：勿把个人绝对路径（`C:\Users\<你>\…`）、密钥/令牌/密码、个人文件路径写进代码或文档。自检：
   ```bash
   git status && git diff
   git grep -n -E "C:/Users|C:\\\\Users|C:/Repos|api[_-]?key|secret|token|password"
   ```
   注意：**历史里的敏感信息删提交也删不掉**（需改写历史 + 强推，或删库重建）——务必在提交前发现。配置/密钥优先走环境变量（如 `DOMINO_MODEL_DIR`）或本地未跟踪文件（`.gitignore`）。
4. 提交（信息风格见上），推送到你的 fork。
5. 发起 Pull Request，说明：**改了什么 / 为什么 / 如何验证**（含测试与实机结果）。
6. 涉及引擎/桥/并发/FFI、或改动较大时，PR 描述里附上你的验证证据（命令输出、截图等）。

## 报告问题 / 建议

在 GitHub Issues 中提供：环境（系统/显卡/驱动）、复现步骤、期望与实际、日志/截图。若涉及播放卡顿，请注明视频分辨率、是否开滤镜/超分、FPS 角标数值。

## 许可

贡献即表示同意你的代码以本项目 [MIT](LICENSE) 许可发布。注意第三方组件许可（见 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)）。
