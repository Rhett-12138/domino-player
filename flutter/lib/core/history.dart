import 'dart:convert';
import 'dart:io';

/// 播放历史条目：最近打开的文件与上次播放位置。
class PlayHistoryEntry {
  PlayHistoryEntry({
    required this.path,
    required this.position,
    required this.updatedAt,
  });

  final String path;
  double position;
  int updatedAt;

  String get name {
    final i = path.lastIndexOf(RegExp(r'[\\/]'));
    return i >= 0 ? path.substring(i + 1) : path;
  }

  Map<String, dynamic> toJson() => {
        'path': path,
        'position': position,
        'updatedAt': updatedAt,
      };

  static PlayHistoryEntry? fromJson(dynamic v) {
    if (v is! Map) return null;
    final path = v['path'];
    if (path is! String || path.isEmpty) return null;
    final pos = v['position'];
    final ts = v['updatedAt'];
    return PlayHistoryEntry(
      path: path,
      position: pos is num ? pos.toDouble() : 0,
      updatedAt: ts is num ? ts.toInt() : 0,
    );
  }
}

/// 轻量 JSON 播放历史；filePath 为空时只在内存中使用（测试/无 AppData 环境）。
class PlayHistory {
  PlayHistory(this.filePath);

  final String? filePath;
  final List<PlayHistoryEntry> entries = [];

  static String? defaultPath() {
    final appData = Platform.environment['APPDATA'];
    if (appData == null || appData.isEmpty) return null;
    return '$appData\\DominoPlayer\\history.json';
  }

  Future<void> load() async {
    final p = filePath;
    if (p == null) return;
    try {
      final f = File(p);
      if (!await f.exists()) return;
      final data = jsonDecode(await f.readAsString());
      if (data is! List) return;
      entries
        ..clear()
        ..addAll(data.map(PlayHistoryEntry.fromJson).whereType<PlayHistoryEntry>());
      entries.sort((a, b) => b.updatedAt.compareTo(a.updatedAt));
    } catch (_) {
      // 历史损坏不应影响播放；下次保存会覆盖。
    }
  }

  void save() {
    final p = filePath;
    if (p == null) return;
    try {
      final f = File(p);
      f.parent.createSync(recursive: true);
      f.writeAsStringSync(jsonEncode(entries.map((e) => e.toJson()).toList()));
    } catch (_) {}
  }

  void touch(String path, {double? position}) {
    if (path.isEmpty) return;
    final now = DateTime.now().millisecondsSinceEpoch;
    PlayHistoryEntry? hit;
    for (final e in entries) {
      if (e.path == path) {
        hit = e;
        break;
      }
    }
    if (hit == null) {
      hit = PlayHistoryEntry(path: path, position: position ?? 0, updatedAt: now);
      entries.insert(0, hit);
    } else {
      hit.updatedAt = now;
      if (position != null && position > 0) hit.position = position;
      entries.remove(hit);
      entries.insert(0, hit);
    }
    while (entries.length > 20) {
      entries.removeLast();
    }
    save();
  }

  void updatePosition(String path, double position) {
    if (path.isEmpty || position < 0) return;
    for (final e in entries) {
      if (e.path == path) {
        e.position = position;
        save();
        return;
      }
    }
  }

  void clear() {
    entries.clear();
    save();
  }
}
