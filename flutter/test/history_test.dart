import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:domino_flutter/core/history.dart';

void main() {
  test('PlayHistory persists recent files and position', () async {
    final dir = await Directory.systemTemp.createTemp('domino_history_test');
    addTearDown(() => dir.delete(recursive: true));
    final path = '${dir.path}/history.json';

    final h = PlayHistory(path);
    h.touch(r'C:\clips\a.mp4');
    h.touch(r'C:\clips\b.mp4');
    h.updatePosition(r'C:\clips\a.mp4', 42.5);
    expect(h.entries.first.path, r'C:\clips\b.mp4');

    final loaded = PlayHistory(path);
    await loaded.load();
    expect(loaded.entries.length, 2);
    expect(loaded.entries.first.name, 'b.mp4');
    expect(loaded.entries.any((e) => e.path.endsWith('a.mp4') && e.position == 42.5), isTrue);
  });
}
