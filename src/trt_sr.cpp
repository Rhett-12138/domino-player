#include "trt_sr.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>

#ifdef DOMINO_HAS_TRT_RTX

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#include "engine.h"
#include <NvInfer.h>
#include <NvOnnxParser.h>
#include <cuda_runtime_api.h>
#include "convert_avx2.h"
#include "trt/gpu_interop.h"

extern "C" {
#include <libswscale/swscale.h>
}

namespace {

using CUresult = int;
using CUdevice = int;
using CUcontext = void*;
using CUdeviceptr = unsigned long long;
using CUstream = void*;

using cuInit_t = CUresult(__stdcall*)(unsigned);
using cuDeviceGet_t = CUresult(__stdcall*)(CUdevice*, int);
using cuDevicePrimaryCtxRetain_t = CUresult(__stdcall*)(CUcontext*, CUdevice);
using cuDevicePrimaryCtxRelease_t = CUresult(__stdcall*)(CUdevice);
using cuCtxSetCurrent_t = CUresult(__stdcall*)(CUcontext);
using cuMemAlloc_t = CUresult(__stdcall*)(CUdeviceptr*, size_t);
using cuMemFree_t = CUresult(__stdcall*)(CUdeviceptr);
using cuMemHostAlloc_t = CUresult(__stdcall*)(void**, size_t, unsigned);
using cuMemFreeHost_t = CUresult(__stdcall*)(void*);
using cuMemcpyHtoD_t = CUresult(__stdcall*)(CUdeviceptr, const void*, size_t);
using cuMemcpyDtoH_t = CUresult(__stdcall*)(void*, CUdeviceptr, size_t);
using cuMemcpyHtoDAsync_t = CUresult(__stdcall*)(CUdeviceptr, const void*, size_t, CUstream);
using cuMemcpyDtoHAsync_t = CUresult(__stdcall*)(void*, CUdeviceptr, size_t, CUstream);
using cuStreamCreate_t = CUresult(__stdcall*)(CUstream*, unsigned);
using cuStreamDestroy_t = CUresult(__stdcall*)(CUstream);
using cuStreamSynchronize_t = CUresult(__stdcall*)(CUstream);

struct Driver {
  HMODULE lib = nullptr;
  cuInit_t init = nullptr;
  cuDeviceGet_t deviceGet = nullptr;
  cuDevicePrimaryCtxRetain_t ctxRetain = nullptr;
  cuDevicePrimaryCtxRelease_t ctxRelease = nullptr;
  cuCtxSetCurrent_t ctxSet = nullptr;
  cuMemAlloc_t memAlloc = nullptr;
  cuMemFree_t memFree = nullptr;
  cuMemHostAlloc_t hostAlloc = nullptr;
  cuMemFreeHost_t hostFree = nullptr;
  cuMemcpyHtoD_t h2d = nullptr;
  cuMemcpyDtoH_t d2h = nullptr;
  cuMemcpyHtoDAsync_t h2dAsync = nullptr;
  cuMemcpyDtoHAsync_t d2hAsync = nullptr;
  cuStreamCreate_t streamCreate = nullptr;
  cuStreamDestroy_t streamDestroy = nullptr;
  cuStreamSynchronize_t streamSync = nullptr;

