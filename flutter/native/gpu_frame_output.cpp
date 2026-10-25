#include "gpu_frame_output.h"

#include "frame_output.h"
#include "log.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

extern "C" {
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kSmallW = 80;
constexpr int kSmallH = 45;
constexpr int kOutCount = 3;

// Mirrors beauty_math.h; keep both in sync.
const char* kBeautyHlsl = R"HLSL(
cbuffer Params : register(b0)
{
    float4 p0;   // texel.x, texel.y, smooth, white
    float4 p1;   // filterIdx, filterAmt, radius, unused
    float4 w0;   // taps -4..-1
    float4 w1;   // taps 0..3
    float4 w2;   // tap 4
};

Texture2D tex0 : register(t0);
Texture2D tex1 : register(t1);
SamplerState samp : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut vs_main(uint id : SV_VertexID)
{
    float2 p = float2((float)((id << 1) & 2), (float)(id & 2));
    VSOut o;
    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    o.uv = p;
    return o;
}

float skinProb(float r, float g, float b)
{
    float rg = r - g;
    float rb = r - b;
    if (rg < 0.02 || rb < rg * 0.6) return 0.0;
    float mx = max(r, max(g, b));
    float mn = min(r, min(g, b));
    float d = mx - mn;
    float amp = 1.0 - abs(d - 0.18) * 3.5;
    float lum = 1.0 - abs(r - 0.60) * 1.8;
    return saturate(amp) * saturate(lum);
}

float blurBlend(float c, float bl, float skin, float s)
{
    float k = min(1.0, skin * s * 1.2);
    return lerp(c, bl, k);
}

float whiteBlend(float c, float skin, float s)
{
    return c + skin * s * (1.0 - c) * 0.8;
}

float3 applyFilter(float3 rgb, float idx, float amt)
{
    float r = rgb.r, g = rgb.g, b = rgb.b;
    float l = 0.299 * r + 0.587 * g + 0.114 * b;
    float3 f = rgb;
    if (idx < 0.5) {
        f = rgb;
    } else if (idx < 1.5) {
        f = float3(l, l, l);
    } else if (idx < 2.5) {
        float fr = r * 1.06 + 0.045;
        float fg = g;
        float fb = b * 1.12 + 0.045;
        fr = l + (fr - l) * 0.88;
        fg = l + (fg - l) * 0.88;
        fb = l + (fb - l) * 0.88;
        f = float3(fr, fg, fb);
    } else if (idx < 3.5) {
        f = float3(r * 1.12, g * 1.04, b * 0.90);
    } else if (idx < 4.5) {
        f = float3(r * 0.94, g * 1.02, b * 1.12);
    } else if (idx < 5.5) {
        float fr = r + (l - r) * 0.30;
        float fg = g + (l - g) * 0.30;
        float fb = b + (l - b) * 0.30;
        f = float3(fr * 1.08, fg, fb * 0.90);
    } else {
        f = float3(r * 1.10 + 0.05, g * 0.98, b * 1.02 + 0.05);
    }
    return saturate(lerp(rgb, f, amt));
}

float4 ps_blur_h(VSOut i) : SV_Target
{
    float2 t = p0.xy;
    float4 c = w0.x * tex0.Sample(samp, i.uv + float2(-4.0 * t.x, 0.0))
             + w0.y * tex0.Sample(samp, i.uv + float2(-3.0 * t.x, 0.0))
             + w0.z * tex0.Sample(samp, i.uv + float2(-2.0 * t.x, 0.0))
             + w0.w * tex0.Sample(samp, i.uv + float2(-1.0 * t.x, 0.0))
             + w1.x * tex0.Sample(samp, i.uv)
             + w1.y * tex0.Sample(samp, i.uv + float2( 1.0 * t.x, 0.0))
             + w1.z * tex0.Sample(samp, i.uv + float2( 2.0 * t.x, 0.0))
             + w1.w * tex0.Sample(samp, i.uv + float2( 3.0 * t.x, 0.0))
             + w2.x * tex0.Sample(samp, i.uv + float2( 4.0 * t.x, 0.0));
    return c;
}

Texture2D<float> texR : register(t0);
Texture2D<float> texG : register(t1);
Texture2D<float> texB : register(t2);

// 零拷贝路径：TensorRT 的 FP16 NCHW 三个平面由 CUDA 直写纹理，
// 这里只做 GPU 采样/最终 BGRA 上屏（美颜路径仍走 CPU/ps_final）。
float4 ps_gpu(VSOut i) : SV_Target
{
    float r = texR.Sample(samp, i.uv);
    float g = texG.Sample(samp, i.uv);
    float b = texB.Sample(samp, i.uv);
    return float4(r, g, b, 1.0);
}

float4 ps_final(VSOut i) : SV_Target
{
    float4 c = tex0.Sample(samp, i.uv);
    float r = c.r, g = c.g, b = c.b;
    float skin = skinProb(r, g, b);
    if (p0.z > 0.001) {
        float4 bl = tex1.Sample(samp, i.uv);
        r = blurBlend(r, bl.r, skin, p0.z);
        g = blurBlend(g, bl.g, skin, p0.z);
        b = blurBlend(b, bl.b, skin, p0.z);
    }
    if (p0.w > 0.001) {
        r = whiteBlend(r, skin, p0.w);
        g = whiteBlend(g, skin, p0.w);
        b = whiteBlend(b, skin, p0.w);
    }
    if (p1.x > 0.001 && p1.y > 0.001) {
        float3 f = applyFilter(float3(r, g, b), p1.x, p1.y);
        r = f.r; g = f.g; b = f.b;
    }
    return float4(saturate(float3(r, g, b)), 1.0);
}
)HLSL";

