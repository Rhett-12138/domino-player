#include "audio.h"
#include <cstdio>
#include <cstring>

#define CHK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { fprintf(stderr, "audio err 0x%08x %s\n", (unsigned)hr_, #x); return false; } } while (0)

bool WasapiAudioOut::init() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator* enumerator = nullptr;
  CHK(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)));
  CHK(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &dev_));
  CHK(dev_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client_));

  WAVEFORMATEX* mix = nullptr;
  CHK(client_->GetMixFormat(&mix));
  deviceRate = mix->nSamplesPerSec;
  deviceChannels = mix->nChannels;

  HRESULT hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
    500000, 0, mix, nullptr);  // 500000 个100ns = 50ms 缓冲，否则3秒缓冲导致声音延迟
  CoTaskMemFree(mix);
  if (FAILED(hr)) { fprintf(stderr, "audio init fail 0x%08x\n", (unsigned)hr); return false; }

  CHK(client_->GetBufferSize(&bufferFrames_));
  CHK(client_->GetService(__uuidof(IAudioRenderClient), (void**)&render_));
  client_->GetService(__uuidof(IAudioClock), (void**)&clockI_);
  evt_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  CHK(client_->SetEventHandle(evt_));

  ring_.assign(bufferFrames_ * 4 * deviceChannels, 0.f);
  ringPos_ = 0;
  ringFill_ = 0;
  writtenFrames_ = 0;
  return true;
}

bool WasapiAudioOut::start() {
  running_ = true;
  thread_ = CreateThread(nullptr, 0, renderThread, this, 0, nullptr);
  client_->Start();
  return thread_ != nullptr;
}

void WasapiAudioOut::stop() {
  running_ = false;
  if (evt_) SetEvent(evt_);
  if (thread_) { WaitForSingleObject(thread_, 1000); CloseHandle(thread_); thread_ = nullptr; }
  if (client_) client_->Stop();
  if (clockI_) { clockI_->Release(); clockI_ = nullptr; }
}

void WasapiAudioOut::pause() {
  if (client_) client_->Stop();
}

void WasapiAudioOut::resume() {
  if (client_) client_->Start();
}

void WasapiAudioOut::push(const float* data, int frames) {
  if (frames <= 0) return;
  std::lock_guard<std::mutex> lk(mu_);
  size_t total = (size_t)frames * deviceChannels;  // 样本数
  size_t cap = ring_.size();
  size_t head = (ringPos_ + ringFill_) % cap;
  for (size_t i = 0; i < total; i++) {
    if (ringFill_ >= cap) break; // 满了丢弃
    ring_[head] = data[i];
    head = (head + 1) % cap;
    ringFill_++;
  }
}

void WasapiAudioOut::flush() {
  std::lock_guard<std::mutex> lk(mu_);
  ringFill_ = 0;
  ringPos_ = 0;
  writtenFrames_ = 0;
}

size_t WasapiAudioOut::bufferedFrames() {
  std::lock_guard<std::mutex> lk(mu_);
  return ringFill_ / deviceChannels;  // 样本数 → 帧数
}

double WasapiAudioOut::clock() const {
  UINT64 gpos = 0, qpc = 0;
  if (clockI_) clockI_->GetPosition(&gpos, &qpc);
  if (gpos > 0) return (double)gpos / deviceRate;
  UINT32 pad = 0;
  if (client_) client_->GetCurrentPadding(&pad);
  uint64_t presented = writtenFrames_ > pad ? writtenFrames_ - pad : 0;
  return (double)presented / deviceRate;
}

DWORD WINAPI WasapiAudioOut::renderThread(LPVOID p) {
  ((WasapiAudioOut*)p)->renderLoop();
  return 0;
}

void WasapiAudioOut::renderLoop() {
  while (running_) {
    DWORD w = WaitForSingleObject(evt_, 100);
    if (w == WAIT_TIMEOUT) continue;
    if (!running_) break;
    UINT32 pad = 0;
    client_->GetCurrentPadding(&pad);
    UINT32 avail = bufferFrames_ - pad;
    if (avail == 0) continue;
    BYTE* buf = nullptr;
    if (FAILED(render_->GetBuffer(avail, &buf))) continue;
    float* fbuf = (float*)buf;
    UINT32 got = 0;
    {
      std::lock_guard<std::mutex> lk(mu_);
      while (got < avail && ringFill_ >= (size_t)deviceChannels) {
        for (int c = 0; c < deviceChannels; c++)
          fbuf[(size_t)got * deviceChannels + c] = ring_[(ringPos_ + c) % ring_.size()];
        ringPos_ = (ringPos_ + deviceChannels) % ring_.size();
        ringFill_ -= deviceChannels;
        got++;
      }
    }
    // 不足部分补静音，避免把未写入的陈旧数据当有效音频播放
    if (got < avail)
      memset(fbuf + (size_t)got * deviceChannels, 0,
        (size_t)(avail - got) * deviceChannels * sizeof(float));
    writtenFrames_ += got;
    render_->ReleaseBuffer(avail, 0);
  }
}
