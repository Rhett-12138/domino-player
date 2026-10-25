#include "gpu_interop.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
// 非 Windows 仍要能编译 domino_core；实现是不可用的 no-op。
#define __stdcall
using HMODULE = void*;
#endif

namespace {

using cudaError_t = int;
using cudaStream_t = void*;
using cudaGraphicsResource_t = void*;
using cudaArray_t = void*;

constexpr int kSuccess = 0;
constexpr int kMemcpyDeviceToDevice = 3;

using cudaSetDevice_t = cudaError_t(__stdcall*)(int);
using cudaStreamCreate_t = cudaError_t(__stdcall*)(cudaStream_t*);
using cudaStreamDestroy_t = cudaError_t(__stdcall*)(cudaStream_t);
using cudaStreamSynchronize_t = cudaError_t(__stdcall*)(cudaStream_t);
using cudaGraphicsD3D11RegisterResource_t =
    cudaError_t(__stdcall*)(cudaGraphicsResource_t*, void*, unsigned);
using cudaGraphicsUnregisterResource_t = cudaError_t(__stdcall*)(cudaGraphicsResource_t);
using cudaGraphicsMapResources_t =
    cudaError_t(__stdcall*)(int, cudaGraphicsResource_t*, cudaStream_t);
using cudaGraphicsUnmapResources_t =
    cudaError_t(__stdcall*)(int, cudaGraphicsResource_t*, cudaStream_t);
using cudaGraphicsSubResourceGetMappedArray_t =
    cudaError_t(__stdcall*)(cudaArray_t*, cudaGraphicsResource_t, unsigned, unsigned);
using cudaMemcpy2DToArrayAsync_t =
    cudaError_t(__stdcall*)(cudaArray_t, size_t, size_t, const void*, size_t,
                            size_t, size_t, int, cudaStream_t);
using cudaGetErrorString_t = const char*(__stdcall*)(cudaError_t);

#ifdef _WIN32
bool hasSuffix(const std::string& s, const char* suffix) {
  const size_t n = strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

HMODULE loadCudart() {
  if (const char* env = getenv("DOMINO_CUDART_PATH")) {
    if (*env) {
      HMODULE h = LoadLibraryA(env);
      if (h) return h;
    }
  }
  // 常规安装/打包：exe 目录会参与 DLL 搜索。
  for (const char* name : {"cudart64_12.dll", "cudart64_11.dll"}) {
    HMODULE h = LoadLibraryA(name);
    if (h) return h;
  }
  // 开发环境：允许从仓库 libs 直接加载（libs 不进 git）。
  char exe[MAX_PATH] = {};
  if (GetModuleFileNameA(nullptr, exe, MAX_PATH) > 0) {
    std::string dir(exe);
    const size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) dir.resize(slash);
    for (const char* rel : {"\\cudart64_12.dll", "\\libs\\cuda-runtime\\bin\\cudart64_12.dll",
                            "\\..\\..\\..\\libs\\cuda-runtime\\bin\\cudart64_12.dll"}) {
      const std::string path = dir + rel;
      if (hasSuffix(path, ".dll")) {
        HMODULE h = LoadLibraryA(path.c_str());
        if (h) return h;
      }
    }
  }
  return nullptr;
}
#endif

}  // namespace

struct CudaD3D11Interop::Impl {
  struct Entry {
    void* tex[3] = {};
    int w = 0, h = 0;
    cudaGraphicsResource_t res[3] = {};
    cudaArray_t arrays[3] = {};
  };

  HMODULE lib = nullptr;
  cudaSetDevice_t setDevice = nullptr;
  cudaStreamCreate_t streamCreate = nullptr;
  cudaStreamDestroy_t streamDestroy = nullptr;
  cudaStreamSynchronize_t streamSync = nullptr;
  cudaGraphicsD3D11RegisterResource_t registerResource = nullptr;
  cudaGraphicsUnregisterResource_t unregisterResource = nullptr;
  cudaGraphicsMapResources_t mapResources = nullptr;
  cudaGraphicsUnmapResources_t unmapResources = nullptr;
  cudaGraphicsSubResourceGetMappedArray_t getMappedArray = nullptr;
  cudaMemcpy2DToArrayAsync_t memcpy2DToArrayAsync = nullptr;
  cudaGetErrorString_t errorString = nullptr;

  cudaStream_t stream = nullptr;
  Entry entries[2];   // 与 GpuFrameOutput 的两个输入 slot 一一对应，尺寸变化时原地重建

  ~Impl() { shutdown(); }

  void shutdown() {
    reset();
    if (stream && streamDestroy) {
      streamDestroy(stream);
      stream = nullptr;
    }
  }

  void unregisterEntry(Entry& e) {
    if (unregisterResource) {
      for (auto& r : e.res) {
        if (r) unregisterResource(r);
        r = nullptr;
      }
    }
    for (auto& a : e.arrays) a = nullptr;
    for (auto& t : e.tex) t = nullptr;
    e.w = e.h = 0;
  }

  void reset() {
    for (auto& e : entries) unregisterEntry(e);
  }