struct alignas(16) BeautyCB {
  float texel[2];
  float smooth;
  float white;
  float filterIdx;
  float filterAmt;
  float radius;
  float pad;
  float w[12];
};

struct OutTex {
  ComPtr<ID3D11Texture2D> tex;
  ComPtr<ID3D11RenderTargetView> rtv;
  HANDLE handle = nullptr;
  int texW = 0, texH = 0;
  bool busy = false;    // pump thread is rendering
  bool inUse = false;   // Flutter still holds it (release callback pending)
  FlutterDesktopGpuSurfaceDescriptor desc = {};
};

// 零拷贝输入：3 个 R16_FLOAT 平面纹理；CUDA 直写，D3D shader 采样。
struct GpuInSlot {
  ComPtr<ID3D11Texture2D> tex[3];
  ComPtr<ID3D11ShaderResourceView> srv[3];
  int w = 0, h = 0;
  bool busy = false;
};

}  // namespace

struct GpuFrameOutput::Impl {
  struct ReleaseCtx {
    std::shared_ptr<Impl> impl;
    int idx;
    uint64_t gen;
    ID3D11Texture2D* tex;
  };
  static void release(void* p);

  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> ctx;
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> psBlur;
  ComPtr<ID3D11PixelShader> psFinal;
  ComPtr<ID3D11PixelShader> psGpu;
  ComPtr<ID3D11Buffer> cb;
  ComPtr<ID3D11SamplerState> sampler;
  ComPtr<ID3D11Query> syncQuery;

  ComPtr<ID3D11Texture2D> input;
  ComPtr<ID3D11ShaderResourceView> inputSrv;
  SwsContext* swsRgba = nullptr;
  int swsW = 0, swsH = 0;
  std::vector<uint8_t> rgbaBuf;
  ComPtr<ID3D11Texture2D> temp;
  ComPtr<ID3D11RenderTargetView> tempRtv;
  ComPtr<ID3D11ShaderResourceView> tempSrv;
  std::deque<OutTex> outs;   // 已发布的 descriptor 不随 resize 失效
  std::vector<GpuInSlot> gpuIns;

  int w = 0, h = 0;
  int front = -1;
  uint64_t gen = 0;
  bool ok = false;
  std::string adapterName;
  int adapterVendor = 0;
  int adapterDevice = 0;
  long long adapterMemory = 0;
  int featureLevelValue = 0;

  EngineBeautyParams bp;
  std::atomic<bool> newFrame{ false };
  std::mutex mu;
  std::mutex copyMu;   // cudart 写入 ↔ D3D 采样
  std::atomic<bool> fatal{ false };

  std::vector<uint8_t> smallRgb;
  EngineFrameOutput previewGen;

  bool init();
  bool ensureSize(int width, int height);
  bool ensureGpuSlots(int width, int height);
  bool render(const uint8_t* rgb, int width, int height, int outIdx);
  bool renderGpu(int slot, int width, int height, int outIdx);
  bool compile(const char* entry, const char* target, ComPtr<ID3DBlob>& blob);
  void releaseInputTemp();
};

bool GpuFrameOutput::Impl::compile(const char* entry, const char* target, ComPtr<ID3DBlob>& blob) {
  ComPtr<ID3DBlob> err;
  UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
  HRESULT hr = D3DCompile(kBeautyHlsl, strlen(kBeautyHlsl), "beauty.hlsl", nullptr, nullptr,
                          entry, target, flags, 0, &blob, &err);
  if (FAILED(hr)) {
    logx::error("gpu beauty: compile %s failed: %s", entry,
                err ? (const char*)err->GetBufferPointer() : "?");
    return false;
  }
  return true;
}

