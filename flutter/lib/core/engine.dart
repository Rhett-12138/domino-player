import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'format.dart';

/// 引擎接口：UI 只依赖本接口（DominoEngine 是 FFI 实现；测试注入 FakeEngine，无需 DLL）。
abstract class EngineApi {
  // ---- 播放 ----
  bool open(String path);
  void close();
  bool get hasVideo;
  void play();
  void pause();
  bool get paused;
  void seek(double t);
  void setVolume(double v);
  double get position;
  double get duration;
  bool get eof;
  int get frameW;
  int get frameH;

  /// 实时超分开关与目标宽；失败时引擎回退源帧。
  void setSR(bool enabled, int targetW);
  bool get srActive;
  bool get srFailed;
  int get srTargetW;
  bool get srAutoDegraded;

  /// 全屏切换；返回当前是否全屏。
  bool setFullscreen(bool on);

  // ---- 美颜/滤镜 ----
  void setBeauty(double smooth, double white);
  void setFilter(int idx, double amt);

  // ---- 系统文件选择框 ----
  /// 弹出文件选择框；currentPath 提供初始目录。选中返回路径，取消返回 null。
  String? openDialog(String currentPath);

  // ---- 每帧驱动 ----
  void pump();
  int get textureId;
  double get fps;

  // ---- 任务中心 ----
  int taskCreate(String source, int targetW, String desc);
  void taskStart(int idx);
  void taskStop(int idx);
  void taskRemove(int idx);
  int get taskCount;
  int taskStatus(int idx);
  int taskFrames(int idx);
  int taskTotal(int idx);
  double taskResumeTime(int idx);
  String taskName(int idx);
  String taskSource(int idx);
  String taskTemp(int idx);

  // ---- 滤镜缩略图 ----
  int makePreviews(int count);
  int get previewW;
  int get previewH;
  Uint8List previewBytes(int idx);
}

/// domino_engine.dll 的 Dart FFI 绑定（见 flutter/native/engine_bridge.h）
class DominoEngine implements EngineApi {
  DominoEngine._(this._h);
  final Pointer<Void> _h;

  static final DynamicLibrary _lib = _load();

  static DynamicLibrary _load() {
    final exe = File(Platform.resolvedExecutable);
    final dir = exe.parent.path;
    return DynamicLibrary.open('$dir\\domino_engine.dll');
  }

  // ---- 生命周期 ----
  static DominoEngine create(String modelDir, String dataRoot) {
    final model = modelDir.toNativeUtf8();
    final data = dataRoot.toNativeUtf8();
    try {
      return DominoEngine._(_create(model, data));
    } finally {
      calloc.free(model);
      calloc.free(data);
    }
  }

  void dispose() => _destroy(_h);

  // ---- 播放 ----
  bool open(String path) {
    final p = path.toNativeUtf8();
    final ok = _open(_h, p);
    calloc.free(p);
    return ok == 1;
  }

  void close() => _close(_h);
  bool get hasVideo => _hasVideo(_h) == 1;
  void play() => _play(_h);
  void pause() => _pause(_h);
  bool get paused => _paused(_h) == 1;
  void seek(double t) => _seek(_h, t);
  void setVolume(double v) => _volume(_h, v);
  double get position => _position(_h);
  double get duration => _duration(_h);

  // ---- 实时超分 ----
  void setSR(bool enabled, int targetW) => _setSR(_h, enabled ? 1 : 0, targetW);
  bool get srActive => _srActive(_h) == 1;
  bool get srFailed => _srFailed(_h) == 1;
  int get srTargetW => _srTargetW(_h);
  bool get srAutoDegraded => _srAuto(_h) == 1;

  // ---- 美颜/滤镜 ----
  void setBeauty(double smooth, double white) => _beauty(_h, smooth, white);
  void setFilter(int idx, double amt) => _filter(_h, idx, amt);

  // ---- 系统文件选择框 ----
  @override
  String? openDialog(String currentPath) {
    const cap = 8192;
    final buf = calloc<Uint8>(cap);
    final init = currentPath.toNativeUtf8();
    final n = _openDialog(_h, init, buf, cap);
    calloc.free(init);
    if (n != 1) {
      calloc.free(buf);
      return null;
    }
    final s = buf.cast<Utf8>().toDartString();
    calloc.free(buf);
    return s;
  }