  bool load() {
    if (lib && stream) return true;
#ifdef _WIN32
    lib = loadCudart();
    if (!lib) return false;
    auto get = [&](const char* name) {
      return (void*)GetProcAddress(lib, name);
    };
    setDevice = (cudaSetDevice_t)get("cudaSetDevice");
    streamCreate = (cudaStreamCreate_t)get("cudaStreamCreate");
    streamDestroy = (cudaStreamDestroy_t)get("cudaStreamDestroy");
    streamSync = (cudaStreamSynchronize_t)get("cudaStreamSynchronize");
    registerResource = (cudaGraphicsD3D11RegisterResource_t)get(
        "cudaGraphicsD3D11RegisterResource");
    unregisterResource = (cudaGraphicsUnregisterResource_t)get(
        "cudaGraphicsUnregisterResource");
    mapResources = (cudaGraphicsMapResources_t)get("cudaGraphicsMapResources");
    unmapResources = (cudaGraphicsUnmapResources_t)get("cudaGraphicsUnmapResources");
    getMappedArray = (cudaGraphicsSubResourceGetMappedArray_t)get(
        "cudaGraphicsSubResourceGetMappedArray");
    memcpy2DToArrayAsync = (cudaMemcpy2DToArrayAsync_t)get(
        "cudaMemcpy2DToArrayAsync");
    errorString = (cudaGetErrorString_t)get("cudaGetErrorString");
    if (!setDevice || !streamCreate || !streamDestroy || !streamSync ||
        !registerResource || !unregisterResource || !mapResources ||
        !unmapResources || !getMappedArray || !memcpy2DToArrayAsync) {
      reset();
      return false;
    }
    if (setDevice(0) != kSuccess) return false;
    if (!stream && (streamCreate(&stream) != kSuccess || !stream)) return false;
    return true;
#else
    return false;
#endif
  }

  // 固定按 slot 缓存资源；分辨率/档位变化时只重建该 slot，避免 entries 无界增长
  // 和每次切档约 100MB 的显存泄漏。注册/注销都在 worker 线程、CUDA 主上下文下。
  Entry* findOrRegister(int slot, void* textures[3], int w, int h) {
    if (slot < 0 || slot >= 2) return nullptr;
    if (!load()) return nullptr;
    Entry& e = entries[slot];
    if (e.tex[0] == textures[0] && e.tex[1] == textures[1] &&
        e.tex[2] == textures[2] && e.w == w && e.h == h) {
      return &e;
    }
    unregisterEntry(e);
    e.w = w;
    e.h = h;
    for (int i = 0; i < 3; i++) {
      cudaGraphicsResource_t r = nullptr;
      const cudaError_t err = registerResource(&r, textures[i], 0);
      if (err != kSuccess || !r) {
        if (errorString) {
          fprintf(stderr, "cuda interop: register texture %d failed: %s\n", i,
                  errorString(err));
        }
        unregisterEntry(e);
        return nullptr;
      }
      e.res[i] = r;
      e.tex[i] = textures[i];
    }
    return &e;
  }

  bool copy(const void* src, int slot, int w, int h, void* textures[3]) {
    if (!src || w <= 0 || h <= 0 || !textures) return false;
    Entry* e = findOrRegister(slot, textures, w, h);
    if (!e) return false;
    if (mapResources(3, e->res, stream) != kSuccess) return false;
    bool ok = true;
    for (int i = 0; i < 3; i++) {
      e->arrays[i] = nullptr;
      const cudaError_t err = getMappedArray(&e->arrays[i], e->res[i], 0, 0);
      if (err != kSuccess || !e->arrays[i]) {
        if (errorString) {
          fprintf(stderr, "cuda interop: map array %d failed: %s\n", i, errorString(err));
        }
        ok = false;
        break;
      }
    }
    if (ok) {
      const size_t planeBytes = (size_t)w * h * 2;
      for (int i = 0; i < 3; i++) {
        const cudaError_t err = memcpy2DToArrayAsync(
            e->arrays[i], 0, 0,
            (const char*)src + planeBytes * (size_t)i,
            (size_t)w * 2, (size_t)w * 2, (size_t)h,
            kMemcpyDeviceToDevice, stream);
        if (err != kSuccess) {
          if (errorString) {
            fprintf(stderr, "cuda interop: copy plane %d failed: %s\n", i, errorString(err));
          }
          ok = false;
          break;
        }
      }
    }
    // 先等 copy 完成再 unmap，保证 D3D shader 读到完整纹理。
    if (ok && streamSync && streamSync(stream) != kSuccess) ok = false;
    if (unmapResources(3, e->res, stream) != kSuccess) ok = false;
    // unmap 也可能把跨 API 同步排入队列；再同步一次，避免 D3D 采样时资源仍被 CUDA 占用。
    if (streamSync && streamSync(stream) != kSuccess) ok = false;
    return ok;
  }
};

CudaD3D11Interop::CudaD3D11Interop() : impl_(new Impl()) {}
CudaD3D11Interop::~CudaD3D11Interop() = default;

bool CudaD3D11Interop::available() {
  return impl_ && impl_->load();
}

bool CudaD3D11Interop::copyToTextures(const void* srcDevice, int slot, int w, int h,
                                      void* textures[3]) {
  return impl_ && impl_->copy(srcDevice, slot, w, h, textures);
}

void CudaD3D11Interop::reset() {
  if (impl_) impl_->reset();
}

void CudaD3D11Interop::shutdown() {
  if (impl_) impl_->shutdown();
}