bool GpuFrameOutput::Impl::init() {
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
  D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
  // zero-copy 需要 D3D11 设备与 TensorRT 在同一块 NVIDIA GPU 上；默认 adapter
  // 在双显卡机器可能是核显。默认强制 NVIDIA 才能启用 cudart 互操作；混合显卡
  // 若 Flutter 无法打开 legacy shared handle，可用 DOMINO_D3D_ADAPTER=default
  // 退回默认 adapter（零拷贝自动降级为 CPU，但显示链路保持原生）。
  ComPtr<IDXGIAdapter1> nvAdapter;
  ComPtr<IDXGIFactory1> factory;
  const char* adapterMode = getenv("DOMINO_D3D_ADAPTER");
  const bool preferDefault = adapterMode && strcmp(adapterMode, "default") == 0;
  if (!preferDefault && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    for (UINT i = 0;; i++) {
      ComPtr<IDXGIAdapter1> ad;
      if (factory->EnumAdapters1(i, &ad) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_ADAPTER_DESC1 desc = {};
      if (SUCCEEDED(ad->GetDesc1(&desc)) && desc.VendorId == 0x10DE) {
        nvAdapter = ad;
        break;
      }
    }
  }
  HRESULT hr = D3D11CreateDevice(
      nvAdapter.Get(), nvAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
      nullptr, flags, levels, 3, D3D11_SDK_VERSION, &device, &got, &ctx);
  if (FAILED(hr) || !device || !ctx) {
    logx::warn("gpu beauty: D3D11CreateDevice failed 0x%08x", (unsigned)hr);
    return false;
  }
  featureLevelValue = (int)got;

  ComPtr<IDXGIDevice> dxgiDevice;
  ComPtr<IDXGIAdapter> dxgiAdapter;
  ComPtr<IDXGIAdapter1> adapter1;
  if (SUCCEEDED(device.As(&dxgiDevice)) &&
      SUCCEEDED(dxgiDevice->GetAdapter(&dxgiAdapter)) &&
      SUCCEEDED(dxgiAdapter.As(&adapter1))) {
    DXGI_ADAPTER_DESC1 desc = {};
    if (SUCCEEDED(adapter1->GetDesc1(&desc))) {
      adapterVendor = (int)desc.VendorId;
      adapterDevice = (int)desc.DeviceId;
      adapterMemory = (long long)desc.DedicatedVideoMemory;
      const int n = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
      if (n > 1) {
        std::string name((size_t)n - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name.data(), n, nullptr, nullptr);
        adapterName = std::move(name);
      }
    }
  }

  ComPtr<ID3DBlob> vsBlob, psBlurBlob, psFinalBlob, psGpuBlob;
  if (!compile("vs_main", "vs_5_0", vsBlob) ||
      !compile("ps_blur_h", "ps_5_0", psBlurBlob) ||
      !compile("ps_final", "ps_5_0", psFinalBlob) ||
      !compile("ps_gpu", "ps_5_0", psGpuBlob)) {
    return false;
  }
  if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs)) ||
      FAILED(device->CreatePixelShader(psBlurBlob->GetBufferPointer(), psBlurBlob->GetBufferSize(), nullptr, &psBlur)) ||
      FAILED(device->CreatePixelShader(psFinalBlob->GetBufferPointer(), psFinalBlob->GetBufferSize(), nullptr, &psFinal)) ||
      FAILED(device->CreatePixelShader(psGpuBlob->GetBufferPointer(), psGpuBlob->GetBufferSize(), nullptr, &psGpu))) {
    logx::error("gpu beauty: CreateShader failed");
    return false;
  }

  D3D11_BUFFER_DESC cbd = {};
  cbd.ByteWidth = sizeof(BeautyCB);
  cbd.Usage = D3D11_USAGE_DYNAMIC;
  cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(device->CreateBuffer(&cbd, nullptr, &cb))) return false;

  D3D11_SAMPLER_DESC sd = {};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sd.MinLOD = 0;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(device->CreateSamplerState(&sd, &sampler))) return false;

  D3D11_QUERY_DESC qd = {};
  qd.Query = D3D11_QUERY_EVENT;
  if (FAILED(device->CreateQuery(&qd, &syncQuery))) return false;

  smallRgb.resize((size_t)kSmallW * kSmallH * 3);
  ok = true;
  return true;
}

void GpuFrameOutput::Impl::releaseInputTemp() {
  tempSrv.Reset();
  tempRtv.Reset();
  temp.Reset();
  inputSrv.Reset();
  input.Reset();
  if (swsRgba) { sws_freeContext(swsRgba); swsRgba = nullptr; }
  swsW = swsH = 0;
  rgbaBuf.clear();
}