  bool load() {
    lib = LoadLibraryA("nvcuda.dll");
    if (!lib) return false;
    init = (cuInit_t)GetProcAddress(lib, "cuInit");
    deviceGet = (cuDeviceGet_t)GetProcAddress(lib, "cuDeviceGet");
    ctxRetain = (cuDevicePrimaryCtxRetain_t)GetProcAddress(lib, "cuDevicePrimaryCtxRetain");
    ctxRelease = (cuDevicePrimaryCtxRelease_t)GetProcAddress(lib, "cuDevicePrimaryCtxRelease");
    ctxSet = (cuCtxSetCurrent_t)GetProcAddress(lib, "cuCtxSetCurrent");
    memAlloc = (cuMemAlloc_t)GetProcAddress(lib, "cuMemAlloc_v2");
    memFree = (cuMemFree_t)GetProcAddress(lib, "cuMemFree_v2");
    hostAlloc = (cuMemHostAlloc_t)GetProcAddress(lib, "cuMemHostAlloc");
    hostFree = (cuMemFreeHost_t)GetProcAddress(lib, "cuMemFreeHost");
    h2d = (cuMemcpyHtoD_t)GetProcAddress(lib, "cuMemcpyHtoD_v2");
    d2h = (cuMemcpyDtoH_t)GetProcAddress(lib, "cuMemcpyDtoH_v2");
    h2dAsync = (cuMemcpyHtoDAsync_t)GetProcAddress(lib, "cuMemcpyHtoDAsync_v2");
    d2hAsync = (cuMemcpyDtoHAsync_t)GetProcAddress(lib, "cuMemcpyDtoHAsync_v2");
    streamCreate = (cuStreamCreate_t)GetProcAddress(lib, "cuStreamCreate");
    streamDestroy = (cuStreamDestroy_t)GetProcAddress(lib, "cuStreamDestroy");
    streamSync = (cuStreamSynchronize_t)GetProcAddress(lib, "cuStreamSynchronize");
    return init && deviceGet && ctxRetain && ctxRelease && ctxSet && memAlloc && memFree &&
           hostAlloc && hostFree && h2d && d2h && h2dAsync && d2hAsync &&
           streamCreate && streamDestroy && streamSync;
  }
};

class TrtLogger : public nvinfer1::ILogger {
public:
  void log(Severity severity, const char* msg) noexcept override {
    if (severity <= Severity::kWARNING) fprintf(stderr, "[trt] %s\n", msg);
  }
};

bool cpuHasAvx2F16c() {
  static const bool ok = [] {
    int regs[4] = {};
    __cpuid(regs, 1);
    const bool f16c = (regs[2] & (1 << 29)) != 0;
    const bool osxsave = (regs[2] & (1 << 27)) != 0;
    const bool avx = (regs[2] & (1 << 28)) != 0;
    __cpuidex(regs, 7, 0);
    const bool avx2 = (regs[1] & (1 << 5)) != 0;
    return f16c && osxsave && avx && avx2;
  }();
  return ok;
}

uint16_t f2h(float v) {
  uint32_t x;
  memcpy(&x, &v, sizeof(x));
  const uint32_t sign = (x >> 16) & 0x8000u;
  int32_t exp = (int32_t)((x >> 23) & 0xff) - 127 + 15;
  uint32_t mant = x & 0x7fffffu;
  if (exp <= 0) {
    if (exp < -10) return (uint16_t)sign;
    mant |= 0x800000u;
    const int shift = 14 - exp;
    return (uint16_t)(sign | (mant >> shift));
  }
  if (exp >= 31) return (uint16_t)(sign | 0x7c00u);
  return (uint16_t)(sign | ((uint32_t)exp << 10) | (mant >> 13));
}

float h2f(uint16_t h) {
  const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
  const uint32_t exp = (h >> 10) & 0x1fu;
  const uint32_t mant = h & 0x3ffu;
  uint32_t x;
  if (exp == 0) {
    if (mant == 0) {
      x = sign;
    } else {
      int e = -1;
      uint32_t m = mant;
      do { m <<= 1; e++; } while ((m & 0x400u) == 0);
      x = sign | (uint32_t)(127 - 15 - e) << 23 | ((m & 0x3ffu) << 13);
    }
  } else if (exp == 31) {
    x = sign | 0x7f800000u | (mant << 13);
  } else {
    x = sign | ((exp + 112u) << 23) | (mant << 13);
  }
  float v;
  memcpy(&v, &x, sizeof(v));
  return v;
}

size_t dimsVolume(const nvinfer1::Dims& d) {
  size_t v = 1;
  for (int i = 0; i < d.nbDims; i++) v *= (size_t)d.d[i];
  return v;
}

std::mutex g_buildMu;

}  // namespace

struct TensorRtSR::Impl {
  Driver drv;
  CudaD3D11Interop gpuInterop;
  TrtLogger logger;
  nvinfer1::IRuntime* runtime = nullptr;
  nvinfer1::ICudaEngine* engine = nullptr;
  nvinfer1::IExecutionContext* ctx = nullptr;
  CUcontext cuCtx = nullptr;
  CUdevice cuDev = 0;
  CUstream stream = nullptr;
  CUdeviceptr dIn = 0;
  CUdeviceptr dOut = 0;
  size_t dInBytes = 0;
  size_t dOutBytes = 0;
  int scale = 4;
  int maxInW = 0;
  int maxInH = 0;
  int minInW = 320;
  int minInH = 180;
  bool ready = false;
  SwsContext* sws = nullptr;
  int swsW = 0, swsH = 0, swsOutW = 0, swsOutH = 0;
  bool swsFast = false;
  std::vector<uint8_t> resized;
  uint16_t* hostIn = nullptr;
  uint16_t* hostOut = nullptr;
  size_t hostInBytes = 0;
  size_t hostOutBytes = 0;
  uint16_t lut[256]{};
  uint8_t halfToByte[65536]{};
  bool lutReady = false;

