// 展示层纯函数（无引擎/FFI 依赖，便于单元测试）。
import 'dart:typed_data';

/// 任务状态名（与 engine_bridge.h 的 ST_* 对应）。
const taskStatusNames = ['未开始', '进行中', '未完成', '完成', '失败'];

/// 任务状态 → 显示名（越界回退"失败"）。
String taskStatusName(int status) {
  if (status < 0 || status >= taskStatusNames.length) return taskStatusNames.last;
  return taskStatusNames[status];
}

/// 秒 → mm:ss。
String formatTime(double s) {
  final t = s.isFinite && s > 0 ? s.toInt() : 0;
  final m = t ~/ 60;
  final sec = t % 60;
  return '${m.toString().padLeft(2, '0')}:${sec.toString().padLeft(2, '0')}';
}

/// 取路径中的文件名（兼容 \ 与 /）。
String baseName(String path) {
  if (path.isEmpty) return '';
  final i = path.lastIndexOf(RegExp(r'[\\/]'));
  return i >= 0 ? path.substring(i + 1) : path;
}

/// 滤镜名列表（顺序与 C++ applyFilter 的 idx 一一对应）。
const filterNames = ['原图', '黑白', '日系清新', '暖色', '冷色', '复古胶片', '粉调'];

/// RGB24 → RGBA8888（按像素转换，alpha 置 0xFF）。
/// 注意：源按 3 字节/像素、目标按 4 字节/像素，索引不能混用。
Uint8List rgb24ToRgba(Uint8List rgb, int width, int height) {
  final out = Uint8List(width * height * 4);
  final pixels = (rgb.length ~/ 3).clamp(0, width * height);
  for (int p = 0; p < pixels; p++) {
    out[p * 4 + 0] = rgb[p * 3 + 0];
    out[p * 4 + 1] = rgb[p * 3 + 1];
    out[p * 4 + 2] = rgb[p * 3 + 2];
    out[p * 4 + 3] = 0xFF;
  }
  return out;
}