bool GpuFrameOutput::Impl::ensureSize(int width, int height) {
  if (width <= 0 || height <= 0) return false;
  std::lock_guard<std::mutex> lk(mu);
  if (input && temp && w == width && h == height) {
    for (auto& o : outs)
      if (o.tex && o.texW == width && o.texH == height) return true;
  }

  // 旧尺寸的 OutTex/descriptor 若仍被 Flutter 持有，不能清掉（否则 descriptor 悬垂）
  bool anyUse = false;
  for (auto& o : outs) if (o.inUse || o.busy) { anyUse = true; break; }
  if (!anyUse) outs.clear();

  const int oldW = w, oldH = h;
  w = width;
  h = height;
  gen++;
  releaseInputTemp();

  D3D11_TEXTURE2D_DESC d = {};
  d.Width = (UINT)width;
  d.Height = (UINT)height;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.SampleDesc.Count = 1;
  d.SampleDesc.Quality = 0;

  // RGB24 -> RGBA8 upload (D3D11 has no RGB24 format).
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.Usage = D3D11_USAGE_DYNAMIC;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  d.MiscFlags = 0;
  HRESULT hr = device->CreateTexture2D(&d, nullptr, &input);
  if (FAILED(hr)) { logx::error("gpu beauty: input texture 0x%08x", (unsigned)hr); w = oldW; h = oldH; return false; }
  hr = device->CreateShaderResourceView(input.Get(), nullptr, &inputSrv);
  if (FAILED(hr)) { logx::error("gpu beauty: input srv 0x%08x", (unsigned)hr); releaseInputTemp(); w = oldW; h = oldH; return false; }

  // Horizontal blur intermediate texture
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  d.CPUAccessFlags = 0;
  hr = device->CreateTexture2D(&d, nullptr, &temp);
  if (FAILED(hr)) { logx::error("gpu beauty: temp texture 0x%08x", (unsigned)hr); releaseInputTemp(); w = oldW; h = oldH; return false; }
  hr = device->CreateRenderTargetView(temp.Get(), nullptr, &tempRtv);
  if (FAILED(hr)) { logx::error("gpu beauty: temp rtv 0x%08x", (unsigned)hr); releaseInputTemp(); w = oldW; h = oldH; return false; }
  hr = device->CreateShaderResourceView(temp.Get(), nullptr, &tempSrv);
  if (FAILED(hr)) { logx::error("gpu beauty: temp srv 0x%08x", (unsigned)hr); releaseInputTemp(); w = oldW; h = oldH; return false; }

  // BGRA shared texture exposed to Flutter GPU surface. 先建成功再追加，
  // 失败不会留下“半成品”导致后续 ensureSize 误判。
  d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
  std::vector<OutTex> fresh;
  fresh.reserve(kOutCount);
  for (int i = 0; i < kOutCount; i++) {
    OutTex o;
    o.texW = width;
    o.texH = height;
    hr = device->CreateTexture2D(&d, nullptr, &o.tex);
    if (FAILED(hr)) { logx::error("gpu beauty: out texture 0x%08x", (unsigned)hr); releaseInputTemp(); w = oldW; h = oldH; return false; }
    hr = device->CreateRenderTargetView(o.tex.Get(), nullptr, &o.rtv);
    if (FAILED(hr)) { logx::error("gpu beauty: out rtv 0x%08x", (unsigned)hr); releaseInputTemp(); w = oldW; h = oldH; return false; }
    ComPtr<IDXGIResource> res;
    hr = o.tex.As(&res);
    if (FAILED(hr)) { logx::error("gpu beauty: out QI 0x%08x", (unsigned)hr); releaseInputTemp(); w = oldW; h = oldH; return false; }
    hr = res->GetSharedHandle(&o.handle);
    if (FAILED(hr) || !o.handle) {
      logx::error("gpu beauty: GetSharedHandle failed 0x%08x", (unsigned)hr);
      releaseInputTemp();
      w = oldW; h = oldH;
      return false;
    }
    o.busy = false;
    o.inUse = false;
    fresh.push_back(std::move(o));
  }
  for (auto& o : fresh) outs.push_back(std::move(o));
  front = -1;
  return true;
}

