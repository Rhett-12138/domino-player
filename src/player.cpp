#include "player.h"
#include "audio.h"
#include "log.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <ncnn/gpu.h>

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
  sr_.clear();
  ncnn::destroy_gpu_instance();
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
    if (paused_) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }

    // 缓冲音频最多超前 ~120ms，避免解码抢跑导致 A/V 漂移/缓冲溢出
    if (audio_->bufferedFrames() > (size_t)(audio_->deviceRate * 0.12)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }

    int n = aud_.nextAudioFrame(srcBuf.data(), 8192, &pts);
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
      float v = (float)volume_;
      if (v != 1.f)
        for (int i = 0; i < outN * audio_->deviceChannels; i++) dstBuf[i] *= v;
      audio_->push(dstBuf.data(), outN);
    }
  }
  if (resampler) swr_free(&resampler);
}

void Player::ensureSR() {
  if (srInited_ || noSR_) return;
  if (sr_.init(cfg_.srParam().c_str(), cfg_.srBin().c_str()))
    srInited_ = true;
  else
    fprintf(stderr, "sr init failed\n");
}

bool Player::open(const std::string& path) {
  stopAudio();
  vid_.close();
  aud_.close();
  if (!vid_.open(path.c_str())) return false;
  ensureSR();
  duration_ = vid_.duration;
  fps_ = vid_.fps;
  if (vid_.hasAudio && audio_->init() && aud_.open(path.c_str()) && aud_.hasAudio)
    hasAudio_ = true;
  else
    hasAudio_ = false;
  paused_ = false;
  playhead_ = 0;
  eof_ = false;
  frameDur_ = fps_ > 0 ? 1.0 / fps_ : 1.0 / 30.0;
  havePending_ = false;
  pendingEOF_ = false;
  presentedFrames_ = 0;
  baseWall_ = 0;
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
  seekPending_.store(true);
}

void Player::seekInternal(double t) {
  vid_.seek(t);
  aud_.seek(t);
  resetAudio_.store(true);
  playhead_ = t;
}

bool Player::processSeek() {
  if (!seekPending_.exchange(false)) return false;
  havePending_ = false;
  pendingEOF_ = false;
  seekInternal(seekTarget_);
  presentedFrames_ = 0;
  baseWall_ = 0;
  return true;
}

void Player::setPaused(bool p) {
  if (paused_ == p) return;
  paused_ = p;
  if (!p) eof_ = false;   // 重新播放即脱离 EOF 态
  if (hasAudio_) {
    if (p) {
      audio_->pause();
      audio_->flush();
    } else {
      if (!audioStarted_.load()) startAudio();
      aud_.seek(playhead_);
      audio_->flush();
      resetAudio_.store(true);
      audio_->resume();
    }
  }
}

void Player::startAudio() {
  if (audioStarted_.exchange(true)) return;
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

void Player::prefetch() {
  if (havePending_ || pendingEOF_) return;
  double pts = 0;
  int r = vid_.nextVideoFrame(rgbBuf_.data(), &pts);
  if (r < 0) { pendingEOF_ = true; return; }
  pendingPts_ = pts;
  bool ready = false;
  if (noSR_) {
    // 关闭超分：直接用源帧
    srgb_.assign(rgbBuf_.data(), rgbBuf_.data() + (size_t)vid_.vw * vid_.vh * 3);
    srOutW_ = vid_.vw; srOutH_ = vid_.vh;
    ready = true;
  } else if (inW_ > 0) {
    // 手动 --in：固定 SR 输入宽（兼容旧用法）
    int sw = sr_.process(rgbBuf_.data(), vid_.vw, vid_.vh, inW_, srgb_);
    if (sw > 0) { srOutW_ = sw; srOutH_ = (int)(srgb_.size() / sw / 3); ready = true; }
  } else if (vid_.vw >= srTargetW_) {
    // 源宽已 ≥ 目标挡位：无需超分，直接用源帧
    srgb_.assign(rgbBuf_.data(), rgbBuf_.data() + (size_t)vid_.vw * vid_.vh * 3);
    srOutW_ = vid_.vw; srOutH_ = vid_.vh;
    ready = true;
  } else {
    // 挡位模式：SR 输入 = 挡位/4，模型 4x 输出 = 挡位
    int inW = srTargetW_ / 4;
    int sw = sr_.process(rgbBuf_.data(), vid_.vw, vid_.vh, inW, srgb_);
    if (sw > 0) { srOutW_ = sw; srOutH_ = (int)(srgb_.size() / sw / 3); ready = true; }
  }
  if (ready) havePending_ = true;
}

bool Player::readyToPresent() {
  if (!havePending_) return false;
  double now = nowWall();
  if (baseWall_ == 0) baseWall_ = now;
  double deadline = calcPresentDeadline(baseWall_, presentedFrames_, frameDur_, vsync_);
  return now >= deadline;
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
  if (paused_) return;
  prefetch();
  if (readyToPresent()) {
    if (presentFrame()) startAudio();   // 第一帧呈现时启动音频
  }
  handleEof();
}

Player::EofAction Player::handleEof() {
  if (!pendingEOF_ || havePending_) return EofAction::None;
  if (liveActive()) return EofAction::LivePresent;  // 上层处理预览/完成
  // 非直播：暂停并回起点
  eof_ = true;              // 暴露"已到结尾"（前端据此实现循环/重播）
  paused_ = true;
  requestSeek(0);
  havePending_ = false;
  return EofAction::Pause;
}

void Player::updateLiveProgress(double frames, double fps) {
  if (fps <= 0) return;
  playhead_ = frames / fps;
  if (playhead_ > duration_) duration_ = playhead_;
}

void Player::releaseForMerge(std::atomic<bool>* fileFree) {
  vid_.close();
  aud_.close();
  stopAudio();
  fileFree->store(true);
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