  void initLut() {
    if (lutReady) return;
    for (int i = 0; i < 256; i++) lut[i] = f2h((float)i / 255.0f);
    for (int i = 0; i < 65536; i++) {
      float v = h2f((uint16_t)i) * 255.f;
      v = std::min(255.f, std::max(0.f, v));
      halfToByte[i] = (uint8_t)v;
    }
    lutReady = true;
  }

  bool ensureDevice(size_t inBytes, size_t outBytes) {
    if (inBytes > dInBytes) {
      if (dIn) drv.memFree(dIn);
      dIn = 0;
      if (drv.memAlloc(&dIn, inBytes) != 0) return false;
      dInBytes = inBytes;
    }
    if (outBytes > dOutBytes) {
      if (dOut) drv.memFree(dOut);
      dOut = 0;
      if (drv.memAlloc(&dOut, outBytes) != 0) return false;
      dOutBytes = outBytes;
    }
    return true;
  }

  bool fitInput(int srcW, int srcH, int reqW, int& fitW, int& fitH) const {
    if (srcW <= 0 || srcH <= 0 || reqW <= 1 || maxInW <= 0 || maxInH <= 0) return false;
    fitW = std::min(reqW, maxInW);
    fitH = (int)(((int64_t)srcH * fitW / srcW) & ~1);
    if (fitH > maxInH) {
      fitH = maxInH;
      fitW = (int)(((int64_t)srcW * fitH / srcH) & ~1);
    }
    if (fitW > maxInW) {
      fitW = maxInW;
      fitH = (int)(((int64_t)srcH * fitW / srcW) & ~1);
    }
    if (fitW < minInW || fitH < minInH) {
      fitW = std::max(fitW, minInW);
      fitH = (int)(((int64_t)srcH * fitW / srcW) & ~1);
      if (fitH < minInH) {
        fitH = minInH;
        fitW = (int)(((int64_t)srcW * fitH / srcH) & ~1);
      }
    }
    return fitW >= 2 && fitH >= 2 && fitW <= maxInW && fitH <= maxInH;
  }