  // ---- 每帧驱动 ----
  void pump() => _pump(_h);
  int get textureId => _textureId(_h);

  /// 实际呈现帧率（引擎每 1s 统计）。
  double get fps => _fps(_h);

  /// 是否已播到结尾（EOF；重新播放/seek 后复位）。
  bool get eof => _eof(_h) == 1;

  /// 当前视频帧宽/高（无帧时可能为 0）。
  int get frameW => _frameW(_h);
  int get frameH => _frameH(_h);

  /// 全屏切换；返回当前是否全屏。
  bool setFullscreen(bool on) => _setFullscreen(_h, on ? 1 : 0) == 1;

  // ---- 任务中心 ----
  int taskCreate(String source, int targetW, String desc) {
    final s = source.toNativeUtf8();
    final d = desc.toNativeUtf8();
    final idx = _taskCreate(_h, s, targetW, d);
    calloc.free(s);
    calloc.free(d);
    return idx;
  }

  void taskStart(int idx) => _taskStart(_h, idx);
  void taskStop(int idx) => _taskStop(_h, idx);
  void taskRemove(int idx) => _taskRemove(_h, idx);
  int get taskCount => _taskCount(_h);
  int taskStatus(int idx) => _taskStatus(_h, idx);
  int taskFrames(int idx) => _taskFrames(_h, idx);
  int taskTotal(int idx) => _taskTotal(_h, idx);
  double taskResumeTime(int idx) => _taskResumeTime(_h, idx);

  String taskName(int idx) => _taskString(_taskName, idx);
  String taskSource(int idx) => _taskString(_taskSource, idx);
  String taskTemp(int idx) => _taskString(_taskTemp, idx);

  String _taskString(int Function(Pointer<Void>, int, Pointer<Uint8>, int) fn, int idx) {
    const cap = 4096;
    final buf = calloc<Uint8>(cap);
    fn(_h, idx, buf, cap);
    final s = buf.cast<Utf8>().toDartString();
    calloc.free(buf);
    return s;
  }

  // ---- 滤镜缩略图 ----
  // 生成 count 张（含原图）当前帧滤镜预览，返回实际张数。
  int makePreviews(int count) => _makePreviews(_h, count);
  int get previewW => _previewW(_h);
  int get previewH => _previewH(_h);

