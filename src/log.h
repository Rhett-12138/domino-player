#pragma once
#include <cstdio>
#include <cstdarg>

// 轻量日志：统一走 stderr（无缓冲），带模块前缀
namespace logx {
inline void info(const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "[info] ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}
inline void warn(const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "[warn] ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}
inline void error(const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "[error] ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}
}
