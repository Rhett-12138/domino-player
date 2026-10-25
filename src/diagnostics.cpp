#include "diagnostics.h"

#include <cstdio>

namespace {

std::string jsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c; break;
    }
  }
  return out;
}

void appendBool(std::string& s, const char* key, bool value, bool comma = true) {
  s += '"';
  s += key;
  s += "\":";
  s += value ? "true" : "false";
  if (comma) s += ',';
}

}  // namespace

std::string diagnosticsToJson(const DiagnosticsInfo& i) {
  std::string s = "{\"version\":\"";
  s += jsonEscape(i.version);
  s += "\",\"backend\":\"";
  s += jsonEscape(i.backend);
  s += "\",\"output\":\"";
  s += jsonEscape(i.output);
  s += "\",\"sr\":{";
  appendBool(s, "inited", i.srInited);
  appendBool(s, "failed", i.srFailed);
  appendBool(s, "beauty", i.beauty);
  char num[256];
  snprintf(num, sizeof(num), "\"targetW\":%d},", i.targetW);
  s += num;
  s += "\"zeroCopy\":{";
  appendBool(s, "active", i.zeroCopyActive);
  appendBool(s, "gpuSink", i.gpuSink);
  appendBool(s, "fatal", i.gpuFatal);
  s += "\"reason\":\"";
  s += jsonEscape(i.fallbackReason);
  s += "\"},\"gpu\":{\"adapter\":\"";
  s += jsonEscape(i.adapter);
  snprintf(num, sizeof(num), "\",\"vendorId\":%d,\"deviceId\":%d,\"memoryMB\":%lld,\"featureLevel\":\"%s\"},",
           i.vendorId, i.deviceId, i.memoryMB, jsonEscape(i.featureLevel).c_str());
  s += num;
  s += "\"cuda\":{";
  appendBool(s, "cudart", i.cudart);
  appendBool(s, "tensorrt", i.tensorrt, false);
  s += "},\"vulkan\":{";
  s += "\"available\":";
  s += i.vulkanGpuCount > 0 ? "true" : "false";
  snprintf(num, sizeof(num), ",\"gpuCount\":%d},", i.vulkanGpuCount);
  s += num;
  s += "\"fallback\":{\"reason\":\"";
  s += jsonEscape(i.fallbackReason);
  s += "\",\"lastError\":\"\"},\"fps\":";
  snprintf(num, sizeof(num), "%.2f}", i.fps);
  s += num;
  return s;
}
