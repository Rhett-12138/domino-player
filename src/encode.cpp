#include "encode.h"
#include "config.h"
#include "log.h"
#include "ffmpeg_raii.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <deque>
#include <chrono>
#include <filesystem>
#include <condition_variable>
#include <mutex>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
}

int chooseSRInputWidth(int srcW, int targetW, SRInputQuality q, int scale) {
  if (scale <= 0) scale = 4;
  const int fast = (targetW + scale - 1) / scale;
  if (targetW <= 0 || srcW <= 0) return 0;
  // 限制中间帧峰值：1080p→4K FullSource 实测 ~1.9GB；2K 源不再放大到 10240。
  const int cap = 1920;
  switch (q) {
    case SRInputQuality::Fast:
      return fast;
    case SRInputQuality::Balanced:
      // 真正的中间档：默认 3/4 源宽，避免 1080p→4K 直接等同 FullSource。
      return (std::max)(fast, (std::min)(cap, (std::min)(srcW, (std::max)(srcW * 3 / 4, fast))));
    case SRInputQuality::FullSource:
    default:
      return (std::max)(fast, (std::min)(srcW, cap));
  }
}

bool encodeOffline(const char* srcPath, const char* outPath, int targetW, const EncodeOptions& opts) {
  bool verbose = opts.verbose;
  EncodeProgress* prog = opts.prog;
  double startTime = opts.startTime;
  long long startFrame = opts.startFrame;
  // ---- 源：独立解复用器，视频解码 + 音频流复制 ----
  FmtCtxGuard sf;
  if (avformat_open_input(&sf.p, srcPath, nullptr, nullptr) < 0) { fprintf(stderr, "open source failed\n"); return false; }
  if (avformat_find_stream_info(sf.p, nullptr) < 0) { fprintf(stderr, "stream info failed\n"); return false; }
  int vsi = av_find_best_stream(sf.p, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (vsi < 0) { fprintf(stderr, "no video stream\n"); return false; }
  int asi = av_find_best_stream(sf.p, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  AVStream* vs = sf.p->streams[vsi];
  int srcW = vs->codecpar->width;
  int srcH = vs->codecpar->height;
  // 视频输出 PTS 从 0 重建；音频包也必须减掉同一媒体起点，避免 .ts 等
  // start_time≈1.4s 的素材出现固定唇音不同步。
  const double videoStartSec = vs->start_time != AV_NOPTS_VALUE
    ? vs->start_time * av_q2d(vs->time_base) : 0.0;
  AVRational srcRate = vs->avg_frame_rate;
  double srcFps = av_q2d(srcRate);
  if (srcRate.num <= 0 || srcRate.den <= 0 || srcFps <= 0 || srcFps > 240 ||
      srcRate.den > 1001 || srcRate.num > 240 * 1001) {
    // 异常/VFR 的 avg_frame_rate 会得到极端 time_base；按 CFR 30fps 回退
    srcRate = AVRational{ 30, 1 };
    srcFps = 30;
  }

  // 断点续编：seek 到检查点（解码器在下方新开，无需 flush）
  if (startTime > 0) {
    double tb = av_q2d(vs->time_base);
    if (tb > 0) av_seek_frame(sf.p, vsi, (int64_t)(startTime / tb), AVSEEK_FLAG_BACKWARD);
  }

  // 视频解码器
  const AVCodec* vdec = avcodec_find_decoder(vs->codecpar->codec_id);
  if (!vdec) { fprintf(stderr, "no video decoder\n"); return false; }
  CodecCtxGuard vctx;
  vctx.p = avcodec_alloc_context3(vdec);
  avcodec_parameters_to_context(vctx.p, vs->codecpar);
  if (avcodec_open2(vctx.p, vdec, nullptr) < 0) { fprintf(stderr, "video decoder open failed\n"); return false; }
  SwsGuard swsIn;
  swsIn.p = sws_getContext(srcW, srcH, vctx.p->pix_fmt, srcW, srcH, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);

  // ---- 超分 ----
  bool needSR = srcW < targetW;
  SR sr;
  if (needSR) {
    const std::string base = opts.modelBase.empty() ? Config::defaults().modelBase : opts.modelBase;
    const std::string param = joinModelPath(base, srModelParamName(opts.model));
    const std::string bin = joinModelPath(base, srModelBinName(opts.model));
    if (!sr.init(param.c_str(), bin.c_str())) {
      fprintf(stderr, "sr init failed\n");
      return false;
    }
  }
  int outW = needSR ? targetW : srcW;
  int outH = needSR ? (int)(((size_t)srcH * targetW / srcW) & ~1) : srcH;
  const int srScale = srModelScale(opts.model);
  int inW = needSR ? chooseSRInputWidth(srcW, targetW, opts.srInput, srScale) : srcW;
  // 高质量模型（x4plus / x4plus-anime）显存/中间特征更大：实测 1280 输入
  // 会失败，960（4x=3840）稳定；限制上限，避免整任务回退源帧。
  if (needSR && srModelIsHQ(opts.model) && inW > 960) inW = 960;
  if (inW > 1) inW &= ~1;   // 模型输入宽保持偶数
  const int inH = needSR ? (int)(((int64_t)srcH * inW / srcW) & ~1) : srcH;
  const int encInW = needSR ? inW * srScale : srcW;
  const int encInH = needSR ? inH * srScale : srcH;

  // ---- 输出：fMP4（边写边可播放）+ h264 + 音频流复制 ----
  OutFmtGuard ofmt;
  avformat_alloc_output_context2(&ofmt.p, nullptr, "mp4", outPath);
  if (!ofmt.p) { fprintf(stderr, "muxer alloc failed\n"); return false; }
  // 续编产物：delay_moov（尾部 moov 便于剥离）+ default_base_moof（分片偏移相对 moof，拼接后仍有效）
  const char* flags = startTime > 0
    ? "frag_keyframe+empty_moov+delay_moov+default_base_moof"
    : "frag_keyframe+empty_moov";
  av_opt_set(ofmt.p->priv_data, "movflags", flags, 0);

  const AVCodec* enc = avcodec_find_encoder_by_name("libx264");
  if (!enc) enc = avcodec_find_encoder(AV_CODEC_ID_H264);
  if (!enc) { fprintf(stderr, "no h264 encoder\n"); return false; }
  CodecCtxGuard ectx;
  ectx.p = avcodec_alloc_context3(enc);
  ectx.p->width = outW;
  ectx.p->height = outH;
  // 保留精确有理帧率（如 30000/1001），避免 29.97/23.976 源被取整后 A/V 漂移
  ectx.p->time_base = av_inv_q(srcRate);
  ectx.p->framerate = srcRate;
  ectx.p->pix_fmt = AV_PIX_FMT_YUV420P;
  // 离线默认质量优先：CRF 18；opts.crf=0 时回退固定码率
  ectx.p->bit_rate = opts.crf > 0 ? 0 : 10 * 1000 * 1000;
  ectx.p->gop_size = 60;
  // 关闭 B 帧：B 帧会让首个 DTS 为负，fMP4 封装把时间轴整体后移约 2 帧，
  // 而音频仍从 0 开始，形成固定唇音不同步。
  ectx.p->max_b_frames = 0;
  ectx.p->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;  // SPS/PPS 放 extradata，fMP4 的 moov 在文件开头需要它
  av_opt_set(ectx.p->priv_data, "preset", opts.preset.c_str(), 0);
  if (opts.crf > 0) av_opt_set_int(ectx.p->priv_data, "crf", opts.crf, 0);
  // 关闭编码前瞻：否则视频包晚于音频数秒才输出，fMP4 交错时会把首个视频包
  // 之后的帧整体后移，造成音画错位/首帧显示数秒。
  av_opt_set(ectx.p->priv_data, "tune", "zerolatency", 0);
  if (avcodec_open2(ectx.p, enc, nullptr) < 0) { fprintf(stderr, "encoder open failed\n"); return false; }

  AVStream* ost = avformat_new_stream(ofmt.p, nullptr);
  ost->time_base = ectx.p->time_base;
  avcodec_parameters_from_context(ost->codecpar, ectx.p);
  ost->codecpar->codec_tag = 0;   // 交给 mp4 muxer 选择 avc1/hvc1 等 tag

  AVStream* oas = nullptr;
  if (asi >= 0) {
    oas = avformat_new_stream(ofmt.p, nullptr);
    avcodec_parameters_copy(oas->codecpar, sf.p->streams[asi]->codecpar);
    oas->codecpar->codec_tag = 0;   // .ts 的 tag（如 0x0f）不能直接用于 mp4
    oas->time_base = sf.p->streams[asi]->time_base;
  }
  int64_t audioOffset = 0;   // 在 write_header 之后按 muxer 最终 time_base 计算

  // .ts 里的 AAC 是 ADTS，直接复制到 mp4 会报 malformed；需要 adtstoasc 裸流转换
  AVBSFContext* absf = nullptr;
  bool needAdts = false;
  if (oas && oas->codecpar->codec_id == AV_CODEC_ID_AAC) {
    needAdts = sf.p->streams[asi]->codecpar->codec_tag == 0x0f ||
      (sf.p->iformat && sf.p->iformat->name &&
       std::string(sf.p->iformat->name).find("mpegts") != std::string::npos);
  }
  if (needAdts) {
    const AVBitStreamFilter* f = av_bsf_get_by_name("aac_adtstoasc");
    if (!f || av_bsf_alloc(f, &absf) < 0) {
      fprintf(stderr, "aac_adtstoasc init failed\n");
      return false;
    }
    avcodec_parameters_copy(absf->par_in, sf.p->streams[asi]->codecpar);
    absf->time_base_in = sf.p->streams[asi]->time_base;
    if (av_bsf_init(absf) < 0) {
      av_bsf_free(&absf);
      fprintf(stderr, "aac_adtstoasc init failed\n");
      return false;
    }
  }

  if (!(ofmt.p->oformat->flags & AVFMT_NOFILE)) {
    if (avio_open(&ofmt.p->pb, outPath, AVIO_FLAG_WRITE) < 0) return false;
  }
  if (avformat_write_header(ofmt.p, nullptr) < 0) {
    fprintf(stderr, "write header failed\n");
    return false;
  }
  // muxer 可能在写 header 后调整 stream time_base（如音频改为 1/44100），必须重新计算
  if (oas && videoStartSec > 0.0001) {
    audioOffset = av_rescale_q((int64_t)llround(videoStartSec * AV_TIME_BASE),
                               AV_TIME_BASE_Q, oas->time_base);
  }

  // ---- 编码循环 ----
  SwsGuard swsOut;
  swsOut.p = sws_getContext(encInW, encInH, AV_PIX_FMT_RGB24, outW, outH, AV_PIX_FMT_YUV420P,
                            SWS_LANCZOS, nullptr, nullptr, nullptr);
  if (!swsOut.p) { fprintf(stderr, "output sws init failed\n"); return false; }
  int curSwsW = encInW;
  int curSwsH = encInH;
  // SR 推理失败时回退源帧：用独立缩放器，绝不静默丢帧。
  SwsGuard swsFallback;
  if (needSR) {
    swsFallback.p = sws_getContext(srcW, srcH, AV_PIX_FMT_RGB24, outW, outH, AV_PIX_FMT_YUV420P,
                                   SWS_LANCZOS, nullptr, nullptr, nullptr);
    if (!swsFallback.p) { fprintf(stderr, "fallback sws init failed\n"); return false; }
  }
  SwsGuard swsPrev, swsPrevSrc;
  std::vector<uint8_t> prevRgb;
  if (opts.preview && needSR) {
    if (encInW != outW || encInH != outH) {
      swsPrev.p = sws_getContext(encInW, encInH, AV_PIX_FMT_RGB24, outW, outH, AV_PIX_FMT_RGB24,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr);
    }
    if (srcW != outW || srcH != outH) {
      swsPrevSrc.p = sws_getContext(srcW, srcH, AV_PIX_FMT_RGB24, outW, outH, AV_PIX_FMT_RGB24,
                                    SWS_BILINEAR, nullptr, nullptr, nullptr);
    }
    prevRgb.resize((size_t)outW * outH * 3);
  }
  const size_t srcBytes = (size_t)srcW * srcH * 3;
  std::vector<uint8_t> rgb(srcBytes);
  std::vector<uint8_t> yuv((size_t)outW * outH * 3 / 2);
  std::vector<std::vector<uint8_t>> freeSrc;
  std::vector<std::vector<uint8_t>> freeOut;

  long long count = 0;
  long long srFallbackCount = 0;
  auto t0 = std::chrono::steady_clock::now();
  auto lastCheck = t0;
  if (prog) {
    long long total = (long long)(sf.p->duration > 0 ? (double)sf.p->duration / AV_TIME_BASE * srcFps : 0);
    prog->totalFrames.store(total);
  }

  PktGuard ipkt;
  ipkt.p = av_packet_alloc();
  FrameGuard fr;
  fr.p = av_frame_alloc();
  bool cancelled = false;
  bool writeError = false;

  auto writeAudioPacket = [&](AVPacket* p) {
    p->stream_index = oas->index;
    if (av_interleaved_write_frame(ofmt.p, p) < 0) writeError = true;
  };

  // 音频最多领先视频 100ms 写出；否则 muxer 会把晚到的视频包整体后移
  std::deque<AVPacket*> audioQ;
  auto queueAudio = [&](AVPacket* p) {
    AVPacket* q = av_packet_alloc();
    if (!q) { writeError = true; return; }
    av_packet_move_ref(q, p);
    audioQ.push_back(q);
  };
  auto flushAudio = [&](int64_t limitOas) {
    while (!audioQ.empty()) {
      AVPacket* p = audioQ.front();
      int64_t pts = p->pts != AV_NOPTS_VALUE ? p->pts : p->dts;
      if (limitOas != AV_NOPTS_VALUE && pts != AV_NOPTS_VALUE && pts > limitOas) break;
      writeAudioPacket(p);
      av_packet_free(&p);
      audioQ.pop_front();
    }
  };
  auto queueAudioFromBsf = [&](AVPacket* p) {
    av_packet_rescale_ts(p, absf->time_base_out, oas->time_base);
    if (audioOffset != 0) {
      if (p->pts != AV_NOPTS_VALUE) p->pts -= audioOffset;
      if (p->dts != AV_NOPTS_VALUE) p->dts -= audioOffset;
    }
    queueAudio(p);
  };

  auto drainEncoder = [&] {
    PktGuard op;
    op.p = av_packet_alloc();
    while (avcodec_receive_packet(ectx.p, op.p) == 0) {
      av_packet_rescale_ts(op.p, ectx.p->time_base, ost->time_base);
      op.p->stream_index = ost->index;
      if (oas && op.p->pts != AV_NOPTS_VALUE) {
        const int64_t lead = av_rescale_q(1, AVRational{ 1, 10 }, ost->time_base);
        flushAudio(av_rescale_q(op.p->pts + lead, ost->time_base, oas->time_base));
      }
      if (av_interleaved_write_frame(ofmt.p, op.p) < 0) writeError = true;
      av_packet_unref(op.p);
    }
  };

  // SR 与解码/缩放/编码流水线：主线程继续解码、写音频和编码，SR 在独立线程
  // 运行。结果按 FIFO 取回，保证视频 PTS 顺序不变。
  struct SrJob {
    std::vector<uint8_t> src;
    std::vector<uint8_t> out;
    int w;
    int h;
    double ft;
  };
  struct SrResult {
    std::vector<uint8_t> src;
    std::vector<uint8_t> rgb;
    int w = 0;
    int h = 0;
    int srRet = 0;
    bool fallback = false;
    double ft = 0;
  };

  const size_t kMaxSrQueue = 2;
  std::mutex srMu;
  std::condition_variable srCv;
  std::deque<SrJob> srJobs;
  std::deque<SrResult> srResults;
  bool srStop = false;
  bool srAbort = false;
  std::thread srThread;

  auto acquireSrc = [&] {
    if (!freeSrc.empty()) {
      auto b = std::move(freeSrc.back());
      freeSrc.pop_back();
      if (b.size() != srcBytes) b.resize(srcBytes);
      return b;
    }
    return std::vector<uint8_t>(srcBytes);
  };

  auto acquireOut = [&] {
    if (!freeOut.empty()) {
      auto b = std::move(freeOut.back());
      freeOut.pop_back();
      return b;
    }
    return std::vector<uint8_t>();
  };

  auto encodeResult = [&](SrResult r) {
    int sw = r.w;
    int sh = r.h;
    uint8_t* sp = r.rgb.empty() ? r.src.data() : r.rgb.data();
    SwsContext* yuvSws = swsOut.p;
    bool fallback = r.fallback;

    if (fallback) {
      // SR 失败不能丢帧：回退源帧，最终任务会判失败
      logx::error("offline SR failed: sw=%d expected=%dx%d; fallback source",
                  r.srRet, encInW, encInH);
      srFallbackCount++;
      sw = srcW;
      sh = srcH;
      sp = r.src.data();
      yuvSws = swsFallback.p;
    } else if (sw != curSwsW || sh != curSwsH) {
      // Real-CUGAN 等模型会裁掉边缘（输出比 scale*input 小），按实际尺寸重建缩放
      if (swsOut.p) sws_freeContext(swsOut.p);
      swsOut.p = sws_getContext(sw, sh, AV_PIX_FMT_RGB24, outW, outH, AV_PIX_FMT_YUV420P,
                                SWS_LANCZOS, nullptr, nullptr, nullptr);
      if (!swsOut.p) {
        logx::error("dynamic output sws failed: %dx%d -> %dx%d", sw, sh, outW, outH);
        srFallbackCount++;
        sw = srcW;
        sh = srcH;
        sp = r.src.data();
        yuvSws = swsFallback.p;
        fallback = true;
      } else {
        if (opts.preview) {
          if (swsPrev.p) sws_freeContext(swsPrev.p);
          swsPrev.p = sws_getContext(sw, sh, AV_PIX_FMT_RGB24, outW, outH, AV_PIX_FMT_RGB24,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
        }
        curSwsW = sw;
        curSwsH = sh;
      }
    }

    int sl[1] = { sw * 3 };
    if (opts.preview) {
      if (sw != outW || sh != outH) {
        SwsContext* pc = fallback ? swsPrevSrc.p : swsPrev.p;
        if (pc) {
          uint8_t* pd[1] = { prevRgb.data() };
          int pl[1] = { outW * 3 };
          sws_scale(pc, &sp, sl, 0, sh, pd, pl);
          opts.preview(prevRgb.data(), outW, outH, opts.previewUserdata);
        }
      } else {
        opts.preview(sp, outW, outH, opts.previewUserdata);
      }
    }

    size_t ysize = (size_t)outW * outH;
    uint8_t* d[3] = { yuv.data(), yuv.data() + ysize, yuv.data() + ysize + ysize / 4 };
    int ds[3] = { outW, outW / 2, outW / 2 };
    sws_scale(yuvSws, &sp, sl, 0, sh, d, ds);

    FrameGuard f;
    f.p = av_frame_alloc();
    f.p->format = ectx.p->pix_fmt;
    f.p->width = outW;
    f.p->height = outH;
    f.p->pts = startFrame + count;  // 续编时帧号延后
    av_image_fill_arrays(f.p->data, f.p->linesize, yuv.data(), AV_PIX_FMT_YUV420P, outW, outH, 1);
    int sret = avcodec_send_frame(ectx.p, f.p);
    while (sret == AVERROR(EAGAIN)) {
      drainEncoder();
      sret = avcodec_send_frame(ectx.p, f.p);
    }
    if (sret == 0) drainEncoder();
    count++;
    if (prog) {
      prog->frame.store(count + startFrame);
      if (r.ft > 0) prog->curTime.store(r.ft);  // 检查点：最近帧的源时间
      double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      if (el > 0.5) prog->fps.store(count / el);
      // 周期持久化断点（约每 1s），应用被杀后仍可续编
      auto now = std::chrono::steady_clock::now();
      if (prog->checkpointCb && std::chrono::duration<double>(now - lastCheck).count() >= 1.0) {
        lastCheck = now;
        prog->checkpointCb();
      }
    }
    if (verbose && count % 30 == 0) {
      double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      printf("encoded %lld frames (%.1f fps)\n", count, count / el);
    }

    // 源帧/输出缓冲回到主线程池，避免每帧重新分配大块 RGB 内存
    if (r.src.size() == srcBytes) freeSrc.push_back(std::move(r.src));
    if (!r.rgb.empty()) freeOut.push_back(std::move(r.rgb));
  };

  auto drainSr = [&] {
    for (;;) {
      SrResult r;
      {
        std::lock_guard<std::mutex> lk(srMu);
        if (srResults.empty()) break;
        r = std::move(srResults.front());
        srResults.pop_front();
      }
      encodeResult(std::move(r));
    }
  };

  auto submitFrame = [&](AVFrame* frame) -> bool {
    double ft = 0;
    if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
      ft = frame->best_effort_timestamp * av_q2d(vs->time_base);
    }
    // 断点续编：跳过检查点之前的帧（仅解码预热，不重编码）
    if (startTime > 0 && ft < startTime) return true;

    uint8_t* d0[1] = { rgb.data() };
    int dl0[1] = { srcW * 3 };
    sws_scale(swsIn.p, frame->data, frame->linesize, 0, srcH, d0, dl0);

    SrJob job{ std::move(rgb), acquireOut(), srcW, srcH, ft };
    rgb = acquireSrc();
    for (;;) {
      drainSr();
      if (prog && prog->cancel.load()) {
        cancelled = true;
        return false;
      }
      if (writeError) return false;

      std::unique_lock<std::mutex> lk(srMu);
      if (srAbort) return false;
      if (srJobs.size() < kMaxSrQueue) {
        srJobs.push_back(std::move(job));
        lk.unlock();
        srCv.notify_all();
        return true;
      }
      // 队列满时等待 SR 取走一帧；超时后回头 drain 已完成的结果
      srCv.wait_for(lk, std::chrono::milliseconds(10));
    }
  };

  auto finishSr = [&](bool abort) {
    if (!needSR) return;
    {
      std::lock_guard<std::mutex> lk(srMu);
      srAbort = abort;
      srStop = !abort;
    }
    srCv.notify_all();
    if (srThread.joinable()) srThread.join();
    if (!abort) {
      drainSr();
    } else {
      std::lock_guard<std::mutex> lk(srMu);
      srJobs.clear();
      srResults.clear();
    }
    sr.clear();
  };

  if (needSR) {
    srThread = std::thread([&] {
      for (;;) {
        SrJob job;
        {
          std::unique_lock<std::mutex> lk(srMu);
          srCv.wait(lk, [&] {
            return srAbort || !srJobs.empty() || (srStop && srJobs.empty());
          });
          if (srAbort || (srStop && srJobs.empty())) break;
          job = std::move(srJobs.front());
          srJobs.pop_front();
        }
        srCv.notify_all();  // 让 submit 继续投递下一帧

        SrResult res;
        res.ft = job.ft;
        res.src = std::move(job.src);
        res.rgb = std::move(job.out);
        const int sw = sr.process(res.src.data(), job.w, job.h, inW, res.rgb);
        res.srRet = sw;
        if (sw > 0) {
          res.w = sw;
          res.h = (int)(res.rgb.size() / (size_t)sw / 3);
        } else {
          res.w = job.w;
          res.h = job.h;
          res.fallback = true;
        }
        {
          std::lock_guard<std::mutex> lk(srMu);
          srResults.push_back(std::move(res));
        }
        srCv.notify_all();
      }
    });
  }

  // 无 SR 时沿用同步编码；needSR 走上面的并发 SR 流水线。
  auto encodeDecodedFrame = [&](AVFrame* frame) {
    // 断点续编：跳过检查点之前的帧（仅解码预热，不重编码）
    if (startTime > 0) {
      double ft = frame->best_effort_timestamp * av_q2d(vs->time_base);
      if (ft < startTime) return;
    }
    uint8_t* d0[1] = { rgb.data() };
    int dl0[1] = { srcW * 3 };
    sws_scale(swsIn.p, frame->data, frame->linesize, 0, srcH, d0, dl0);

    const int sw = srcW;
    const int sh = srcH;
    uint8_t* sp = rgb.data();
    int sl[1] = { sw * 3 };
    if (opts.preview) opts.preview(sp, outW, outH, opts.previewUserdata);

    size_t ysize = (size_t)outW * outH;
    uint8_t* d[3] = { yuv.data(), yuv.data() + ysize, yuv.data() + ysize + ysize / 4 };
    int ds[3] = { outW, outW / 2, outW / 2 };
    sws_scale(swsOut.p, &sp, sl, 0, sh, d, ds);

    FrameGuard f;
    f.p = av_frame_alloc();
    f.p->format = ectx.p->pix_fmt;
    f.p->width = outW;
    f.p->height = outH;
    f.p->pts = startFrame + count;  // 续编时帧号延后
    av_image_fill_arrays(f.p->data, f.p->linesize, yuv.data(), AV_PIX_FMT_YUV420P, outW, outH, 1);
    int sret = avcodec_send_frame(ectx.p, f.p);
    while (sret == AVERROR(EAGAIN)) {
      drainEncoder();
      sret = avcodec_send_frame(ectx.p, f.p);
    }
    if (sret == 0) drainEncoder();
    count++;
    if (prog) {
      prog->frame.store(count + startFrame);
      double ft = frame->best_effort_timestamp * av_q2d(vs->time_base);
      if (ft > 0) prog->curTime.store(ft);  // 检查点：最近帧的源时间
      double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      if (el > 0.5) prog->fps.store(count / el);
      // 周期持久化断点（约每 1s），应用被杀后仍可续编
      auto now = std::chrono::steady_clock::now();
      if (prog->checkpointCb && std::chrono::duration<double>(now - lastCheck).count() >= 1.0) {
        lastCheck = now;
        prog->checkpointCb();
      }
    }
    if (verbose && count % 30 == 0) {
      double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      printf("encoded %lld frames (%.1f fps)\n", count, count / el);
    }
  };
  auto drainDecoder = [&]() -> bool {
    while (avcodec_receive_frame(vctx.p, fr.p) == 0) {
      if (needSR) {
        if (!submitFrame(fr.p)) return false;
      } else {
        encodeDecodedFrame(fr.p);
      }
    }
    return true;
  };

  while (av_read_frame(sf.p, ipkt.p) >= 0) {
    if (prog && prog->cancel.load()) {
      cancelled = true;
      break;
    }
    if (writeError) break;
    if (ipkt.p->stream_index == vsi) {
      int sret = avcodec_send_packet(vctx.p, ipkt.p);
      while (sret == AVERROR(EAGAIN)) {
        // 解码器缓冲满：先取帧，再重试未消费的包，不能丢弃
        int r = avcodec_receive_frame(vctx.p, fr.p);
        if (r == AVERROR(EAGAIN)) {
          // 理论上 send EAGAIN 时 receive 应可出帧；避免死循环
          logx::warn("decoder receive EAGAIN while draining");
          break;
        }
        if (r < 0) {
          logx::error("decoder receive failed: %d", r);
          break;
        }
        if (needSR) {
          if (!submitFrame(fr.p)) {
            if (!writeError) cancelled = true;
            break;
          }
        } else {
          encodeDecodedFrame(fr.p);
        }
        sret = avcodec_send_packet(vctx.p, ipkt.p);
      }
      if (sret == 0 && !drainDecoder() && !writeError) cancelled = true;
    } else if (oas && ipkt.p->stream_index == asi) {
      double at = ipkt.p->pts != AV_NOPTS_VALUE ? ipkt.p->pts * av_q2d(sf.p->streams[asi]->time_base) : 0;
      // 视频起点之前的音频不写入，避免输出负时间戳
      if (videoStartSec > 0.0001 && at + 1e-6 < videoStartSec) { av_packet_unref(ipkt.p); continue; }
      // 断点续编：跳过检查点之前的音频包
      if (startTime > 0 && at < startTime) { av_packet_unref(ipkt.p); continue; }
      if (absf) {
        // BSF 输入保持源时间基（time_base_in），输出再转到 oas 时间基
        if (av_bsf_send_packet(absf, ipkt.p) < 0) {
          writeError = true;
        } else {
          while (av_bsf_receive_packet(absf, ipkt.p) == 0) {
            queueAudioFromBsf(ipkt.p);
            av_packet_unref(ipkt.p);
          }
        }
      } else {
        av_packet_rescale_ts(ipkt.p, sf.p->streams[asi]->time_base, oas->time_base);
        if (audioOffset != 0) {
          if (ipkt.p->pts != AV_NOPTS_VALUE) ipkt.p->pts -= audioOffset;
          if (ipkt.p->dts != AV_NOPTS_VALUE) ipkt.p->dts -= audioOffset;
        }
        queueAudio(ipkt.p);
      }
    }
    av_packet_unref(ipkt.p);
  }

  // flush 音频 BSF（ADTS → ASC 可能缓存尾部）
  if (absf) {
    if (av_bsf_send_packet(absf, nullptr) >= 0) {
      while (av_bsf_receive_packet(absf, ipkt.p) == 0) {
        queueAudioFromBsf(ipkt.p);
        av_packet_unref(ipkt.p);
      }
    }
    av_bsf_free(&absf);
  }

  // flush 解码器 + 编码器
  int dret = avcodec_send_packet(vctx.p, nullptr);
  while (dret == AVERROR(EAGAIN)) {
    if (!drainDecoder() && !writeError) cancelled = true;
    dret = avcodec_send_packet(vctx.p, nullptr);
  }
  if (dret == 0 && !drainDecoder() && !writeError) cancelled = true;
  finishSr(cancelled || writeError);
  int eret = avcodec_send_frame(ectx.p, nullptr);
  while (eret == AVERROR(EAGAIN)) {
    drainEncoder();
    eret = avcodec_send_frame(ectx.p, nullptr);
  }
  if (eret == 0) drainEncoder();
  flushAudio(AV_NOPTS_VALUE);   // 写出剩余音频
  if (av_write_trailer(ofmt.p) < 0) writeError = true;
  double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const bool ok = !cancelled && !writeError && srFallbackCount == 0;
  printf("ENCODE %s frames=%lld fps=%.1f out=%dx%d audio=%d sr_fallback=%lld -> %s\n",
    cancelled ? "CANCELLED" : (writeError ? "ERROR" : (srFallbackCount ? "SR_FAILED" : "DONE")),
    count, count / el, outW, outH, oas ? 1 : 0, srFallbackCount, outPath);
  if (prog) { prog->fps.store(count / el); prog->done.store(true); prog->success.store(ok); }

  return ok;
}

bool remuxFaststart(const char* inPath, const char* outPath, RemuxProgressFn prog, void* ud) {
  AVFormatContext* in = nullptr;
  if (avformat_open_input(&in, inPath, nullptr, nullptr) < 0) { fprintf(stderr, "remux open failed\n"); return false; }
  avformat_find_stream_info(in, nullptr);
  int64_t total = in->pb ? avio_size(in->pb) : 0;
  AVFormatContext* out = nullptr;
  if (avformat_alloc_output_context2(&out, nullptr, "mp4", outPath) < 0) { fprintf(stderr, "remux alloc failed\n"); return false; }
  av_opt_set(out->priv_data, "movflags", "faststart", 0);  // moov 前置，利于在线播放/兼容

  for (unsigned i = 0; i < in->nb_streams; i++) {
    AVStream* is = in->streams[i];
    AVStream* os = avformat_new_stream(out, nullptr);
    avcodec_parameters_copy(os->codecpar, is->codecpar);
    os->time_base = is->time_base;
  }
  if (avio_open(&out->pb, outPath, AVIO_FLAG_WRITE) < 0) return false;
  avformat_write_header(out, nullptr);

  AVPacket* pkt = av_packet_alloc();
  int ret;
  while ((ret = av_read_frame(in, pkt)) >= 0) {
    int si = pkt->stream_index;
    av_packet_rescale_ts(pkt, in->streams[si]->time_base, out->streams[si]->time_base);
    av_interleaved_write_frame(out, pkt);
    av_packet_unref(pkt);
    if (prog && total > 0 && in->pb) {
      int64_t pos = avio_tell(in->pb);
      if (pos > 0) prog((float)pos / (float)total, ud);
    }
  }
  av_packet_free(&pkt);
  av_write_trailer(out);
  avio_closep(&out->pb);
  avformat_free_context(out);
  avformat_close_input(&in);
  if (prog) prog(1.0f, ud);
  return true;
}

// 断点续编合并：把主文件（前段）与续编文件（后段）重封装为新的 fMP4
bool mergeToMp4(const char* mainPath, const char* partPath, const char* outPath) {
  FmtCtxGuard in1, in2;
  if (avformat_open_input(&in1.p, mainPath, nullptr, nullptr) < 0 || avformat_find_stream_info(in1.p, nullptr) < 0) { fprintf(stderr, "merge: open main failed\n"); return false; }
  if (avformat_open_input(&in2.p, partPath, nullptr, nullptr) < 0 || avformat_find_stream_info(in2.p, nullptr) < 0) { fprintf(stderr, "merge: open part failed\n"); return false; }

  OutFmtGuard out;
  if (avformat_alloc_output_context2(&out.p, nullptr, "mp4", outPath) < 0) { fprintf(stderr, "merge: out alloc failed\n"); return false; }
  av_opt_set(out.p->priv_data, "movflags", "frag_keyframe+empty_moov", 0);  // 输出仍为渐进可播 fMP4

  // 建立输出流（从第一个输入拷贝流参数；续编流参数应一致）
  for (unsigned i = 0; i < in1.p->nb_streams; i++) {
    AVStream* is = in1.p->streams[i];
    AVStream* os = avformat_new_stream(out.p, nullptr);
    avcodec_parameters_copy(os->codecpar, is->codecpar);
    os->time_base = is->time_base;
  }

  if (avio_open(&out.p->pb, outPath, AVIO_FLAG_WRITE) < 0) { fprintf(stderr, "merge: out open failed\n"); return false; }
  avformat_write_header(out.p, nullptr);

  PktGuard pkt;
  pkt.p = av_packet_alloc();
  // 主段：写出的同时记录各流最大 dts（输出时间基）
  std::vector<int64_t> mainMaxDts(out.p->nb_streams, AV_NOPTS_VALUE);
  while (av_read_frame(in1.p, pkt.p) >= 0) {
    int si = pkt.p->stream_index;
    if (si < (int)in1.p->nb_streams && si < (int)out.p->nb_streams) {
      av_packet_rescale_ts(pkt.p, in1.p->streams[si]->time_base, out.p->streams[si]->time_base);
      pkt.p->stream_index = si;
      if (pkt.p->dts != AV_NOPTS_VALUE && (mainMaxDts[si] == AV_NOPTS_VALUE || pkt.p->dts > mainMaxDts[si])) mainMaxDts[si] = pkt.p->dts;
      av_interleaved_write_frame(out.p, pkt.p);
    }
    av_packet_unref(pkt.p);
  }

  // 续编段：缓冲全部包，每流计算固定偏移使 dts 从主段末尾衔接
  std::vector<AVPacket*> partPkts;
  std::vector<int64_t> partFirstDts(out.p->nb_streams, AV_NOPTS_VALUE);
  std::vector<int64_t> partLastDts(out.p->nb_streams, AV_NOPTS_VALUE);
  while (av_read_frame(in2.p, pkt.p) >= 0) {
    int si = pkt.p->stream_index;
    if (si < (int)in2.p->nb_streams && si < (int)out.p->nb_streams) {
      av_packet_rescale_ts(pkt.p, in2.p->streams[si]->time_base, out.p->streams[si]->time_base);
      pkt.p->stream_index = si;
      if (partFirstDts[si] == AV_NOPTS_VALUE) partFirstDts[si] = pkt.p->dts;
      if (pkt.p->dts != AV_NOPTS_VALUE &&
          (partLastDts[si] == AV_NOPTS_VALUE || pkt.p->dts > partLastDts[si]))
        partLastDts[si] = pkt.p->dts;
      partPkts.push_back(av_packet_clone(pkt.p));
    }
    av_packet_unref(pkt.p);
  }
  // 幂等合并：只有“主文件末尾已经覆盖分段末尾”才说明该段已合并过（上次替换后未清理）。
  // 不能用分段首帧判断：检查点每秒落盘，崩溃后分段起点可能比主文件末尾早 ~1s，
  // 那仍是需要追加的合法内容。
  bool alreadyMerged = true;
  bool compared = false;
  for (unsigned i = 0; i < out.p->nb_streams; i++) {
    if (mainMaxDts[i] == AV_NOPTS_VALUE || partLastDts[i] == AV_NOPTS_VALUE) continue;
    compared = true;
    const int64_t tol = av_rescale_q(1, AVRational{ 1, 10 }, out.p->streams[i]->time_base);
    if (mainMaxDts[i] + tol < partLastDts[i]) { alreadyMerged = false; break; }
  }
  if (!compared) alreadyMerged = false;
  if (!alreadyMerged) {
    std::vector<int64_t> off(out.p->nb_streams, 0);
    for (unsigned i = 0; i < out.p->nb_streams; i++) {
      if (mainMaxDts[i] != AV_NOPTS_VALUE && partFirstDts[i] != AV_NOPTS_VALUE)
        off[i] = mainMaxDts[i] + 1 - partFirstDts[i];
    }
    for (AVPacket* pp : partPkts) {
      int si = pp->stream_index;
      if (off[si] != 0) {
        if (pp->dts != AV_NOPTS_VALUE) pp->dts += off[si];
        if (pp->pts != AV_NOPTS_VALUE) pp->pts += off[si];
      }
      av_interleaved_write_frame(out.p, pp);
      av_packet_free(&pp);
    }
  } else {
    for (AVPacket* pp : partPkts) av_packet_free(&pp);
  }

  av_write_trailer(out.p);
  return true;
}