  /// 取出第 idx 张预览（RGB24），返回 RGBA8888 的 Uint8List。
  Uint8List previewBytes(int idx) {
    const cap = 1 << 20;
    final buf = calloc<Uint8>(cap);
    final n = _previewBuf(_h, idx, buf, cap); // n = w*h*3（RGB24）
    final w = previewW;
    final h = previewH;
    final out = rgb24ToRgba(n > 0 ? buf.asTypedList(n) : Uint8List(0), w, h);
    calloc.free(buf);
    return out;
  }
  // ---- FFI 声明（字段用 Dart 类型；lookupFunction<原生签名, Dart 签名>） ----
  static final Pointer<Void> Function(Pointer<Utf8>, Pointer<Utf8>) _create = _lib
      .lookupFunction<Pointer<Void> Function(Pointer<Utf8>, Pointer<Utf8>),
          Pointer<Void> Function(Pointer<Utf8>, Pointer<Utf8>)>('domino_create');
  static final void Function(Pointer<Void>) _destroy = _lib
      .lookupFunction<Void Function(Pointer<Void>), void Function(Pointer<Void>)>('domino_destroy');
  static final int Function(Pointer<Void>, Pointer<Utf8>) _open = _lib
      .lookupFunction<Int32 Function(Pointer<Void>, Pointer<Utf8>),
          int Function(Pointer<Void>, Pointer<Utf8>)>('domino_open');
  static final void Function(Pointer<Void>) _close = _lib
      .lookupFunction<Void Function(Pointer<Void>), void Function(Pointer<Void>)>('domino_close');
  static final int Function(Pointer<Void>) _hasVideo = _lib
      .lookupFunction<Int32 Function(Pointer<Void>), int Function(Pointer<Void>)>('domino_has_video');
  static final void Function(Pointer<Void>) _play = _lib
      .lookupFunction<Void Function(Pointer<Void>), void Function(Pointer<Void>)>('domino_play');
  static final void Function(Pointer<Void>) _pause = _lib
      .lookupFunction<Void Function(Pointer<Void>), void Function(Pointer<Void>)>('domino_pause');
  static final int Function(Pointer<Void>) _paused = _lib
      .lookupFunction<Int32 Function(Pointer<Void>), int Function(Pointer<Void>)>('domino_paused');
  static final void Function(Pointer<Void>, double) _seek = _lib
      .lookupFunction<Void Function(Pointer<Void>, Double), void Function(Pointer<Void>, double)>('domino_seek');
  static final void Function(Pointer<Void>, double) _volume = _lib
      .lookupFunction<Void Function(Pointer<Void>, Double), void Function(Pointer<Void>, double)>('domino_volume');
  static final double Function(Pointer<Void>) _position = _lib
      .lookupFunction<Double Function(Pointer<Void>), double Function(Pointer<Void>)>('domino_position');
  static final double Function(Pointer<Void>) _duration = _lib
      .lookupFunction<Double Function(Pointer<Void>), double Function(Pointer<Void>)>('domino_duration');
  static final void Function(Pointer<Void>, int, int) _setSR = _lib
      .lookupFunction<Void Function(Pointer<Void>, Int32, Int32),
          void Function(Pointer<Void>, int, int)>('domino_set_sr');
  static final int Function(Pointer<Void>) _srActive = _lib
      .lookupFunction<Int32 Function(Pointer<Void>), int Function(Pointer<Void>)>('domino_sr_active');
  static final int Function(Pointer<Void>) _srFailed = _lib
      .lookupFunction<Int32 Function(Pointer<Void>), int Function(Pointer<Void>)>('domino_sr_failed');
  static final int Function(Pointer<Void>) _srTargetW = _lib
      .lookupFunction<Int32 Function(Pointer<Void>), int Function(Pointer<Void>)>('domino_sr_target_w');
  static final int Function(Pointer<Void>) _srAuto = _lib
      .lookupFunction<Int32 Function(Pointer<Void>), int Function(Pointer<Void>)>('domino_sr_auto');
  static final void Function(Pointer<Void>, double, double) _beauty = _lib
      .lookupFunction<Void Function(Pointer<Void>, Float, Float),
          void Function(Pointer<Void>, double, double)>('domino_beauty');
  static final void Function(Pointer<Void>, int, double) _filter = _lib
      .lookupFunction<Void Function(Pointer<Void>, Int32, Float),
          void Function(Pointer<Void>, int, double)>('domino_filter');
  static final void Function(Pointer<Void>) _pump = _lib
      .lookupFunction<Void Function(Pointer<Void>), void Function(Pointer<Void>)>('domino_pump');
  static final int Function(Pointer<Void>) _textureId = _lib
      .lookupFunction<Int64 Function(Pointer<Void>), int Function(Pointer<Void>)>('domino_texture_id');
  static final double Function(Pointer<Void>) _fps = _lib
      .lookupFunction<Double Function(Pointer<Void>), double Function(Pointer<Void>)>('domino_fps');
  static final int Function(Pointer<Void>) _eof = _lib
      .lookupFunction<Int32 Function(Pointer<Void>), int Function(Pointer<Void>)>('domino_eof');
  static final int Function(Pointer<Void>) _frameW = _lib
      .lookupFunction<Int32 Function(Pointer<Void>), int Function(Pointer<Void>)>('domino_frame_w');
  static final int Function(Pointer<Void>) _frameH = _lib
      .lookupFunction<Int32 Function(Pointer<Void>), int Function(Pointer<Void>)>('domino_frame_h');
  static final int Function(Pointer<Void>, int) _setFullscreen = _lib
      .lookupFunction<Int32 Function(Pointer<Void>, Int32), int Function(Pointer<Void>, int)>('domino_set_fullscreen');