bool GpuFrameOutput::Impl::ensureGpuSlots(int width, int height) {
  if (width <= 0 || height <= 0) return false;
  std::lock_guard<std::mutex> lk(mu);
  if (gpuIns.size() == 2 && gpuIns[0].w == width && gpuIns[0].h == height) {
    return true;
  }
  for (auto& s : gpuIns) {
    if (s.busy) return false;
  }
  gpuIns.clear();
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = (UINT)width;
  d.Height = (UINT)height;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = DXGI_FORMAT_R16_FLOAT;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  // D3D11_RESOURCE_MISC_SHARED 是 cudart 跨 API 写纹理的稳定条件；去掉后
  // 多 slot 并发在 nvwgf2umx 里会偶发 fail-fast（c0000409）。
  d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
  gpuIns.resize(2);
  for (auto& s : gpuIns) {
    s.w = width;
    s.h = height;
    s.busy = false;
    for (int i = 0; i < 3; i++) {
      if (FAILED(device->CreateTexture2D(&d, nullptr, &s.tex[i])) ||
          FAILED(device->CreateShaderResourceView(s.tex[i].Get(), nullptr, &s.srv[i]))) {
        logx::error("gpu zero-copy: create R16 texture failed");
        gpuIns.clear();
        return false;
      }
    }
  }
  return true;
}

