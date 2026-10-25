#include "player.h"
#include "audio.h"
#include "log.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

Player::Player(IFrameOutput& out) : out_(out), gpuSink_(dynamic_cast<IGpuFrameSink*>(&out)) {
  audio_ = createDefaultAudioSink();   // 默认 WASAPI；可经 setAudioSink 注入其他实现
}

std::string Player::gpuFallbackReason() const {
  std::lock_guard<std::mutex> lk(diagMu_);
  return gpuFallbackReason_;
}

void Player::setGpuFallbackReason(const std::string& reason) {
  std::lock_guard<std::mutex> lk(diagMu_);
  gpuFallbackReason_ = reason;
}

Player::~Player() {
  shutdown();
}

void Player::shutdown() {
  stopVideo();
  stopAudio();
  vid_.close();
  aud_.close();
  clearVideoQueue();
  dropPendingFrame();
  {
    std::lock_guard<std::mutex> lk(videoMu_);
    pendingFrame_.rgb.clear();
    freeBufs_.clear();
  }
  currentPath_.clear();
  resetClockAnchors();
  trt_.clear();
  useTrt_.store(false);
  sr_.clear();   // 全局 GPU 实例由前端在停止所有任务后统一销毁
  srInited_.store(false);
  srFailed_.store(false);
  srProducedFps_.store(0);
  gpuSrActive_.store(false);
}

void Player::audioFeedStatic(Player* self) { self->audioFeed(); }

void Player::audioFeed() {
  SwrContext* resampler = nullptr;
  std::vector<float> srcBuf(8192 * aud_.channels);
  std::vector<float> dstBuf(8192 * audio_->deviceChannels);
  double pts = 0;

  while (!stopFeed_.load()) {
    if (resetAudio_) {
      audio_->flush();
      if (resampler) { swr_free(&resampler); resampler = nullptr; }
      resetAudio_ = false;
    }
    if (paused_.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }

    // 缓冲音频最多超前 ~120ms，避免解码抢跑导致 A/V 漂移/缓冲溢出
    if (audio_->bufferedFrames() > (size_t)(audio_->deviceRate * 0.12)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }

    int n = 0;
    {
      std::lock_guard<std::mutex> lk(audMu_);
      n = aud_.nextAudioFrame(srcBuf.data(), 8192, &pts);
    }
    if (n <= 0) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }

    if (!resampler) {
      AVChannelLayout srcLay, dstLay;
      av_channel_layout_default(&srcLay, aud_.channels);
      av_channel_layout_default(&dstLay, audio_->deviceChannels);
      int o = swr_alloc_set_opts2(&resampler, &dstLay, AV_SAMPLE_FMT_FLT, audio_->deviceRate,
        &srcLay, AV_SAMPLE_FMT_FLT, aud_.sampleRate, 0, nullptr);
      av_channel_layout_uninit(&srcLay);
      av_channel_layout_uninit(&dstLay);
      if (o < 0 || !resampler || swr_init(resampler) < 0) {
        fprintf(stderr, "swr init failed o=%d\n", o);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
    }
    int maxOut = (int)((double)n * audio_->deviceRate / aud_.sampleRate) + 64;
    if (maxOut > 8192) maxOut = 8192;
    uint8_t* odata = (uint8_t*)dstBuf.data();
    const uint8_t* idata = (const uint8_t*)srcBuf.data();
    int outN = swr_convert(resampler, &odata, maxOut, &idata, n);
    if (outN > 0) {
      // 音量
      float v = (float)volume_.load();
      if (v != 1.f)
        for (int i = 0; i < outN * audio_->deviceChannels; i++) dstBuf[i] *= v;
      audio_->push(dstBuf.data(), outN);
    }
  }
  if (resampler) swr_free(&resampler);
}

