#pragma once
#include "config.h"
#include "decode.h"
#include "sr.h"
#include <algorithm>
#include <atomic>
#include <functional>

// 编码进度（编码线程更新，预览/UI 线程读取）
struct EncodeProgress {
  std::atomic<long long> frame{ 0 };        // 已编码视频帧
  std::atomic<long long> totalFrames{ 0 };  // 总帧数（0=未知）
  std::atomic<double> fps{ 0.0 };
  std::atomic<bool> done{ false };
  std::atomic<bool> success{ false };
  std::atomic<bool> cancel{ false };        // 请求取消
  std::atomic<double> curTime{ 0 };         // 最近编码帧的源时间戳（断点续编检查点）
  std::function<void()> checkpointCb;       // 编码中周期回调（用于持久化断点，供重启后续编）
  void reset() {
    frame.store(0); totalFrames.store(0); fps.store(0.0);
    done.store(false); success.store(false); cancel.store(false); curTime.store(0);
    checkpointCb = nullptr;
  }
};

// 预览回调：编码线程每产出一帧调用（RGB24, w×h），用于实时预览
typedef void (*EncodePreviewFn)(const uint8_t* rgb, int w, int h, void* userdata);

// SR 输入策略：离线默认尽量保留源细节，4x 输出后再缩到目标尺寸。
enum class SRInputQuality {
  Fast,         // target/4，最快，细节损失最大
  Balanced,     // 约 3/4 源宽，兼顾细节与耗时
  FullSource,   // max(target/4, min(srcW,1920))，尽量保留源宽并限制中间帧
};

// 选择模型的输入宽；保证 scale*inW >= targetW（源极小时除外）。
int chooseSRInputWidth(int srcW, int targetW, SRInputQuality q, int scale = 4);

struct EncodeOptions {
  bool verbose = true;
  EncodeProgress* prog = nullptr;
  EncodePreviewFn preview = nullptr;
  void* previewUserdata = nullptr;
  double startTime = 0;         // 从源视频该秒开始（断点续编）；0=从头
  long long startFrame = 0;     // 帧号偏移（续编时继续旧帧号）
  std::string modelBase;        // NCNN 模型目录（空=Config::defaults()）
  std::string onnxBase;         // TensorRT ONNX 目录（空=Config::defaults()）
  std::string trtCacheDir;      // TensorRT engine 缓存目录（空=Config::defaults()）
  SRModel model = SRModel::Realistic;  // 真人/实拍或动画模型
  SRInputQuality srInput = SRInputQuality::FullSource;  // 离线默认质量优先
  bool useTrt = true;           // 可用时使用 TensorRT-RTX，失败回退 NCNN
  int crf = 18;                 // x264 CRF；0=用 bit_rate
  std::string preset = "slow";
};

// 离线超分：全片处理 + libx264 编码输出 fMP4（边写边可播放，含音频流复制）
bool encodeOffline(const char* srcPath, const char* outPath, int targetW,
                   const EncodeOptions& opts = {});

// 转存进度回调（0..1，用户数据透传）
typedef void (*RemuxProgressFn)(float p, void* userdata);

// 转存：fMP4 → 标准 mp4（moov 前置 faststart），流复制不改编码
bool remuxFaststart(const char* inPath, const char* outPath,
                    RemuxProgressFn prog = nullptr, void* ud = nullptr);

// 断点续编合并：把主文件（前段）与续编文件（后段）重封装为新的 fMP4
bool mergeToMp4(const char* mainPath, const char* partPath, const char* outPath);
