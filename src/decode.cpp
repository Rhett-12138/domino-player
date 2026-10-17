#include "decode.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

bool MediaStream::open(const char* path) {
  close();   // 允许复用对象，且失败路径统一由此清理
  avformat_network_init();
  if (avformat_open_input(&fmt, path, nullptr, nullptr) < 0) { close(); return false; }
  if (avformat_find_stream_info(fmt, nullptr) < 0) { close(); return false; }

  vsi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (vsi < 0) { close(); return false; }
  AVCodecParameters* vcp = fmt->streams[vsi]->codecpar;
  const AVCodec* vdec = avcodec_find_decoder(vcp->codec_id);
  if (!vdec) { close(); return false; }
  vctx = avcodec_alloc_context3(vdec);
  if (!vctx || avcodec_parameters_to_context(vctx, vcp) < 0) { close(); return false; }
  if (avcodec_open2(vctx, vdec, nullptr) < 0) { close(); return false; }
  vw = vctx->width;
  vh = vctx->height;
  duration = (double)fmt->duration / AV_TIME_BASE;
  if (duration <= 0 && fmt->streams[vsi]->duration > 0) {
    duration = (double)fmt->streams[vsi]->duration * av_q2d(fmt->streams[vsi]->time_base);
  }
  AVRational r = fmt->streams[vsi]->avg_frame_rate;
  if (r.num > 0 && r.den > 0) fps = av_q2d(r);
  if (fps <= 0 || fps > 240) fps = 30;
  sws = sws_getContext(vw, vh, vctx->pix_fmt, vw, vh, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!sws) { close(); return false; }

  asi = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (asi >= 0) {
    AVCodecParameters* acp = fmt->streams[asi]->codecpar;
    const AVCodec* adec = avcodec_find_decoder(acp->codec_id);
    if (adec) {
      actx = avcodec_alloc_context3(adec);
      if (actx && avcodec_parameters_to_context(actx, acp) >= 0 &&
          avcodec_open2(actx, adec, nullptr) == 0) {
        sampleRate = actx->sample_rate;
        channels = actx->ch_layout.nb_channels;
        if (channels <= 0) channels = 2;
        AVChannelLayout outLayout;
        av_channel_layout_default(&outLayout, channels);
        swr = nullptr;
        if (swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT, sampleRate,
              &actx->ch_layout, actx->sample_fmt, actx->sample_rate, 0, nullptr) >= 0 &&
            swr_init(swr) >= 0) {
          hasAudio = true;
        } else {
          // 音频不可用不应影响视频播放
          if (swr) swr_free(&swr);
          avcodec_free_context(&actx);
          asi = -1;
          sampleRate = 0;
          channels = 0;
          hasAudio = false;
        }
        av_channel_layout_uninit(&outLayout);
      } else {
        if (actx) avcodec_free_context(&actx);
        asi = -1;
        hasAudio = false;
      }
    }
  }

  pkt = av_packet_alloc();
  fr = av_frame_alloc();
  if (!pkt || !fr) { close(); return false; }
  return true;
}

int MediaStream::nextVideoFrame(uint8_t* rgbOut, double* outPts) {
  if (!fmt || !vctx || !sws || !pkt || !fr) return -1;
  int ret;
  while ((ret = av_read_frame(fmt, pkt)) >= 0) {
    if (pkt->stream_index == vsi) {
      if (avcodec_send_packet(vctx, pkt) == 0) {
        while (avcodec_receive_frame(vctx, fr) == 0) {
          if (outPts) *outPts = fr->best_effort_timestamp * av_q2d(fmt->streams[vsi]->time_base);
          uint8_t* d[1] = { rgbOut };
          int ds[1] = { vw * 3 };
          sws_scale(sws, fr->data, fr->linesize, 0, vh, d, ds);
          av_packet_unref(pkt);
          return 0;
        }
      }
    }
    av_packet_unref(pkt);
  }
  if (avcodec_send_packet(vctx, nullptr) == 0) {
    while (avcodec_receive_frame(vctx, fr) == 0) {
      if (outPts) *outPts = fr->best_effort_timestamp * av_q2d(fmt->streams[vsi]->time_base);
      uint8_t* d[1] = { rgbOut };
      int ds[1] = { vw * 3 };
      sws_scale(sws, fr->data, fr->linesize, 0, vh, d, ds);
      return 0;
    }
  }
  return -1;
}

int MediaStream::nextAudioFrame(float* outInterleaved, int maxSamples, double* outPts) {
  if (!fmt || !actx || !swr || !pkt || !fr) return -1;
  int ret;
  while ((ret = av_read_frame(fmt, pkt)) >= 0) {
    if (pkt->stream_index == asi) {
      if (avcodec_send_packet(actx, pkt) == 0) {
        while (avcodec_receive_frame(actx, fr) == 0) {
          if (outPts) *outPts = fr->best_effort_timestamp * av_q2d(fmt->streams[asi]->time_base);
          int delay = swr_get_delay(swr, actx->sample_rate);
          int outCount = av_rescale_rnd(delay + fr->nb_samples, actx->sample_rate, actx->sample_rate, AV_ROUND_UP);
          if (outCount > maxSamples) outCount = maxSamples;
          convBuf.resize((size_t)outCount * channels * sizeof(float));
          uint8_t* odata = convBuf.data();
          int n = swr_convert(swr, &odata, outCount,
            (const uint8_t**)fr->extended_data, fr->nb_samples);
          if (n > 0) {
            memcpy(outInterleaved, convBuf.data(), (size_t)n * channels * sizeof(float));
            av_packet_unref(pkt);
            return n;
          }
        }
      }
    }
    av_packet_unref(pkt);
  }
  convBuf.resize((size_t)4096 * channels * sizeof(float));
  uint8_t* odata = convBuf.data();
  int n = swr_convert(swr, &odata, 4096, nullptr, 0);
  if (n > 0) {
    memcpy(outInterleaved, convBuf.data(), (size_t)n * channels * sizeof(float));
    return n;
  }
  return -1;
}

bool MediaStream::seek(double seconds) {
  if (!fmt || !vctx || vsi < 0) return false;
  if (seconds < 0) seconds = 0;
  int64_t ts = (int64_t)(seconds / av_q2d(fmt->streams[vsi]->time_base));
  if (av_seek_frame(fmt, vsi, ts, AVSEEK_FLAG_BACKWARD) < 0) return false;
  avcodec_flush_buffers(vctx);
  if (actx) {
    avcodec_flush_buffers(actx);
    if (swr) {
      swr_close(swr);
      swr_init(swr);
    }
  }
  return true;
}

void MediaStream::close() {
  if (pkt) { av_packet_free(&pkt); pkt = nullptr; }
  if (fr) { av_frame_free(&fr); fr = nullptr; }
  convBuf.clear();
  if (swr) { swr_free(&swr); swr = nullptr; }               // swr_free 会置空，显式再置空
  if (sws) { sws_freeContext(sws); sws = nullptr; }         // sws_freeContext 不置空，必须手动置空避免二次释放
  if (actx) { avcodec_free_context(&actx); actx = nullptr; }
  if (vctx) { avcodec_free_context(&vctx); vctx = nullptr; }
  if (fmt) { avformat_close_input(&fmt); fmt = nullptr; }
  // 复位尺寸/状态：close 后 hasVideo()/fps 等应回到"未打开"（可复用对象）
  vw = 0; vh = 0;
  vsi = -1; asi = -1;
  fps = 0;
  duration = 0;
  sampleRate = 0; channels = 0;
  hasAudio = false;
}