void Player::ensureSR() {
  if (noSR_.load() || srFailed_.load()) return;
  if (srInited_.load()) {
    if ((useTrt_.load() && trt_.ready()) || (!useTrt_.load() && sr_.ready())) return;
    srInited_.store(false);
  }
  if (cfg_.useTrt) {
    const SRModel model = SRModel::Realistic;
    const std::string onnx = cfg_.srOnnx(model);
    if (!onnx.empty() &&
        trt_.init(onnx.c_str(), cfg_.trtCacheDir().c_str(), "realistic", 4, 1920, 1080)) {
      useTrt_.store(true);
      srInited_.store(true);
      return;
    }
    trt_.clear();
  }
  useTrt_.store(false);
  if (sr_.init(cfg_.srParam().c_str(), cfg_.srBin().c_str())) {
    srInited_.store(true);
  } else {
    srFailed_.store(true);
    fprintf(stderr, "sr init failed\n");
  }
}

void Player::setNoSR(bool b) {
  noSR_.store(b);
  srFailed_.store(false);
  srProducedFps_.store(0);
  gpuSrActive_.store(false);
  if (!gpuSink_ || !gpuSink_->gpuFatal()) gpuSrDisabled_.store(false);
  if (b) {
    // 用户主动关闭：清掉档位与自动状态
    srCeilingW_.store(0);
    srTargetW_.store(0);
    srAutoDegraded_.store(false);
    setGpuFallbackReason("SR disabled");
  }
  fpsLowSince_ = 0;
  autoCooldownUntil_ = 0;
  dropUntilPts_.store(0);
  dropPendingFrame();
  seekGen_.fetch_add(1);
  clearVideoQueue();
  videoCv_.notify_all();
}

void Player::setSRTarget(int presetIdx, int targetW) {
  srPresetIdx_.store(presetIdx);
  srCeilingW_.store(targetW);
  srTargetW_.store(targetW);
  srAutoDegraded_.store(false);
  srFailed_.store(false);
  srProducedFps_.store(0);
  gpuSrActive_.store(false);
  if (!gpuSink_ || !gpuSink_->gpuFatal()) gpuSrDisabled_.store(false);
  if (targetW > 0) noSR_.store(false);
  // 手动重选档位 = 重新尝试；清掉自动降档的冷却/追帧
  fpsLowSince_ = 0;
  autoCooldownUntil_ = 0;
  dropUntilPts_.store(0);
  dropPendingFrame();
  seekGen_.fetch_add(1);
  clearVideoQueue();
  videoCv_.notify_all();
}

void Player::maybeAutoDegrade(double now) {
  if (!autoDegradeEnabled_.load() || noSR_.load() || srCeilingW_.load() <= 0) return;
  if (now < autoCooldownUntil_) return;
  if (now - lastAutoCheck_ < 0.20) return;
  lastAutoCheck_ = now;

  // 只有实际呈现帧率低于 15 FPS 才降档；A/V 短暂落后不再触发。
  // 启动/初始化阶段给 3 秒稳定窗口，避免把 engine/JIT 首帧误判为低帧率。
  if (paused_.load() || now < 3.0) {
    fpsLowSince_ = 0;
    return;
  }
  double fps = fpsDisplay_;
  // 零拷贝/呈现可能因跳帧让 fpsDisplay 低于真实 SR 产出；优先用 VideoWorker
  // 的实际 SR 帧率，避免慢输出把 24FPS SR 误判成低帧率并降档。
  const double produced = srProducedFps_.load();
  if (srInited_.load() && !srFailed_.load() && produced > 0.0) fps = produced;
  if (fps <= 0.0 || fps >= 15.0) {
    fpsLowSince_ = 0;
    return;
  }
  if (fpsLowSince_ <= 0) {
    fpsLowSince_ = now;
    return;
  }
  if (now - fpsLowSince_ < 2.0) return;

  const int cur = srTargetW_.load();
  if (cur <= 0) return;
  // 2K/4K 直接降到 1080p；1080p 仍低于 15 FPS 则关闭 SR。
  const int next = cur > 1920 ? 1920 : 0;
  srTargetW_.store(next);
  srAutoDegraded_.store(true);
  if (next == 0) noSR_.store(true);

  // 追到当前媒体时钟，避免降档后仍保留之前的音画差
  const double clock = currentMediaClock();
  dropPendingFrame();
  pendingPts_ = clock;
  dropUntilPts_.store((std::min)(clock, duration_ > 0 ? duration_ : clock));
  seekGen_.fetch_add(1);   // 丢弃在途的旧档位帧，避免旧 gpuSlot 跨尺寸复用
  clearVideoQueue();
  videoCv_.notify_all();

  fpsLowSince_ = 0;
  autoCooldownUntil_ = now + 4.0;
  fprintf(stderr, "sr auto degrade: %d -> %d (fps %.1f)\n", cur, next, fps);
}

