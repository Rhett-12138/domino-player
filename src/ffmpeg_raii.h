#pragma once
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/frame.h>

// FFmpeg 资源 RAII 守卫：失败/异常路径自动释放，杜绝泄漏与二次释放。
// 用法：在函数顶部声明，后续用 .p 取裸指针。

struct FmtCtxGuard {   // 输入解复用器（close_input 会置空）
  AVFormatContext* p = nullptr;
  ~FmtCtxGuard() { if (p) avformat_close_input(&p); }
};

struct OutFmtGuard {   // 输出封装器（先关 pb 再 free）
  AVFormatContext* p = nullptr;
  ~OutFmtGuard() { if (p) { if (p->pb) avio_closep(&p->pb); avformat_free_context(p); } }
};

struct CodecCtxGuard { // 编解码器上下文
  AVCodecContext* p = nullptr;
  ~CodecCtxGuard() { if (p) avcodec_free_context(&p); }
};

struct SwsGuard {
  SwsContext* p = nullptr;
  ~SwsGuard() { if (p) sws_freeContext(p); }
};

struct SwrGuard {
  SwrContext* p = nullptr;
  ~SwrGuard() { if (p) swr_free(&p); }
};

struct PktGuard {
  AVPacket* p = nullptr;
  ~PktGuard() { if (p) av_packet_free(&p); }
};

struct FrameGuard {
  AVFrame* p = nullptr;
  ~FrameGuard() { if (p) av_frame_free(&p); }
};
