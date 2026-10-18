#include "player.h"
#include "audio.h"
#include "log.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

Player::Player(IFrameOutput& out) : out_(out) {
  audio_ = createDefaultAudioSink();   // 默认 WASAPI；可经 setAudioSink 注入其他实现
}

Player::~Player() {
  shutdown();
}

void Player::shutdown() {
  stopAudio();
  vid_.close();
  aud_.close();
  currentPath_.clear();
  resetClockAnchors();
  sr_.clear();   // 全局 GPU 实例由前端在停止所有任务后统一销毁
  srInited_ = false;
  srFailed_ = false;
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
  if (noSR_ || srFailed_) return;
  if (srInited_ && !sr_.ready()) srInited_ = false;
  if (srInited_) return;
  if (sr_.init(cfg_.srParam().c_str(), cfg_.srBin().c_str())) {
    srInited_ = true;
  } else {
    srFailed_ = true;
    fprintf(stderr, "sr init failed\n");
  }
}

bool Player::open(const std::string& path) {
  stopAudio();
  vid_.close();
  aud_.close();
  currentPath_.clear();
  resetClockAnchors();
  if (!vid_.open(path.c_str())) return false;
  currentPath_ = path;
  srFailed_ = false;
  ensureSR();
  duration_ = vid_.duration;
  fps_ = vid_.fps;
  if (vid_.hasAudio && audio_->init() && aud_.open(path.c_str()) && aud_.hasAudio)
    hasAudio_ = true;
  else
    hasAudio_ = false;
  paused_.store(false);
  playhead_ = 0;
  eof_ = false;
  seekPending_.store(false);
  eofSeek_ = false;
  seekTarget_ = 0;
  havePending_ = false;
  pendingEOF_ = false;
  presentedFrames_ = 0;
  resetClockAnchors();
  frameCount_ = 0;
  lastFps_ = 0;
  fpsT0_ = std::chrono::steady_clock::now();
  tStart_ = std::chrono::steady_clock::now();
  rgbBuf_.resize((size_t)vid_.vw * vid_.vh * 3);
  return true;
}

void Player::close() {
  stopAudio();
  vid_.close();
  aud_.close();
  currentPath_.clear();
  resetClockAnchors();
  playhead_ = 0;
  duration_ = 0;
  fps_ = 30;
  hasAudio_ = false;
  eof_ = false;
}

void Player::requestSeek(double t) {
  if (t < 0) t = 0;
  if (t > duration_) t = duration_;
  seekTarget_ = t;
  eofSeek_ = false;   // 外部 seek 后应脱离 EOF 态
  seekPending_.store(true);
}

void Player::seekInternal(double t) {
  vid_.seek(t);
  {
    std::lock_guard<std::mutex> lk(audMu_);
    aud_.seek(t);
  }
  resetAudio_.store(true);
  playhead_ = t;
  resetClockAnchors();
}

bool Player::processSeek() {
  if (!seekPending_.exchange(false)) return false;
  havePending_ = false;
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
  if (!p) {
    eof_ = false;   // 重新播放即脱离 EOF 态
    // 暂停期间墙钟仍在走，恢复时必须重置呈现基准，否则会按 pump 频率快进追帧
    presentedFrames_ = 0;
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

void Player::prefetch(bool decodeOnly) {
  if (havePending_ || pendingEOF_) return;
  double pts = 0;
  int r = vid_.nextVideoFrame(rgbBuf_.data(), &pts);
  if (r < 0) { pendingEOF_ = true; return; }
  pendingPts_ = pts;
  if (decodeOnly) return;   // 追帧丢弃：只解码，不跑 SR
  if (!noSR_ && !srInited_ && !srFailed_) ensureSR();
  bool useSR = !noSR_ && srInited_;

  auto useSource = [&] {
    srgb_.assign(rgbBuf_.data(), rgbBuf_.data() + (size_t)vid_.vw * vid_.vh * 3);
    srOutW_ = vid_.vw;
    srOutH_ = vid_.vh;
    havePending_ = true;
  };

  // 关闭超分、SR 初始化失败、或源宽已 ≥ 目标挡位：直接用源帧
  if (!useSR || (inW_ <= 0 && vid_.vw >= srTargetW_)) {
    useSource();
    return;
  }

  // 挡位模式：SR 输入 = 挡位/4，模型 4x 输出 = 挡位；--in 为旧用法固定输入宽
  int inW = inW_ > 0 ? inW_ : srTargetW_ / 4;
  int sw = sr_.process(rgbBuf_.data(), vid_.vw, vid_.vh, inW, srgb_);
  if (sw > 0) {
    srOutW_ = sw;
    srOutH_ = (int)(srgb_.size() / sw / 3);
    havePending_ = true;
  } else {
    // 推理失败：本次及后续回退源帧，避免每帧重复失败
    srFailed_ = true;
    useSource();
  }
}

void Player::resetClockAnchors() {
  fallbackWallBase_ = 0;
  fallbackMediaBase_ = 0;
  audioClockBase_ = 0;
  audioMediaBase_ = 0;
  lastAudioClock_ = 0;
  lastAudioClockWall_ = 0;
  lastPumpWall_ = 0;
  catchingUp_ = false;
  catchupDeadline_ = 0;
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
  out_.presentFrame(srgb_.data(), srOutW_, srOutH_);
  playhead_ = pendingPts_;
  presentedFrames_++;
  havePending_ = false;
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
    catchingUp_ = true;
    catchupDeadline_ = now + 2.0;   // 最多追赶 2s，避免弱机永久丢帧
  }
  lastPumpWall_ = now;

  // 仅 Ticker 真正停摆后才追帧；SR/解码持续慢于实时不能丢帧，
  // 否则会永远追不上、几乎不呈现（4K SR 评估时暴露）。
  // 每个 pump 追帧最多 100ms，且只解码不跑 SR；避免一次 pump 卡数十秒。
  if (catchingUp_) {
    havePending_ = false;
    const double catchupStart = nowWall();
    while (!pendingEOF_ && nowWall() - catchupStart < 0.10) {
      if (currentMediaClock() - pendingPts_ <= 0.10) break;
      prefetch(true);
    }
    if (pendingEOF_ || nowWall() >= catchupDeadline_ ||
        currentMediaClock() - pendingPts_ <= 0.10)
      catchingUp_ = false;
  }
  prefetch();
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
