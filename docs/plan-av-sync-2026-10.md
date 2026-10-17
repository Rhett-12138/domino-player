# 计划：A/V 同步与 PTS 调度（2026-10，第二阶段）

> 状态：**已完成**（2026-10-17）。`ctest` / `flutter test` / `flutter build windows --debug` 通过；
> 29.97 测试片实机播放时烧录时间码与墙钟 1:1 推进。
>
> 目标：把播放从“帧序号 × 固定帧率 + 墙钟”改成“按媒体 PTS 呈现，音频时钟优先”，
> 并让离线编码保留非整数帧率（29.97/23.976），消除长期 A/V 漂移。

## 范围

1. **播放呈现时钟**
   - 有音频且 `AudioSink::clock()` 有效时，用音频时钟驱动视频；
   - 无音频/时钟不可用时回退墙钟；
   - 切换时以当前回退时钟为锚，避免跳变；设备时钟回退/跳变时重新锚定。
2. **按 PTS 呈现**
   - `readyToPresent()` 比较 `pendingPts_` 与当前媒体时钟，不再用 `presentedFrames_ × frameDur`；
   - 暂停/seek/EOF 重置锚点；VFR 与 23.976/29.97 自然按时间戳播放。
3. **音频 seek 线程安全**
   - `MediaStream aud_` 的 `seek()` 与喂食线程的 `nextAudioFrame()` 加互斥；
   - 首次启动/恢复时把音频 seek 到当前视频 PTS，避免非零起点错位。
4. **编码精确帧率与起点归一**
   - 输出 `time_base = 1 / avg_frame_rate`（有理数，如 1001/30000），不再 `lround` 成整数；
   - 帧 PTS 继续按帧序号递增；音频流复制原时间戳，A/V 不再因 29.97→30 漂移；
   - 非 0 起点（`.ts`）音频减去同一 `videoStart`，并在 `avformat_write_header` 后用最终 time_base 重算；
   - 关闭 B 帧 + x264 `zerolatency`，音频最多领先视频 100ms 写入，避免 fMP4 交错把视频 DTS 整体后移；
   - `.ts` AAC 用 `aac_adtstoasc` BSF。
5. **测试**
   - 新增 29.97 测试视频（短、小），验证编码输出帧率≈29.97、时长≈源；
   - 保留暂停/恢复、EOF、seek、合并回归测试；
   - 播放时钟用哑音频 sink（clock=0）走墙钟回退，必要时补一个会走时钟的 fake sink。

## 评审后补强

- 非 0 起点：音频 PTS 减去视频起点，并丢弃视频起点之前的音频；偏移在 `avformat_write_header` 后按 muxer 最终 time_base 重算。
- `.ts` AAC：使用 `aac_adtstoasc` BSF，输出按 `time_base_out` 转回输出流时间基。
- 交错控制：音频包先入队，最多领先视频 100ms 写出；配合 `max_b_frames=0` + `tune=zerolatency`，避免 fMP4 把视频 DTS 整体后移。
- 音频时钟看门狗：设备时钟 0.5s 不推进时回退墙钟；Ticker 停摆恢复时丢弃积压帧，避免追帧快进。
- 回归测试：29.97 帧数/帧率、.ts 起点归一、音频时钟驱动、停摆后丢帧。

## 不在本轮

- 完全按源 PTS 编码（VFR 逐帧时间戳映射）——本轮先修非整数帧率；
- 音频主时钟的缓冲/延迟补偿（先用 `IAudioClock` 已播放位置）；
- pump 移出 UI 线程、GPU 美颜、硬件解码、实时 SR。

## 验证

- `ctest` / `flutter test` / `flutter build windows --debug`；
- 探针：暂停 2s 后恢复 0.8s，位置增量≈0.8s；
- 实机：播放 29.97/普通视频，暂停/恢复、seek、音画同步观察；
- 编码测试：29.97 源 → 输出视频帧率≈29.97，时长≈源，音轨连续。