bool Player::open(const std::string& path) {
  stopVideo();
  stopAudio();
  vid_.close();
  aud_.close();
  currentPath_.clear();
  clearVideoQueue();
  {
    std::lock_guard<std::mutex> lk(videoMu_);
    freeBufs_.clear();
  }
  resetClockAnchors();
  if (!vid_.open(path.c_str())) return false;
  currentPath_ = path;
  srFailed_.store(false);
  srProducedFps_.store(0);
  gpuSrActive_.store(false);
  setGpuFallbackReason("");
  if (!gpuSink_ || !gpuSink_->gpuFatal()) gpuSrDisabled_.store(false);
  srAutoDegraded_.store(false);
  fpsLowSince_ = 0;
  autoCooldownUntil_ = 0;
  lastAutoCheck_ = 0;
  duration_ = vid_.duration;
  fps_ = vid_.fps;
  if (vid_.hasAudio && audio_->init() && aud_.open(path.c_str()) && aud_.hasAudio)
    hasAudio_ = true;
  else
    hasAudio_ = false;
  paused_.store(false);
  playhead_ = 0;
  pendingPts_ = 0;   // 新文件首帧前不能沿用上一个视频的 PTS 锚点
  eof_ = false;
  seekPending_.store(false);
  eofSeek_ = false;
  seekTarget_ = 0;
  dropPendingFrame();
  pendingFrame_.rgb.clear();
  pendingEOF_ = false;
  presentedFrames_ = 0;
  resetClockAnchors();
  frameCount_ = 0;
  lastFps_ = 0;
  fpsT0_ = std::chrono::steady_clock::now();
  tStart_ = std::chrono::steady_clock::now();
  srcBuf_.assign((size_t)vid_.vw * vid_.vh * 3, 0);
  videoEof_.store(false);
  seekReq_.store(false);
  dropUntilPts_.store(0);
  startVideo();
  return true;
}

void Player::close() {
  stopVideo();
  stopAudio();
  vid_.close();
  aud_.close();
  clearVideoQueue();
  dropPendingFrame();
  {
    std::lock_guard<std::mutex> lk(videoMu_);
    pendingFrame_.rgb.clear();
    freeBufs_.clear();
  }
  currentPath_.clear();
  resetClockAnchors();
  playhead_ = 0;
  pendingPts_ = 0;
  duration_ = 0;
  fps_ = 30;
  hasAudio_ = false;
  eof_ = false;
  fpsLowSince_ = 0;
  autoCooldownUntil_ = 0;
  lastAutoCheck_ = 0;
}

void Player::requestSeek(double t) {
  if (t < 0) t = 0;
  if (t > duration_) t = duration_;
  seekTarget_ = t;
  eofSeek_ = false;   // 外部 seek 后应脱离 EOF 态
  seekPending_.store(true);
}

void Player::seekInternal(double t) {
  {
    std::lock_guard<std::mutex> lk(audMu_);
    aud_.seek(t);
  }
  resetAudio_.store(true);
  playhead_ = t;
  pendingPts_ = t;   // seek 后时钟/同步保护以目标为基准，避免用旧 EOF 帧 PTS 锚定
  fpsLowSince_ = 0;
  autoCooldownUntil_ = 0;
  lastAutoCheck_ = 0;
  resetClockAnchors();
  dropPendingFrame();
  pendingEOF_ = false;
  // 视频 seek 交给 worker；generation 保证 seek 前在途帧被丢弃
  seekTargetVideo_.store(t);
  seekGen_.fetch_add(1);
  seekReq_.store(true);
  dropUntilPts_.store(0);
  videoEof_.store(false);
  clearVideoQueue();
  videoCv_.notify_all();
}