bool GpuFrameOutput::Impl::render(const uint8_t* rgb, int width, int height, int outIdx) {
  const auto tStart = std::chrono::steady_clock::now();
  D3D11_MAPPED_SUBRESOURCE map = {};
  if (FAILED(ctx->Map(input.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return false;
  if (!swsRgba || swsW != width || swsH != height) {
    if (swsRgba) sws_freeContext(swsRgba);
    swsRgba = sws_getContext(width, height, AV_PIX_FMT_RGB24,
                             width, height, AV_PIX_FMT_RGBA,
                             SWS_POINT, nullptr, nullptr, nullptr);
    swsW = width;
    swsH = height;
    rgbaBuf.resize((size_t)width * height * 4);
  }
  if (swsRgba) {
    const uint8_t* srcPlane[1] = { rgb };
    int srcStride[1] = { width * 3 };
    uint8_t* dstPlane[1] = { rgbaBuf.data() };
    int dstStride[1] = { width * 4 };
    sws_scale(swsRgba, srcPlane, srcStride, 0, height, dstPlane, dstStride);
    for (int y = 0; y < height; y++) {
      memcpy((uint8_t*)map.pData + (size_t)y * map.RowPitch,
             rgbaBuf.data() + (size_t)y * width * 4, (size_t)width * 4);
    }
  } else {
    for (int y = 0; y < height; y++) {
      const uint8_t* s = rgb + (size_t)y * width * 3;
      uint8_t* d = (uint8_t*)map.pData + (size_t)y * map.RowPitch;
      for (int x = 0; x < width; x++) {
        d[0] = s[0];
        d[1] = s[1];
        d[2] = s[2];
        d[3] = 255;
        s += 3;
        d += 4;
      }
    }
  }
  ctx->Unmap(input.Get(), 0);
  const auto tUpload = std::chrono::steady_clock::now();

  int radius = bp.smooth > 0.001f ? (int)std::lround(bp.smooth * 4.0f) : 0;
  radius = std::max(0, std::min(4, radius));
  const float sigma = 0.6f + (float)radius * 0.8f;
  float weights[9] = {};
  float sum = 0.f;
  for (int i = -4; i <= 4; i++) {
    if (radius > 0 && std::abs(i) <= radius) {
      float v = std::exp(-(float)(i * i) / (2.f * sigma * sigma));
      weights[i + 4] = v;
      sum += v;
    }
  }
  if (sum > 0.f) for (float& v : weights) v /= sum;

  BeautyCB cbData = {};
  cbData.texel[0] = 1.f / (float)width;
  cbData.texel[1] = 1.f / (float)height;
  cbData.smooth = bp.smooth;
  cbData.white = bp.white;
  cbData.filterIdx = (float)bp.filterIdx;
  cbData.filterAmt = bp.filterAmt;
  cbData.radius = (float)radius;
  memcpy(cbData.w, weights, sizeof(weights));

  D3D11_MAPPED_SUBRESOURCE cbm = {};
  if (FAILED(ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &cbm))) return false;
  memcpy(cbm.pData, &cbData, sizeof(cbData));
  ctx->Unmap(cb.Get(), 0);

  D3D11_VIEWPORT vp = { 0.f, 0.f, (float)width, (float)height, 0.f, 1.f };
  ctx->RSSetViewports(1, &vp);
  ctx->IASetInputLayout(nullptr);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(vs.Get(), nullptr, 0);
  ctx->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
  ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());

  ID3D11ShaderResourceView* nullSrvs[2] = { nullptr, nullptr };
  ctx->PSSetShaderResources(0, 2, nullSrvs);

  if (radius > 0) {
    ID3D11RenderTargetView* rt = tempRtv.Get();
    ctx->OMSetRenderTargets(1, &rt, nullptr);
    ctx->PSSetShader(psBlur.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = { inputSrv.Get(), nullptr };
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->Draw(3, 0);
  }

  ID3D11RenderTargetView* rt = outs[(size_t)outIdx].rtv.Get();
  ctx->OMSetRenderTargets(1, &rt, nullptr);
  ctx->PSSetShader(psFinal.Get(), nullptr, 0);
  ID3D11ShaderResourceView* srvs[2] = { inputSrv.Get(), radius > 0 ? tempSrv.Get() : inputSrv.Get() };
  ctx->PSSetShaderResources(0, 2, srvs);
  ctx->Draw(3, 0);
  const auto tDraw = std::chrono::steady_clock::now();

  ctx->PSSetShaderResources(0, 2, nullSrvs);
  ctx->OMSetRenderTargets(0, nullptr, nullptr);
  // Cross-device shared texture: wait for GPU completion before Flutter opens it.
  ctx->End(syncQuery.Get());
  ctx->Flush();
  bool done = false;
  for (int i = 0; i < 2000; i++) {
    HRESULT qhr = ctx->GetData(syncQuery.Get(), nullptr, 0, 0);
    if (qhr == S_OK) { done = true; break; }
    if (FAILED(qhr)) break;
    std::this_thread::yield();
  }
  if (!done) {
    // query 未完成时不能再次 End 同一个 query；重建给下一帧
    syncQuery.Reset();
    D3D11_QUERY_DESC qd = {};
    qd.Query = D3D11_QUERY_EVENT;
    device->CreateQuery(&qd, &syncQuery);
  }
  const char* profile = getenv("DOMINO_GPU_PROFILE");
  if (profile && *profile && *profile != '0') {
    static std::atomic<int> profileFrames{ 0 };
    if (++profileFrames % 30 == 0) {
      const auto tEnd = std::chrono::steady_clock::now();
      const double up = std::chrono::duration<double, std::milli>(tUpload - tStart).count();
      const double draw = std::chrono::duration<double, std::milli>(tDraw - tUpload).count();
      const double wait = std::chrono::duration<double, std::milli>(tEnd - tDraw).count();
      fprintf(stderr, "[gpu] %dx%d upload=%.2f draw=%.2f wait=%.2f total=%.2f\n",
              width, height, up, draw, wait, up + draw + wait);
    }
  }
  return true;
}

bool GpuFrameOutput::Impl::renderGpu(int slot, int width, int height, int outIdx) {
  // Draw/query 期间必须挡住 acquire/ensureGpuSlots；否则跨 API 纹理可能在采样中
  // 被重建，实测会导致 query 永不完成或 nvwgf2umx fail-fast。
  std::lock_guard<std::mutex> lk(mu);
  if (slot < 0 || slot >= (int)gpuIns.size()) return false;
  GpuInSlot& in = gpuIns[(size_t)slot];
  if (!in.tex[0] || in.w != width || in.h != height) return false;

  // 上一轮 D3D 提交可能在驱动队列中；CUDART 写入的是同一批纹理，
  // 先 flush 再采样，避免跨 API 资源状态竞争。
  ctx->Flush();
  D3D11_VIEWPORT vp = { 0.f, 0.f, (float)width, (float)height, 0.f, 1.f };
  ctx->RSSetViewports(1, &vp);
  ctx->IASetInputLayout(nullptr);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(vs.Get(), nullptr, 0);
  ctx->PSSetShader(psGpu.Get(), nullptr, 0);
  ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
  ID3D11ShaderResourceView* srvs[3] = { in.srv[0].Get(), in.srv[1].Get(), in.srv[2].Get() };
  ctx->PSSetShaderResources(0, 3, srvs);
  ID3D11RenderTargetView* rt = outs[(size_t)outIdx].rtv.Get();
  ctx->OMSetRenderTargets(1, &rt, nullptr);
  ctx->Draw(3, 0);
  ID3D11ShaderResourceView* nullSrvs[3] = { nullptr, nullptr, nullptr };
  ctx->PSSetShaderResources(0, 3, nullSrvs);
  ctx->OMSetRenderTargets(0, nullptr, nullptr);

  ctx->End(syncQuery.Get());
  ctx->Flush();
  bool done = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  while (std::chrono::steady_clock::now() < deadline) {
    HRESULT qhr = ctx->GetData(syncQuery.Get(), nullptr, 0, 0);
    if (qhr == S_OK) { done = true; break; }
    if (FAILED(qhr)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!done) {
    // 查询未完成时 GPU 可能仍在采样这三张纹理，不能归还 slot。
    logx::error("gpu zero-copy: present query timeout");
    fatal.store(true);
    syncQuery.Reset();
    D3D11_QUERY_DESC qd = {};
    qd.Query = D3D11_QUERY_EVENT;
    if (FAILED(device->CreateQuery(&qd, &syncQuery))) fatal.store(true);
    return false;
  }
  return true;
}

void GpuFrameOutput::Impl::release(void* p) {
  auto* c = (ReleaseCtx*)p;
  if (!c) return;
  if (c->impl) {
    std::lock_guard<std::mutex> lk(c->impl->mu);
    if (c->gen == c->impl->gen && c->idx >= 0 && c->idx < (int)c->impl->outs.size())
      c->impl->outs[(size_t)c->idx].inUse = false;
  }
  if (c->tex) c->tex->Release();
  delete c;
}

GpuFrameOutput::GpuFrameOutput() : impl_(std::make_shared<Impl>()) {
  impl_->ok = impl_->init();
}

GpuFrameOutput::~GpuFrameOutput() = default;

bool GpuFrameOutput::ok() const { return impl_ && impl_->ok; }

std::string GpuFrameOutput::adapterName() const { return impl_ ? impl_->adapterName : std::string(); }
int GpuFrameOutput::adapterVendorId() const { return impl_ ? impl_->adapterVendor : 0; }
int GpuFrameOutput::adapterDeviceId() const { return impl_ ? impl_->adapterDevice : 0; }
long long GpuFrameOutput::dedicatedVideoMemory() const { return impl_ ? impl_->adapterMemory : 0; }
int GpuFrameOutput::featureLevel() const { return impl_ ? impl_->featureLevelValue : 0; }

void GpuFrameOutput::setBeautyParams(const EngineBeautyParams& bp) {
  if (!impl_) return;
  impl_->bp = bp;
}

void GpuFrameOutput::presentFrame(const uint8_t* rgb, int w, int h) {
  if (!impl_ || !impl_->ok || !rgb || w <= 0 || h <= 0) return;

  // Small 80x45 frame for filter previews (CPU math).
  {
    auto& s = impl_->smallRgb;
    for (int y = 0; y < kSmallH; y++) {
      int sy = (int)((int64_t)y * h / kSmallH);
      for (int x = 0; x < kSmallW; x++) {
        int sx = (int)((int64_t)x * w / kSmallW);
        size_t si = ((size_t)sy * w + sx) * 3;
        size_t di = ((size_t)y * kSmallW + x) * 3;
        s[di + 0] = rgb[si + 0];
        s[di + 1] = rgb[si + 1];
        s[di + 2] = rgb[si + 2];
      }
    }
  }

  if (!impl_->ensureSize(w, h)) return;

  int idx = -1;
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    for (int i = 0; i < (int)impl_->outs.size(); i++) {
      auto& o = impl_->outs[(size_t)i];
      if (o.texW == w && o.texH == h && i != impl_->front && !o.busy && !o.inUse) {
        idx = i;
        o.busy = true;
        break;
      }
    }
  }
  if (idx < 0) return;   // All shared textures are held by Flutter; drop this frame.
  impl_->copyMu.lock();
  const bool rendered = impl_->render(rgb, w, h, idx);
  impl_->copyMu.unlock();
  if (!rendered) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->outs[(size_t)idx].busy = false;
    return;
  }

  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto& o = impl_->outs[(size_t)idx];
    o.busy = false;
    impl_->front = idx;
    o.desc.struct_size = sizeof(FlutterDesktopGpuSurfaceDescriptor);
    o.desc.handle = o.handle;
    o.desc.width = (size_t)w;
    o.desc.height = (size_t)h;
    o.desc.visible_width = (size_t)w;
    o.desc.visible_height = (size_t)h;
    o.desc.format = kFlutterDesktopPixelFormatBGRA8888;
    o.desc.release_callback = Impl::release;
    o.desc.release_context = nullptr;
    impl_->newFrame.store(true);
  }
}

