# 分析：2x / 3x / 1.5x SR 模型与灵活选择（2026-10）

> 结论：**2x 最值得先做；3x 对 720p→4K 有价值；1.5x 没有主流原生模型，建议用 2x 输出再缩放模拟。**
> 当前只带了 4x 模型，且离线/Runtime 仍假设 4x；要真正提速需要通用 scale + 模型注册表。

## 当前假设

- 模型固定 4x：`encodeOffline` 里 `encInW = inW * 4`、`inH`、`srW` 都按 4x；
- Player 实时档位也按 `target/4` 计算输入；
- 只有：
  - `RealESRGAN_General_x4_v3`
  - `realesr-animevideov3-x4`
- `SR::process` 本身返回实际输出宽，所以**推理层不强制 4x**；限制在编码尺寸计算和模型文件选择。

## 可选模型

| Scale | 可用性 | 说明 |
|---|---|---|
| 2x | **最可行** | Real-ESRGAN `x2plus` 官方权重，需要转 NCNN；通用画质，适合 1080→4K、720→2K |
| 3x | 可行但要多做验证 | Real-CUGAN 的 NCNN 实现有 2x/3x/4x；偏动画/降噪，真人素材需验证；也可找 SwinIR/RealSR 3x NCNN 移植 |
| 1.5x | **没有主流原生模型** | 不建议找/训练；用 2x 输出缩到 1.5x，或 4x 输入 target/4 模拟 |
| 4x | 已有 | 现有通用模型 |

> 模型文件可以放 `assets/models/sr/ncnn/<name>.param/.bin`，运行时按 scale 注册；
> 不新增运行时依赖，仍由 NCNN Vulkan 执行。

## 按目标/源比例选 scale

理想情况：选 **最小 scale 使 `scale * srcW >= targetW`**，这样可保留完整源宽，模型输出略大于目标后 Lanczos 缩小。

| 源 → 目标 | 比例 | 最佳模型 | 说明 |
|---|---:|---|---|
| 720p → 1080p | 1.5x | 1.5x（没有） | 用 2x 1280→2560 缩到 1920，约 4x 模型的 1/4 输出像素 |
| 720p → 2K | 2.0x | **2x** | 1280→2560 **精确**，最理想 |
| 720p → 4K | 3.0x | **3x** | 1280→3840 **精确** |
| 1080p → 2K | 1.33x | 1.5x（没有） | 2x 1920→3840 缩到 2560；或 2x 输入 1280→2560（更快但先降源） |
| 1080p → 4K | 2.0x | **2x** | 1920→3840 **精确**，比 4x 7680→3840 快且少一次大图 |
| 2K → 4K | 1.5x | 1.5x（没有） | 2x/3x + 缩放；或先 2x 输入 1920→3840 |

## 预估性能（基于当前 4x FullSource 实测）

| 场景 | 当前 4x Full | 换 2x/3x 后预估 | 依据 |
|---|---:|---:|---|
| 720p→2K | 15.3s / 2s | **约 4s**（2x 1280→2560） | 输出像素约 1/4 |
| 720p→4K | 15.3s / 2s | **约 8.5s**（3x 1280→3840） | 输出像素约 1/1.78 |
| 1080p→2K | 32.6s / 2s | **约 14–16s**（2x 1920→3840→2560） | 中间输出 14.7MP vs 33MP |
| 1080p→4K | 36s / 2s | **约 16s**（2x 1920→3840） | 省掉 7680→3840 大图 |
| 720p→1080p | 15.3s / 2s | **约 4s**（2x 1280→2560→1920） | 1.5x 原生不可得 |

- 2x 在 1080→4K 是**质量+速度双赢**：3840 正好是 2x 输出，不需要 7680 中间帧。
- 3x 在 720→4K 正好 1280×3=3840，最合适。
- 1.5x 原生模型缺失；2x 模拟虽然比 4x 快约 4 倍，但不如原生 1.5x（约 7 倍），可以先接受。

## 灵活选择策略（建议）

```text
ratio = targetW / srcW
if ratio <= 1: 不跑 SR
else:
  选择满足 scale >= ratio 的最小可用 scale
  quality=Full:  input = min(srcW, cap/target 约束)     # 保留源
  quality=Fast:  input = ceil(target / scale)           # 按目标反推，最快
  quality=Balanced: 3/4 源宽与 target/scale 的折中
  output = scale * input
  如果 output > target: Lanczos 缩小
```

- 没有对应 scale 时回退 4x，并继续用现在的 FullSource/Fast 策略；
- 质量下拉（Fast/Balanced/FullSource）继续存在；模型 scale 自动按比例选，不需要用户理解“2x/3x”；
- 也可在高级设置里允许用户强制模型 scale，但默认自动。

## 落地工作量

| 工作 | 工作量 | 风险 |
|---|---:|---|
| 质量下拉 + Task 持久化 | 已完成 | 低 |
| 通用 `scale` 字段 + encode/Player 尺寸计算 | 1–2 人日 | 低 |
| 2x Real-ESRGAN 转 NCNN + 注册/回退 | 2–4 人日 | 中（转换/画质验证） |
| 3x Real-CUGAN/SwinIR 接入 | 3–6 人日 | 中高（真人画质、模型包体积） |
| 1.5x | 1–2 人日（仅 2x+缩放策略） | 低 |
| 自动 scale 选择 + bench/回归 | 1 人日 | 低 |

**建议顺序**：先 2x → 再 3x → 1.5x 用 2x 模拟。  
2x 能覆盖 1080→4K、720→2K，是投入产出比最高的一步；3x 主要服务 720→4K。

## 当前代码需要改的点

1. `SR` 增加 `scale()` / `init(param, bin, scale)`，模型注册表按文件名映射；
2. `encodeOffline` 用 `scale` 替换 `*4`，`inW = chooseInputWidth(..., scale)`；
3. `Player` 实时档位的 `target/4` 改成 `target/scale`（如果实时也要用）；
4. `chooseSRInputWidth` 增加 scale 参数，保留 FullSource/Balanced/Fast；
5. 缺少目标 scale 模型时回退 4x，并在 UI/日志提示实际模型；
6. 用 `domino_sr_bench` 增加“同画质不同 scale”对比，先验证 2x/3x 画质再默认启用。

> 注意：Real-CUGAN 偏动画/降噪，直接替换真人素材可能“涂抹”；上线前必须用真人、动画、文字三类素材各做盲测。
