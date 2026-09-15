#pragma once
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

struct MediaStream {
  AVFormatContext* fmt = nullptr;
  int vsi = -1;
  int asi = -1;
  AVCodecContext* vctx = nullptr;
  AVCodecContext* actx = nullptr;
  SwsContext* sws = nullptr;
  SwrContext* swr = nullptr;

  int vw = 0, vh = 0;
  double fps = 30;
  double duration = 0;

  bool hasAudio = false;
  int sampleRate = 0;
  int channels = 0;

  bool open(const char* path);
  // 视频帧 → RGB24；返回 0 成功、-1 结束
  int nextVideoFrame(uint8_t* rgbOut, double* outPts);
  // 音频帧 → float32 交错；返回每声道样本数、-1 结束
  int nextAudioFrame(float* outInterleaved, int maxSamples, double* outPts);
  bool seek(double seconds);
  void close();
};
