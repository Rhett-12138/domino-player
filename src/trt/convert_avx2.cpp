#include "convert_avx2.h"
#include <cstddef>
#include <cstring>
#include <immintrin.h>

namespace {
inline __m128i load12(const uint8_t* p) {
  __m128i lo = _mm_loadl_epi64((const __m128i*)p);
  uint32_t hi = 0;
  memcpy(&hi, p + 8, 4);
  return _mm_insert_epi32(lo, (int)hi, 2);
}

inline uint16_t f2h(float v) {
  return (uint16_t)_mm_cvtsi128_si32(_mm_cvtps_ph(_mm_set_ss(v), 0));
}
}  // namespace

void trtConvertHalfToRgbAvx2(const uint16_t* r, const uint16_t* g, const uint16_t* b,
                             int w, int h, uint8_t* dst, const uint8_t* halfToByte) {
  const __m256 scale = _mm256_set1_ps(255.0f);
  const __m256 zero = _mm256_setzero_ps();
  const __m256 maxv = _mm256_set1_ps(255.0f);
  for (int y = 0; y < h; y++) {
    const uint16_t* rp = r + (size_t)y * w;
    const uint16_t* gp = g + (size_t)y * w;
    const uint16_t* bp = b + (size_t)y * w;
    uint8_t* dp = dst + (size_t)y * w * 3;
    int x = 0;
    for (; x + 8 <= w; x += 8) {
      __m128i hr = _mm_loadu_si128((const __m128i*)(rp + x));
      __m128i hg = _mm_loadu_si128((const __m128i*)(gp + x));
      __m128i hb = _mm_loadu_si128((const __m128i*)(bp + x));
      __m256 fr = _mm256_mul_ps(_mm256_cvtph_ps(hr), scale);
      __m256 fg = _mm256_mul_ps(_mm256_cvtph_ps(hg), scale);
      __m256 fb = _mm256_mul_ps(_mm256_cvtph_ps(hb), scale);
      fr = _mm256_min_ps(maxv, _mm256_max_ps(zero, fr));
      fg = _mm256_min_ps(maxv, _mm256_max_ps(zero, fg));
      fb = _mm256_min_ps(maxv, _mm256_max_ps(zero, fb));
      int32_t ir[8], ig[8], ib[8];
      _mm256_storeu_si256((__m256i*)ir, _mm256_cvttps_epi32(fr));
      _mm256_storeu_si256((__m256i*)ig, _mm256_cvttps_epi32(fg));
      _mm256_storeu_si256((__m256i*)ib, _mm256_cvttps_epi32(fb));
      for (int j = 0; j < 8; j++) {
        dp[(x + j) * 3 + 0] = (uint8_t)ir[j];
        dp[(x + j) * 3 + 1] = (uint8_t)ig[j];
        dp[(x + j) * 3 + 2] = (uint8_t)ib[j];
      }
    }
    for (; x < w; x++) {
      dp[x * 3 + 0] = halfToByte[rp[x]];
      dp[x * 3 + 1] = halfToByte[gp[x]];
      dp[x * 3 + 2] = halfToByte[bp[x]];
    }
  }
}

void trtRgb24ToHalfPlanarAvx2(const uint8_t* src, int w, int h,
                              uint16_t* r, uint16_t* g, uint16_t* b) {
  const __m128i maskR = _mm_setr_epi8(0, 3, 6, 9, (char)0x80, (char)0x80, (char)0x80,
                                      (char)0x80, (char)0x80, (char)0x80, (char)0x80,
                                      (char)0x80, (char)0x80, (char)0x80, (char)0x80,
                                      (char)0x80);
  const __m128i maskG = _mm_setr_epi8(1, 4, 7, 10, (char)0x80, (char)0x80, (char)0x80,
                                      (char)0x80, (char)0x80, (char)0x80, (char)0x80,
                                      (char)0x80, (char)0x80, (char)0x80, (char)0x80,
                                      (char)0x80);
  const __m128i maskB = _mm_setr_epi8(2, 5, 8, 11, (char)0x80, (char)0x80, (char)0x80,
                                      (char)0x80, (char)0x80, (char)0x80, (char)0x80,
                                      (char)0x80, (char)0x80, (char)0x80, (char)0x80,
                                      (char)0x80);
  const __m128 inv255 = _mm_set1_ps(1.0f / 255.0f);
  for (int y = 0; y < h; y++) {
    const uint8_t* sp = src + (size_t)y * w * 3;
    uint16_t* rp = r + (size_t)y * w;
    uint16_t* gp = g + (size_t)y * w;
    uint16_t* bp = b + (size_t)y * w;
    int x = 0;
    for (; x + 4 <= w; x += 4) {
      const __m128i p = load12(sp + (size_t)x * 3);
      const __m128i ir = _mm_cvtepu8_epi32(_mm_shuffle_epi8(p, maskR));
      const __m128i ig = _mm_cvtepu8_epi32(_mm_shuffle_epi8(p, maskG));
      const __m128i ib = _mm_cvtepu8_epi32(_mm_shuffle_epi8(p, maskB));
      const __m128 fr = _mm_mul_ps(_mm_cvtepi32_ps(ir), inv255);
      const __m128 fg = _mm_mul_ps(_mm_cvtepi32_ps(ig), inv255);
      const __m128 fb = _mm_mul_ps(_mm_cvtepi32_ps(ib), inv255);
      _mm_storel_epi64((__m128i*)(rp + x), _mm_cvtps_ph(fr, 0));
      _mm_storel_epi64((__m128i*)(gp + x), _mm_cvtps_ph(fg, 0));
      _mm_storel_epi64((__m128i*)(bp + x), _mm_cvtps_ph(fb, 0));
    }
    for (; x < w; x++) {
      const uint8_t* p = sp + (size_t)x * 3;
      rp[x] = f2h((float)p[0] * (1.0f / 255.0f));
      gp[x] = f2h((float)p[1] * (1.0f / 255.0f));
      bp[x] = f2h((float)p[2] * (1.0f / 255.0f));
    }
  }
}

