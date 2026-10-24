#include "convert_avx2.h"
#include <cstddef>
#include <immintrin.h>

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