bool GpuFrameOutput::acquireGpuSrFrame(int w, int h, int& slot, void* textures[3]) {
  if (!impl_ || !impl_->ok || !textures || w <= 0 || h <= 0) return false;
  if (!impl_->ensureGpuSlots(w, h)) return false;
  std::lock_guard<std::mutex> lk(impl_->mu);
  for (int i = 0; i < (int)impl_->gpuIns.size(); i++) {
    auto& s = impl_->gpuIns[(size_t)i];
    if (s.w == w && s.h == h && !s.busy) {
      s.busy = true;
      slot = i;
      for (int j = 0; j < 3; j++) textures[j] = s.tex[j].Get();
      return true;
    }
  }
  return false;
}

void GpuFrameOutput::releaseGpuSrFrame(int slot) {
  if (!impl_ || slot < 0) return;
  std::lock_guard<std::mutex> lk(impl_->mu);
  if (slot < (int)impl_->gpuIns.size()) impl_->gpuIns[(size_t)slot].busy = false;
}

void GpuFrameOutput::lockGpuCopy() {
  if (impl_) impl_->copyMu.lock();
}

void GpuFrameOutput::unlockGpuCopy() {
  if (impl_) impl_->copyMu.unlock();
}

bool GpuFrameOutput::gpuFatal() const {
  return impl_ && impl_->fatal.load();
}

