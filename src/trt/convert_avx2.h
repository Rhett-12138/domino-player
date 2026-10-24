#pragma once
#include <cstdint>

// AVX2/F16C 专用转换；调用方必须先检查 CPU 支持。
void trtConvertHalfToRgbAvx2(const uint16_t* r, const uint16_t* g, const uint16_t* b,
                             int w, int h, uint8_t* dst, const uint8_t* halfToByte);