  // RGB24 -> FP16 NCHW H2D -> TensorRT。结束后 dOut 是可读的 FP16 NCHW。
  bool infer(const uint8_t* rgb, int w, int h, int reqW, int& outW, int& outH,
             bool fastResize = false) {
    if (drv.ctxSet(cuCtx) != 0) return false;
    const auto tStart = std::chrono::steady_clock::now();
    int fitW = 0, fitH = 0;
    if (!fitInput(w, h, reqW, fitW, fitH)) return false;
    const int inW = fitW;
    const int inH = fitH;

    const uint8_t* src = rgb;
    if (w != inW || h != inH) {
      if (swsW != w || swsH != h || swsOutW != inW || swsOutH != inH ||
          swsFast != fastResize) {
        clearSws();
        sws = sws_getContext(w, h, AV_PIX_FMT_RGB24, inW, inH, AV_PIX_FMT_RGB24,
                             fastResize ? SWS_FAST_BILINEAR : SWS_BILINEAR,
                             nullptr, nullptr, nullptr);
        if (!sws) return false;
        swsW = w; swsH = h; swsOutW = inW; swsOutH = inH;
        swsFast = fastResize;
      }
      resized.resize((size_t)inW * inH * 3);
      uint8_t* dst[1] = { resized.data() };
      const uint8_t* srcPlane[1] = { rgb };
      int srcStride[1] = { w * 3 };
      int dstStride[1] = { inW * 3 };
      sws_scale(sws, srcPlane, srcStride, 0, h, dst, dstStride);
      src = resized.data();
    }
    const auto tResize = std::chrono::steady_clock::now();

    const size_t inBytes = (size_t)3 * inW * inH * 2;
    if (inBytes > hostInBytes) return false;
    const size_t plane = (size_t)inW * inH;
    if (cpuHasAvx2F16c()) {
      trtRgb24ToHalfPlanarAvx2(src, inW, inH, hostIn, hostIn + plane,
                               hostIn + 2 * plane);
    } else {
      for (int y = 0; y < inH; y++) {
        const uint8_t* row = src + (size_t)y * inW * 3;
        uint16_t* r = hostIn + (size_t)y * inW;
        uint16_t* g = hostIn + plane + (size_t)y * inW;
        uint16_t* b = hostIn + 2 * plane + (size_t)y * inW;
        for (int x = 0; x < inW; x++) {
          r[x] = lut[row[x * 3 + 0]];
          g[x] = lut[row[x * 3 + 1]];
          b[x] = lut[row[x * 3 + 2]];
        }
      }
    }
    const auto tConvert = std::chrono::steady_clock::now();

    if (!ensureDevice(inBytes, dOutBytes)) return false;
    if (drv.h2dAsync(dIn, hostIn, inBytes, stream) != 0) return false;
    if (!ctx->setInputShape("input", nvinfer1::Dims4{1, 3, inH, inW})) return false;
    nvinfer1::Dims od = ctx->getTensorShape("output");
    if (od.nbDims != 4 || od.d[0] != 1 || od.d[1] != 3 ||
        od.d[2] <= 0 || od.d[3] <= 0 || od.d[2] % 4 != 0 || od.d[3] % 4 != 0) {
      return false;
    }
    outW = (int)od.d[3];
    outH = (int)od.d[2];
    const size_t outBytes = (size_t)3 * outW * outH * 2;
    if (!ensureDevice(inBytes, outBytes)) return false;
    if (!ctx->setTensorAddress("input", (void*)(uintptr_t)dIn) ||
        !ctx->setTensorAddress("output", (void*)(uintptr_t)dOut)) {
      return false;
    }
    const auto tInferStart = std::chrono::steady_clock::now();
    if (!ctx->enqueueV3((cudaStream_t)stream)) return false;
    if (drv.streamSync(stream) != 0) return false;
    if (const char* p = getenv("DOMINO_TRT_PROFILE_STAGES"); p && *p && *p != '0') {
      const auto tEnd = std::chrono::steady_clock::now();
      fprintf(stderr, "[trt-stage] resize=%.2f convert=%.2f h2d=%.2f infer=%.2f\n",
              std::chrono::duration<double, std::milli>(tResize - tStart).count(),
              std::chrono::duration<double, std::milli>(tConvert - tResize).count(),
              std::chrono::duration<double, std::milli>(tInferStart - tConvert).count(),
              std::chrono::duration<double, std::milli>(tEnd - tInferStart).count());
    }
    return true;
  }

  bool createEngine(const std::string& onnx, const std::string& cachePath) {
    std::lock_guard<std::mutex> lock(g_buildMu);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(cachePath).parent_path(), ec);

    if (std::filesystem::exists(cachePath, ec)) {
      std::ifstream f(cachePath, std::ios::binary);
      std::vector<char> data((std::istreambuf_iterator<char>(f)), {});
      if (!data.empty()) {
        runtime = nvinfer1::createInferRuntime(logger);
        if (runtime) engine = runtime->deserializeCudaEngine(data.data(), data.size());
      }
      if (!engine) {
        if (runtime) { delete runtime; runtime = nullptr; }
        std::filesystem::remove(cachePath, ec);
      }
    }
    if (engine) return true;