bool GpuFrameOutput::presentGpuSrFrame(int slot, int w, int h) {
  if (!impl_ || !impl_->ok || w <= 0 || h <= 0) return false;
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (slot < 0 || slot >= (int)impl_->gpuIns.size()) return false;
  }
  if (!impl_->ensureSize(w, h)) {
    releaseGpuSrFrame(slot);
    return false;
  }
  int idx = -1;
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    for (int i = 0; i < (int)impl_->outs.size(); i++) {
      auto& o = impl_->outs[(size_t)i];
      if (o.texW == w && o.texH == h && i != impl_->front && !o.busy && !o.inUse) {
        idx = i;
        o.busy = true;
        break;
      }
    }
  }
  if (idx < 0) {
    releaseGpuSrFrame(slot);
    return false;
  }
  impl_->copyMu.lock();
  const bool rendered = impl_->renderGpu(slot, w, h, idx);
  impl_->copyMu.unlock();
  if (!rendered) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->outs[(size_t)idx].busy = false;
    // fatal（query 超时）时 GPU 可能还在采样，slot 保持 busy，交给 CPU 回退。
    if (!impl_->fatal.load() && slot < (int)impl_->gpuIns.size()) {
      impl_->gpuIns[(size_t)slot].busy = false;
    }
    return false;
  }
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto& o = impl_->outs[(size_t)idx];
    o.busy = false;
    if (slot < (int)impl_->gpuIns.size()) impl_->gpuIns[(size_t)slot].busy = false;
    impl_->front = idx;
    o.desc.struct_size = sizeof(FlutterDesktopGpuSurfaceDescriptor);
    o.desc.handle = o.handle;
    o.desc.width = (size_t)w;
    o.desc.height = (size_t)h;
    o.desc.visible_width = (size_t)w;
    o.desc.visible_height = (size_t)h;
    o.desc.format = kFlutterDesktopPixelFormatBGRA8888;
    o.desc.release_callback = Impl::release;
    o.desc.release_context = nullptr;
    impl_->newFrame.store(true);
  }
  return true;
}

bool GpuFrameOutput::hasNewFrame() const { return impl_ && impl_->newFrame.load(); }
void GpuFrameOutput::clearNewFrame() { if (impl_) impl_->newFrame.store(false); }
int GpuFrameOutput::width() const { return impl_ ? impl_->w : 0; }
int GpuFrameOutput::height() const { return impl_ ? impl_->h : 0; }

void GpuFrameOutput::makeFilterPreviews(int count) {
  if (!impl_ || impl_->smallRgb.empty() || count < 1) return;
  impl_->previewGen.setBeautyParams(impl_->bp);
  impl_->previewGen.presentFrame(impl_->smallRgb.data(), kSmallW, kSmallH);
  impl_->previewGen.makeFilterPreviews(count);
}

int GpuFrameOutput::previewCount() const { return impl_ ? impl_->previewGen.previewCount() : 0; }
int GpuFrameOutput::previewWidth() const { return impl_ ? impl_->previewGen.previewWidth() : 0; }
int GpuFrameOutput::previewHeight() const { return impl_ ? impl_->previewGen.previewHeight() : 0; }
const uint8_t* GpuFrameOutput::previewBuffer(int idx) const {
  return impl_ ? impl_->previewGen.previewBuffer(idx) : nullptr;
}

const FlutterDesktopGpuSurfaceDescriptor* GpuFrameOutput::acquireDescriptor() {
  if (!impl_) return nullptr;
  std::lock_guard<std::mutex> lk(impl_->mu);
  if (impl_->front < 0 || impl_->front >= (int)impl_->outs.size()) return nullptr;
  auto& o = impl_->outs[(size_t)impl_->front];
  if (!o.tex || !o.handle || o.inUse) return nullptr;
  o.tex->AddRef();
  o.inUse = true;
  o.desc.release_context = new Impl::ReleaseCtx{ impl_, impl_->front, impl_->gen, o.tex.Get() };
  return &o.desc;
}