bool Player::processSeek() {
  if (!seekPending_.exchange(false)) return false;
  pendingEOF_ = false;
  seekInternal(seekTarget_);
  presentedFrames_ = 0;
  resetClockAnchors();
  if (eofSeek_)
    eofSeek_ = false;   // EOF 内部回卷：保留 eof_ 供前端判断
  else
    eof_ = false;
  return true;
}

void Player::setPaused(bool p) {
  if (paused_.load() == p) return;
  paused_.store(p);
  videoCv_.notify_all();
  if (!p) {
    eof_ = false;   // 重新播放即脱离 EOF 态
    // 暂停期间墙钟仍在走，恢复时必须重置呈现基准，否则会按 pump 频率快进追帧
    presentedFrames_ = 0;
    fpsLowSince_ = 0;
    resetClockAnchors();
  }
  if (hasAudio_) {
    if (p) {
      audio_->pause();
      audio_->flush();
      audioClockValid_ = false;
    } else {
      if (!audioStarted_.load()) {
        startAudio();   // 内部已 seek + 置 resetAudio_
      } else {
        std::lock_guard<std::mutex> lk(audMu_);
        aud_.seek(playhead_);
        resetAudio_.store(true);
      }
      audioClockValid_ = false;
      lastAudioClock_ = 0;
      audio_->resume();
    }
  }
}

void Player::startAudio() {
  if (audioStarted_.exchange(true)) return;
  // 视频首个 PTS 可能不是 0；把音频也 seek 到同一媒体时间，避免非零起点错位
  if (playhead_ > 0.01) {
    std::lock_guard<std::mutex> lk(audMu_);
    aud_.seek(playhead_);
  }
  resetAudio_.store(true);
  audioClockValid_ = false;
  lastAudioClock_ = 0;
  audio_->start();
  stopFeed_.store(false);
  audioThread_ = std::thread(audioFeedStatic, this);
}

void Player::stopAudio() {
  stopFeed_.store(true);
  audio_->stop();
  if (audioThread_.joinable()) { audioThread_.join(); audioThread_ = std::thread(); }
  audioStarted_.store(false);
  hasAudio_ = false;
}

void Player::videoLoopStatic(Player* self) { self->videoLoop(); }

void Player::startVideo() {
  if (videoThread_.joinable()) videoThread_.join();
  videoStop_.store(false);
  videoThread_ = std::thread(videoLoopStatic, this);
}

void Player::stopVideo() {
  videoStop_.store(true);
  videoCv_.notify_all();
  if (videoThread_.joinable()) { videoThread_.join(); videoThread_ = std::thread(); }
}

void Player::releaseGpuFrame(VideoFrame& f) {
  if (f.gpuSlot >= 0) {
    if (gpuSink_) gpuSink_->releaseGpuSrFrame(f.gpuSlot);
    f.gpuSlot = -1;
  }
}

void Player::dropPendingFrame() {
  if (!havePending_) return;
  releaseGpuFrame(pendingFrame_);
  if (!pendingFrame_.rgb.empty()) {
    std::lock_guard<std::mutex> lk(videoMu_);
    freeBufs_.push_back(std::move(pendingFrame_.rgb));
  }
  havePending_ = false;
}

void Player::clearVideoQueue() {
  std::lock_guard<std::mutex> lk(videoMu_);
  for (auto& f : videoFrames_) {
    releaseGpuFrame(f);
    if (!f.rgb.empty()) freeBufs_.push_back(std::move(f.rgb));
  }
  videoFrames_.clear();
}