    struct BuildObjects {
      nvinfer1::IBuilder* builder = nullptr;
      nvinfer1::INetworkDefinition* network = nullptr;
      nvonnxparser::IParser* parser = nullptr;
      nvinfer1::IBuilderConfig* config = nullptr;
      ~BuildObjects() {
        delete parser;
        delete config;
        delete network;
        delete builder;
      }
    } objs;
    objs.builder = nvinfer1::createInferBuilder(logger);
    if (!objs.builder) return false;
    objs.network = objs.builder->createNetworkV2(0);
    if (!objs.network) return false;
    objs.parser = nvonnxparser::createParser(*objs.network, logger);
    if (!objs.parser ||
        !objs.parser->parseFromFile(onnx.c_str(), (int)nvinfer1::ILogger::Severity::kWARNING)) {
      return false;
    }
    objs.config = objs.builder->createBuilderConfig();
    if (!objs.config) return false;
    objs.config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, (size_t)4 << 30);

    auto profile = objs.builder->createOptimizationProfile();
    if (!profile) return false;
    const int optW = std::min(960, maxInW);
    const int optH = std::min(540, maxInH);
    const int minW = std::min(320, maxInW);
    const int minH = std::min(180, maxInH);
    if (!profile->setDimensions("input", nvinfer1::OptProfileSelector::kMIN,
                                nvinfer1::Dims4{1, 3, minH, minW}) ||
        !profile->setDimensions("input", nvinfer1::OptProfileSelector::kOPT,
                                nvinfer1::Dims4{1, 3, optH, optW}) ||
        !profile->setDimensions("input", nvinfer1::OptProfileSelector::kMAX,
                                nvinfer1::Dims4{1, 3, maxInH, maxInW}) ||
        objs.config->addOptimizationProfile(profile) < 0) {
      return false;
    }

    auto mem = objs.builder->buildSerializedNetwork(*objs.network, *objs.config);
    if (!mem) return false;
    {
      const std::string tmp = cachePath + ".tmp";
      std::ofstream f(tmp, std::ios::binary);
      f.write((const char*)mem->data(), (std::streamsize)mem->size());
      f.close();
      std::filesystem::rename(tmp, cachePath, ec);
      if (ec) std::filesystem::remove(tmp, ec);
    }
    runtime = nvinfer1::createInferRuntime(logger);
    if (runtime) engine = runtime->deserializeCudaEngine(mem->data(), mem->size());
    delete mem;
    return engine != nullptr;
  }

  void clearSws() {
    if (sws) { sws_freeContext(sws); sws = nullptr; }
    swsW = swsH = swsOutW = swsOutH = 0;
    swsFast = false;
  }

  void reset() {
    ready = false;
    if (cuCtx && drv.ctxSet) drv.ctxSet(cuCtx);
    gpuInterop.shutdown();
    clearSws();
    if (ctx) { delete ctx; ctx = nullptr; }
    if (engine) { delete engine; engine = nullptr; }
    if (runtime) { delete runtime; runtime = nullptr; }
    if (dIn) { drv.memFree(dIn); dIn = 0; dInBytes = 0; }
    if (dOut) { drv.memFree(dOut); dOut = 0; dOutBytes = 0; }
    if (stream) { drv.streamDestroy(stream); stream = nullptr; }
    if (hostIn) { drv.hostFree(hostIn); hostIn = nullptr; }
    if (hostOut) { drv.hostFree(hostOut); hostOut = nullptr; }
    hostInBytes = hostOutBytes = 0;
    if (cuCtx && drv.ctxRelease) drv.ctxRelease(cuDev);
    resized.clear();
    cuCtx = nullptr;
    cuDev = 0;
  }
};

TensorRtSR::TensorRtSR() : impl_(new Impl()) {}
TensorRtSR::~TensorRtSR() { clear(); }

bool TensorRtSR::ready() const { return impl_ && impl_->ready; }