  // ---- 任务 FFI ----
  static final int Function(Pointer<Void>, Pointer<Utf8>, int, Pointer<Utf8>) _taskCreate = _lib
      .lookupFunction<Int32 Function(Pointer<Void>, Pointer<Utf8>, Int32, Pointer<Utf8>),
          int Function(Pointer<Void>, Pointer<Utf8>, int, Pointer<Utf8>)>('domino_task_create');
  static final void Function(Pointer<Void>, int) _taskStart = _lib
      .lookupFunction<Void Function(Pointer<Void>, Int32), void Function(Pointer<Void>, int)>('domino_task_start');
  static final void Function(Pointer<Void>, int) _taskStop = _lib
      .lookupFunction<Void Function(Pointer<Void>, Int32), void Function(Pointer<Void>, int)>('domino_task_stop');
  static final void Function(Pointer<Void>, int) _taskRemove = _lib
      .lookupFunction<Void Function(Pointer<Void>, Int32), void Function(Pointer<Void>, int)>('domino_task_remove');
  static final int Function(Pointer<Void>) _taskCount = _lib
      .lookupFunction<Int32 Function(Pointer<Void>), int Function(Pointer<Void>)>('domino_task_count');
  static final int Function(Pointer<Void>, int) _taskStatus = _lib
      .lookupFunction<Int32 Function(Pointer<Void>, Int32), int Function(Pointer<Void>, int)>('domino_task_status');
  static final int Function(Pointer<Void>, int) _taskFrames = _lib
      .lookupFunction<Int64 Function(Pointer<Void>, Int32), int Function(Pointer<Void>, int)>('domino_task_frames');
  static final int Function(Pointer<Void>, int) _taskTotal = _lib
      .lookupFunction<Int64 Function(Pointer<Void>, Int32), int Function(Pointer<Void>, int)>('domino_task_total');
  static final double Function(Pointer<Void>, int) _taskResumeTime = _lib
      .lookupFunction<Double Function(Pointer<Void>, Int32), double Function(Pointer<Void>, int)>('domino_task_resume_time');
  static final int Function(Pointer<Void>, int, Pointer<Uint8>, int) _taskName = _lib
      .lookupFunction<Int32 Function(Pointer<Void>, Int32, Pointer<Uint8>, Int32),
          int Function(Pointer<Void>, int, Pointer<Uint8>, int)>('domino_task_name');
  static final int Function(Pointer<Void>, int, Pointer<Uint8>, int) _taskSource = _lib
      .lookupFunction<Int32 Function(Pointer<Void>, Int32, Pointer<Uint8>, Int32),
          int Function(Pointer<Void>, int, Pointer<Uint8>, int)>('domino_task_source');
  static final int Function(Pointer<Void>, int, Pointer<Uint8>, int) _taskTemp = _lib
      .lookupFunction<Int32 Function(Pointer<Void>, Int32, Pointer<Uint8>, Int32),
          int Function(Pointer<Void>, int, Pointer<Uint8>, int)>('domino_task_temp');

  // ---- 滤镜缩略图 FFI ----
  static final int Function(Pointer<Void>, int) _makePreviews = _lib
      .lookupFunction<Int32 Function(Pointer<Void>, Int32), int Function(Pointer<Void>, int)>('domino_make_previews');
  static final int Function(Pointer<Void>) _previewW = _lib
      .lookupFunction<Int32 Function(Pointer<Void>), int Function(Pointer<Void>)>('domino_preview_w');
  static final int Function(Pointer<Void>) _previewH = _lib
      .lookupFunction<Int32 Function(Pointer<Void>), int Function(Pointer<Void>)>('domino_preview_h');
  static final int Function(Pointer<Void>, int, Pointer<Uint8>, int) _previewBuf = _lib
      .lookupFunction<Int32 Function(Pointer<Void>, Int32, Pointer<Uint8>, Int32),
          int Function(Pointer<Void>, int, Pointer<Uint8>, int)>('domino_preview_buf');

  // ---- 系统文件选择框 FFI ----
  static final int Function(Pointer<Void>, Pointer<Utf8>, Pointer<Uint8>, int) _openDialog = _lib
      .lookupFunction<Int32 Function(Pointer<Void>, Pointer<Utf8>, Pointer<Uint8>, Int32),
          int Function(Pointer<Void>, Pointer<Utf8>, Pointer<Uint8>, int)>('domino_open_dialog');
}