void Player::videoLoop() {
  int srProduceCount = 0;
  auto srProduceT0 = std::chrono::steady_clock::now();
  while (!videoStop_.load()) {
    // seek 必须在 paused 分支之前消费：暂停时拖动进度条/EOF 回卷也会请求 seek，
    // 否则 wait predicate 永远为真，worker 会空转烧 CPU。
    if (seekReq_.exchange(false)) {
      vid_.seek(seekTargetVideo_.load());
      videoEof_.store(false);
      dropUntilPts_.store(0);
      clearVideoQueue();
      videoCv_.notify_all();
      continue;
    }
    if (paused_.load()) {
      std::unique_lock<std::mutex> lk(videoMu_);
      videoCv_.wait_for(lk, std::chrono::milliseconds(20), [&] {
        return videoStop_.load() || !paused_.load() || seekReq_.load();
      });
      continue;
    }

    {
      std::unique_lock<std::mutex> lk(videoMu_);
      videoCv_.wait_for(lk, std::chrono::milliseconds(20), [&] {
        return videoStop_.load() || seekReq_.load() || paused_.load() ||
               videoFrames_.size() < 2;
      });
      if (videoStop_.load() || paused_.load() || seekReq_.load()) continue;
      if (videoFrames_.size() >= 2) continue;
    }

    const uint64_t gen = seekGen_.load();
    double pts = 0;
    int r = vid_.nextVideoFrame(srcBuf_.data(), &pts);
    if (r < 0) {
      // 解码到尾部时若 seek 已发生，不要把旧代的 EOF 传给 UI
      if (seekGen_.load() == gen && !seekReq_.load()) videoEof_.store(true);
      std::unique_lock<std::mutex> lk(videoMu_);
      videoCv_.wait_for(lk, std::chrono::milliseconds(30), [&] {
        return videoStop_.load() || seekReq_.load() || paused_.load();
      });
      continue;
    }
    if (seekGen_.load() != gen) continue;

    // Ticker 停摆/稳态落后追帧：只解码，不跑 SR
    const double dropUntil = dropUntilPts_.load();
    if (dropUntil > 0 && pts + 0.10 < dropUntil) continue;

    if (!noSR_.load() && !srInited_.load() && !srFailed_.load()) ensureSR();
    const bool useSR = !noSR_.load() && srInited_.load() && !srFailed_.load();
    const bool needSR = useSR && (inW_.load() > 0 || vid_.vw < srTargetW_.load());

    VideoFrame f;
    f.pts = pts;
    {
      std::lock_guard<std::mutex> lk(videoMu_);
      if (!freeBufs_.empty()) {
        f.rgb = std::move(freeBufs_.back());
        freeBufs_.pop_back();
      }
    }

    bool srDone = false;
    if (needSR) {
      const int inW = inW_.load() > 0 ? inW_.load() : srTargetW_.load() / 4;
      bool gpuSr = false;
      const bool noBeauty = !beautyActive_.load();
      if (gpuSink_ && useTrt_.load() && !gpuSrDisabled_.load() && noBeauty) {
        int outW = 0, outH = 0;
        int slot = -1;
        void* textures[3] = {};
        if (trt_.outputSize(vid_.vw, vid_.vh, inW, outW, outH)) {
          // 先注销旧 cudart 资源，再让 GpuFrameOutput 重建 D3D 纹理池。
          if (outW != gpuSlotW_ || outH != gpuSlotH_) {
            trt_.resetGpuInterop();
            gpuSlotW_ = outW;
            gpuSlotH_ = outH;
          }
        }
        if (outW > 0 && outH > 0 &&
            gpuSink_->acquireGpuSrFrame(outW, outH, slot, textures)) {
          const int sw = trt_.processGpu(srcBuf_.data(), vid_.vw, vid_.vh, inW, slot,
                                         textures, gpuSink_);
          if (sw == outW && outW > 0 && outH > 0) {
            f.gpuSlot = slot;
            f.w = sw;
            f.h = outH;
            gpuSr = true;
            if (!gpuSrActive_.exchange(true)) setGpuFallbackReason("");
            srDone = true;
          } else {
            gpuSink_->releaseGpuSrFrame(slot);
            gpuSrDisabled_.store(true);
            gpuSrActive_.store(false);
            setGpuFallbackReason("cudart/D3D interop failed");
            fprintf(stderr, "zero-copy: processGpu failed (size %d vs %d), fallback CPU\n",
                    sw, outW);
          }
        }
      }
      if (!gpuSr) {
        gpuSrActive_.store(false);
        if (gpuSink_ && !gpuSrDisabled_.load() && !noBeauty) {
          setGpuFallbackReason("beauty/filter active");
        }
        const int sw = useTrt_.load()
          ? trt_.process(srcBuf_.data(), vid_.vw, vid_.vh, inW, f.rgb)
          : sr_.process(srcBuf_.data(), vid_.vw, vid_.vh, inW, f.rgb);
        if (sw > 0) {
          f.w = sw;
          f.h = (int)(f.rgb.size() / (size_t)sw / 3);
          srDone = true;
        } else {
          srFailed_.store(true);
          f.rgb.assign(srcBuf_.data(), srcBuf_.data() + (size_t)vid_.vw * vid_.vh * 3);
          f.w = vid_.vw;
          f.h = vid_.vh;
        }
      }
    } else {
      f.rgb.assign(srcBuf_.data(), srcBuf_.data() + (size_t)vid_.vw * vid_.vh * 3);
      f.w = vid_.vw;
      f.h = vid_.vh;
    }
    if (srDone) {
      srProduceCount++;
      const auto now = std::chrono::steady_clock::now();
      const double el = std::chrono::duration<double>(now - srProduceT0).count();
      if (el >= 1.0) {
        srProducedFps_.store(srProduceCount / el);
        srProduceCount = 0;
        srProduceT0 = now;
      }
    }
    if (seekGen_.load() != gen) {
      releaseGpuFrame(f);
      continue;
    }

    // 解码/SR 期间 UI 可能刚设置停摆追帧目标：丢弃已过期帧（不发布）
    const double dropAfter = dropUntilPts_.load();
    if (dropAfter > 0 && pts + 0.10 < dropAfter) {
      std::lock_guard<std::mutex> lk(videoMu_);
      releaseGpuFrame(f);
      if (!f.rgb.empty()) freeBufs_.push_back(std::move(f.rgb));
      continue;
    }

    {
      std::lock_guard<std::mutex> lk(videoMu_);
      // 锁内再校验 generation：UI seek 会先 ++gen 再 clearVideoQueue，
      // 避免旧帧在 clear 之后被塞回队列。
      if (videoStop_.load() || seekGen_.load() != gen) {
        releaseGpuFrame(f);
        if (!f.rgb.empty()) freeBufs_.push_back(std::move(f.rgb));
        continue;
      }
      videoFrames_.push_back(std::move(f));
    }
    // 追帧目标帧已经发布后才清除；期间 UI 不会重复设置新目标，避免 worker 被追帧饿死
    if (dropUntil > 0 && dropUntilPts_.load() <= dropUntil + 1e-6)
      dropUntilPts_.store(0);
    videoCv_.notify_all();
  }
}