bool TensorRtSR::init(const char* onnxPath, const char* cacheDir, const char* cacheKey,
                      int scale, int maxInW, int maxInH) {
  clear();
  if (!onnxPath || !cacheDir || !cacheKey || scale != 4 || maxInW < 64 || maxInH < 64) return false;
  if (!impl_->drv.load()) return false;
  if (impl_->drv.init(0) != 0) return false;
  CUdevice dev = 0;
  if (impl_->drv.deviceGet(&dev, 0) != 0) return false;
  if (impl_->drv.ctxRetain(&impl_->cuCtx, dev) != 0 || !impl_->cuCtx) return false;
  impl_->cuDev = dev;
  if (impl_->drv.ctxSet(impl_->cuCtx) != 0) return false;
  impl_->scale = scale;
  impl_->maxInW = maxInW;
  impl_->maxInH = maxInH;
  impl_->minInW = std::min(320, maxInW);
  impl_->minInH = std::min(180, maxInH);

  std::error_code ec;
  const auto onnxSize = std::filesystem::file_size(onnxPath, ec);
  const auto onnxTime = std::filesystem::last_write_time(onnxPath, ec).time_since_epoch().count();
  const std::string stem = std::string(cacheKey) + "-v1-" +
    std::to_string(maxInW) + "x" + std::to_string(maxInH) + "-" +
    std::to_string(onnxSize) + "-" + std::to_string(onnxTime) + ".engine";
  const std::string cachePath = (std::filesystem::path(cacheDir) / stem).string();
  if (!impl_->createEngine(onnxPath, cachePath)) {
    impl_->reset();
    return false;
  }
  if (impl_->engine->getTensorDataType("input") != nvinfer1::DataType::kHALF ||
      impl_->engine->getTensorDataType("output") != nvinfer1::DataType::kHALF) {
    fprintf(stderr, "trt: model must use FP16 input/output\n");
    impl_->reset();
    return false;
  }
  impl_->ctx = impl_->engine->createExecutionContext();
  if (!impl_->ctx) { impl_->reset(); return false; }
  if (impl_->drv.streamCreate(&impl_->stream, 0) != 0 || !impl_->stream) {
    impl_->reset(); return false;
  }
  const size_t maxInBytes = (size_t)3 * maxInW * maxInH * 2;
  const size_t maxOutBytes = (size_t)3 * maxInW * scale * maxInH * scale * 2;
  if (!impl_->ensureDevice(maxInBytes, maxOutBytes)) { impl_->reset(); return false; }
  if (impl_->drv.hostAlloc((void**)&impl_->hostIn, maxInBytes, 0) != 0 ||
      impl_->drv.hostAlloc((void**)&impl_->hostOut, maxOutBytes, 0) != 0) {
    impl_->reset();
    return false;
  }
  impl_->hostInBytes = maxInBytes;
  impl_->hostOutBytes = maxOutBytes;
  impl_->initLut();
  impl_->ready = true;

  // 触发目标 shape 的 JIT，避免第一帧卡顿；失败不影响后续正常路径。
  const int warmW = std::min(960, maxInW);
  const int warmH = std::min(540, maxInH);
  std::vector<uint8_t> dummy((size_t)warmW * warmH * 3, 0);
  std::vector<uint8_t> unused;
  impl_->drv.ctxSet(impl_->cuCtx);
  process(dummy.data(), warmW, warmH, warmW, unused);
  return true;
}

int TensorRtSR::process(const uint8_t* rgb, int w, int h, int inW,
                        std::vector<uint8_t>& outRgb) {
  if (!impl_ || !impl_->ready || !rgb || w <= 0 || h <= 0 || inW <= 1) return -1;
  Impl& m = *impl_;
  const auto profileT0 = std::chrono::steady_clock::now();
  int outW = 0, outH = 0;
  if (!m.infer(rgb, w, h, inW, outW, outH)) return -1;
  const auto tInfer = std::chrono::steady_clock::now();
  const size_t outBytes = (size_t)3 * outW * outH * 2;
  if (outBytes > m.hostOutBytes) return -1;
  if (m.drv.d2h(m.hostOut, m.dOut, outBytes) != 0) return -1;
  const auto tD2H = std::chrono::steady_clock::now();
  const bool stageProfile = [] {
    const char* p = getenv("DOMINO_TRT_PROFILE_STAGES");
    return p && *p && *p != '0';
  }();

  outRgb.resize((size_t)outW * outH * 3);
  const uint16_t* planes[3] = {
    m.hostOut,
    m.hostOut + (size_t)outW * outH,
    m.hostOut + (size_t)2 * outW * outH
  };
  auto convertRowsScalar = [&](int y0, int y1) {
    for (int y = y0; y < y1; y++) {
      const uint16_t* rp = planes[0] + (size_t)y * outW;
      const uint16_t* gp = planes[1] + (size_t)y * outW;
      const uint16_t* bp = planes[2] + (size_t)y * outW;
      uint8_t* dp = outRgb.data() + (size_t)y * outW * 3;
      for (int x = 0; x < outW; x++) {
        dp[x * 3 + 0] = m.halfToByte[rp[x]];
        dp[x * 3 + 1] = m.halfToByte[gp[x]];
        dp[x * 3 + 2] = m.halfToByte[bp[x]];
      }
    }
  };
  auto convertRowsAvx2 = [&](int y0, int y1) {
    trtConvertHalfToRgbAvx2(planes[0] + (size_t)y0 * outW,
                            planes[1] + (size_t)y0 * outW,
                            planes[2] + (size_t)y0 * outW,
                            outW, y1 - y0,
                            outRgb.data() + (size_t)y0 * outW * 3,
                            m.halfToByte);
  };
  std::function<void(int, int)> convertRows;
  if (cpuHasAvx2F16c()) convertRows = convertRowsAvx2;
  else convertRows = convertRowsScalar;
  int threads = (int)std::thread::hardware_concurrency();
  threads = (std::max)(1, (std::min)(12, threads));
  if ((size_t)outW * outH < 200000) threads = 1;
  if (threads <= 1) {
    convertRows(0, outH);
  } else {
    std::vector<std::thread> workers;
    for (int i = 0; i < threads; i++) {
      workers.emplace_back(convertRows, outH * i / threads, outH * (i + 1) / threads);
    }
    for (auto& t : workers) t.join();
  }
  if (const char* profile = getenv("DOMINO_TRT_PROFILE")) {
    if (*profile && *profile != '0') {
      const auto t1 = std::chrono::steady_clock::now();
      const int inH = m.scale > 0 ? outH / m.scale : 0;
      fprintf(stderr, "[trt] %dx%d -> %dx%d total=%.2fms",
              inW, inH, outW, outH,
              std::chrono::duration<double, std::milli>(t1 - profileT0).count());
      if (stageProfile) {
        fprintf(stderr, " infer=%.2fms d2h=%.2fms convert=%.2fms",
                std::chrono::duration<double, std::milli>(tInfer - profileT0).count(),
                std::chrono::duration<double, std::milli>(tD2H - tInfer).count(),
                std::chrono::duration<double, std::milli>(t1 - tD2H).count());
      }
      fprintf(stderr, "\n");
    }
  }
  return outW;
}