void Player::prefetch() {
  if (havePending_ || pendingEOF_) return;
  VideoFrame f;
  {
    std::lock_guard<std::mutex> lk(videoMu_);
    if (videoFrames_.empty()) {
      if (videoEof_.load()) pendingEOF_ = true;
      return;
    }
    f = std::move(videoFrames_.front());
    videoFrames_.pop_front();
  }
  videoCv_.notify_all();
  pendingFrame_ = std::move(f);
  pendingPts_ = pendingFrame_.pts;
  havePending_ = true;
}

// 实时 SR 慢于片源时，worker 产出的 PTS 会落后媒体时钟；若仍逐帧呈现，
// 片源时间就会被拉长（用户感知为“慢放”）。这里丢弃已落后太多的帧，
// 保持媒体时间 1x；帧率下降由 fpsDisplay/自动降档处理。
void Player::dropLateFrames() {
  // 只有 SR 实际产出低于片源帧率时才追帧；普通 SlowFrameOutput 测试的
  // SR 产出仍是源帧率，保持原有慢输出/自动降档语义。SR 关闭/失败不追帧。
  if (paused_.load() || noSR_.load() || !srInited_.load() || srFailed_.load() ||
      !havePending_ || pendingEOF_) {
    return;
  }
  const double produced = srProducedFps_.load();
  if (fps_ <= 0.0 || produced <= 0.0 || produced >= fps_ * 0.95) return;
  {
    // 队列里有下一帧说明 UI/呈现仍是主要瓶颈；此时不要跳帧。
    std::lock_guard<std::mutex> lk(videoMu_);
    if (!videoFrames_.empty()) return;
  }
  constexpr double kMaxLagSec = 0.15;
  const double lag = currentMediaClock() - pendingPts_;
  if (lag <= kMaxLagSec) return;
  // 慢 SR 不能在 UI 端无限丢帧（会把画面丢空）。只让 VideoWorker 跳过
  // 旧的源帧、对接近当前媒体时钟的帧跑 SR；当前 pending 仍然上屏，
  // 下一帧会跳到一个更接近媒体时钟的帧，从而保持 1x。
  // 一次追帧请求只能设置一次，避免 UI 每帧重设导致 worker 永远跳过。
  if (dropUntilPts_.load() <= 0) dropUntilPts_.store(currentMediaClock());
}

void Player::resetClockAnchors() {
  fallbackWallBase_ = 0;
  fallbackMediaBase_ = 0;
  audioClockBase_ = 0;
  audioMediaBase_ = 0;
  lastAudioClock_ = 0;
  lastAudioClockWall_ = 0;
  lastPumpWall_ = 0;
  audioClockValid_ = false;
}

// 当前媒体时钟：优先音频设备已播放位置；不可用时用墙钟从首帧 PTS 起算。
// 音频时钟刚可用/回退/跳变时，以当前回退时钟为锚，保证切换不跳变。
double Player::currentMediaClock() {
  const double now = nowWall();
  if (hasAudio_ && audioStarted_.load()) {
    const double ac = audio_->clock();
    // 设备时钟停走（切设备/驱动异常）时不能永久冻结画面：0.5s 未推进则回退墙钟
    if (audioClockValid_ && now - lastAudioClockWall_ > 0.5) {
      const double lastMedia = audioMediaBase_ + (lastAudioClock_ - audioClockBase_);
      audioClockValid_ = false;
      fallbackWallBase_ = now;
      fallbackMediaBase_ = lastMedia;
    }
    if (ac > 0.0005) {
      const bool reanchor = !audioClockValid_ || ac + 0.05 < lastAudioClock_ ||
                            ac > lastAudioClock_ + 0.5;
      if (reanchor) {
        if (fallbackWallBase_ <= 0) {
          fallbackWallBase_ = now;
          fallbackMediaBase_ = pendingPts_;
        }
        audioMediaBase_ = fallbackMediaBase_ + (now - fallbackWallBase_);
        audioClockBase_ = ac;
        audioClockValid_ = true;
      }
      if (ac > lastAudioClock_ + 0.0005 || reanchor) lastAudioClockWall_ = now;
      lastAudioClock_ = ac;
      return audioMediaBase_ + (ac - audioClockBase_);
    }
  }
  if (fallbackWallBase_ <= 0) {
    fallbackWallBase_ = now;
    fallbackMediaBase_ = pendingPts_;
  }
  return fallbackMediaBase_ + (now - fallbackWallBase_);
}

bool Player::readyToPresent() {
  if (!havePending_) return false;
  return currentMediaClock() + 0.0005 >= pendingPts_;
}