bool TensorRtSR::outputSize(int srcW, int srcH, int inW, int& outW, int& outH) const {
  if (!impl_ || !impl_->ready) return false;
  int fitW = 0, fitH = 0;
  if (!impl_->fitInput(srcW, srcH, inW, fitW, fitH)) return false;
  outW = fitW * impl_->scale;
  outH = fitH * impl_->scale;
  return true;
}

int TensorRtSR::processGpu(const uint8_t* rgb, int w, int h, int inW, int slot,
                           void* textures[3], IGpuFrameSink* lockSink) {
  if (!impl_ || !impl_->ready || !rgb || !textures || w <= 0 || h <= 0 || inW <= 1) {
    return -1;
  }
  Impl& m = *impl_;
  const auto t0 = std::chrono::steady_clock::now();
  int outW = 0, outH = 0;
  if (!m.infer(rgb, w, h, inW, outW, outH, true)) return -1;
  const auto tInfer = std::chrono::steady_clock::now();
  if (lockSink) lockSink->lockGpuCopy();
  const bool copied = m.gpuInterop.copyToTextures(
      (const void*)(uintptr_t)m.dOut, slot, outW, outH, textures);
  if (lockSink) lockSink->unlockGpuCopy();
  if (!copied) return -1;
  if (const char* profile = getenv("DOMINO_TRT_PROFILE")) {
    if (*profile && *profile != '0') {
      const auto t1 = std::chrono::steady_clock::now();
      fprintf(stderr, "[trt-gpu] %dx%d -> %dx%d infer=%.2fms copy=%.2fms total=%.2fms\n",
              inW, (int)((int64_t)h * inW / w), outW, outH,
              std::chrono::duration<double, std::milli>(tInfer - t0).count(),
              std::chrono::duration<double, std::milli>(t1 - tInfer).count(),
              std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
  }
  return outW;
}

void TensorRtSR::resetGpuInterop() {
  if (!impl_) return;
  if (impl_->cuCtx && impl_->drv.ctxSet) impl_->drv.ctxSet(impl_->cuCtx);
  impl_->gpuInterop.reset();
}

bool TensorRtSR::gpuInteropAvailable() const {
  return impl_ && impl_->gpuInterop.available();
}

void TensorRtSR::clear() {
  if (!impl_) return;
  impl_->reset();
}

#else

struct TensorRtSR::Impl {};
TensorRtSR::TensorRtSR() : impl_(new Impl()) {}
TensorRtSR::~TensorRtSR() = default;
bool TensorRtSR::ready() const { return false; }
bool TensorRtSR::init(const char*, const char*, const char*, int, int, int) { return false; }
int TensorRtSR::process(const uint8_t*, int, int, int, std::vector<uint8_t>&) { return -1; }
bool TensorRtSR::outputSize(int, int, int, int&, int&) const { return false; }
int TensorRtSR::processGpu(const uint8_t*, int, int, int, int, void*[3], IGpuFrameSink*) { return -1; }
void TensorRtSR::resetGpuInterop() {}
bool TensorRtSR::gpuInteropAvailable() const { return false; }
void TensorRtSR::clear() {}

#endif