bool Player::presentFrame() {
  srOutW_ = pendingFrame_.w;
  srOutH_ = pendingFrame_.h;
  if (pendingFrame_.gpuSlot >= 0) {
    const int slot = pendingFrame_.gpuSlot;
    // slot 的归还在 GpuFrameOutput::presentGpuSrFrame 内部完成（成功/可安全失败）。
    if (gpuSink_ && !gpuSink_->presentGpuSrFrame(slot, pendingFrame_.w, pendingFrame_.h)) {
      if (gpuSink_->gpuFatal()) {
        gpuSrDisabled_.store(true);
        gpuSrActive_.store(false);
        setGpuFallbackReason("GPU present fatal/query timeout");
        fprintf(stderr, "zero-copy: present fatal, fallback CPU\n");
      }
    }
    pendingFrame_.gpuSlot = -1;
  } else {
    out_.presentFrame(pendingFrame_.rgb.data(), pendingFrame_.w, pendingFrame_.h);
  }
  playhead_ = pendingPts_;
  presentedFrames_++;
  havePending_ = false;
  // 帧缓冲回收到 worker，避免每帧分配大 vector
  if (!pendingFrame_.rgb.empty()) {
    std::lock_guard<std::mutex> lk(videoMu_);
    freeBufs_.push_back(std::move(pendingFrame_.rgb));
  }
  videoCv_.notify_all();
  frameIdx_++;
  drewVideo_ = true;
  bool first = (presentedFrames_ == 1);

  frameCount_++;
  double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - fpsT0_).count();
  if (el >= 1.0) { lastFps_ = frameCount_ / el; frameCount_ = 0; fpsT0_ = std::chrono::steady_clock::now(); }
  fpsDisplay_ = lastFps_;
  return first && hasAudio_;
}

void Player::pump() {
  if (!hasVideo()) return;
  processSeek();
  if (paused_.load()) return;

  const double now = nowWall();
  if (lastPumpWall_ > 0 && now - lastPumpWall_ > 0.5) {
    // Ticker 停摆：让 worker 只解码、跳过 SR 追到当前媒体时钟；UI 不阻塞
    dropPendingFrame();
    dropUntilPts_.store(currentMediaClock());
    clearVideoQueue();
    videoCv_.notify_all();
    fpsLowSince_ = 0;   // UI 停摆不是 SR 慢，不能触发自动降档
    autoCooldownUntil_ = now + 2.0;
  }
  lastPumpWall_ = now;
  maybeAutoDegrade(now);

  prefetch();
  dropLateFrames();
  if (readyToPresent()) {
    if (presentFrame()) startAudio();   // 第一帧呈现时启动音频
  }
  handleEof();
}

Player::EofAction Player::handleEof() {
  if (!pendingEOF_ || havePending_) return EofAction::None;
  if (liveActive()) return EofAction::LivePresent;  // 上层处理预览/完成
  // 非直播：暂停并回起点（内部回卷不经过 requestSeek，避免清掉 eof_）
  eof_ = true;              // 暴露"已到结尾"（前端据此实现循环/重播）
  setPaused(true);          // 同时暂停音频 sink
  seekTarget_ = 0;
  eofSeek_ = true;
  seekPending_.store(true);
  havePending_ = false;
  return EofAction::Pause;
}

void Player::updateLiveProgress(double frames, double fps) {
  if (fps <= 0) return;
  playhead_ = frames / fps;
  if (playhead_ > duration_) duration_ = playhead_;
}

void Player::beginRender() { out_.beginPresent(); }

void Player::endRender() {
  // 用 Present 返回间隔测量 vsync
  double pw = nowWall();
  if (lastPresTime_ > 0) {
    presIntervals_.push_back(pw - lastPresTime_);
    if (presIntervals_.size() > 60) presIntervals_.erase(presIntervals_.begin());
    if (presIntervals_.size() == 20) {
      std::vector<double> sorted = presIntervals_;
      std::sort(sorted.begin(), sorted.end());
      double v = sorted[sorted.size() / 2];
      if (v > 0.004 && v < 0.05) vsync_ = v;
    }
  }
  lastPresTime_ = pw;
  out_.endPresent();
}

int Player::vsyncPeriod() const {
  return (int)(vsync_ * 1000.0 + 0.5);
}

std::string Player::titleText() const {
  char buf[160];
  snprintf(buf, sizeof(buf), "Domino P2  %02d:%02d / %02d:%02d",
    (int)playhead_ / 60, (int)playhead_ % 60, (int)duration_ / 60, (int)duration_ % 60);
  return buf;
}
