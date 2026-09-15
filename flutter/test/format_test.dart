import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';

import 'package:domino_flutter/core/format.dart';

void main() {
  group('formatTime', () {
    test('零/负数 → 00:00', () {
      expect(formatTime(0), '00:00');
      expect(formatTime(-5), '00:00');
    });
    test('分钟对齐', () {
      expect(formatTime(5), '00:05');
      expect(formatTime(59.9), '00:59');
      expect(formatTime(65), '01:05');
      expect(formatTime(3600), '60:00');
    });
    test('NaN/Infinity 安全', () {
      expect(formatTime(double.nan), '00:00');
      expect(formatTime(double.infinity), '00:00');
    });
  });

  group('taskStatusName', () {
    test('映射', () {
      expect(taskStatusName(0), '未开始');
      expect(taskStatusName(1), '进行中');
      expect(taskStatusName(2), '未完成');
      expect(taskStatusName(3), '完成');
      expect(taskStatusName(4), '失败');
    });
    test('越界回退', () {
      expect(taskStatusName(-1), '失败');
      expect(taskStatusName(99), '失败');
    });
  });

  test('filterNames 数量稳定', () {
    expect(filterNames.length, 7);
    expect(filterNames.first, '原图');
  });

  group('rgb24ToRgba', () {
    test('按像素映射通道并置 alpha=255', () {
      // 2x2 图：每像素 RGB
      final rgb = Uint8List.fromList([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]);
      final rgba = rgb24ToRgba(rgb, 2, 2);
      expect(rgba.length, 16);
      expect(rgba.sublist(0, 4), [1, 2, 3, 255]);
      expect(rgba.sublist(4, 8), [4, 5, 6, 255]);
      expect(rgba.sublist(8, 12), [7, 8, 9, 255]);
      expect(rgba.sublist(12, 16), [10, 11, 12, 255]);
    });

    test('源不足时剩余像素保持透明（不越界）', () {
      final rgb = Uint8List.fromList([1, 2, 3]); // 仅 1 像素，声明 2x1
      final rgba = rgb24ToRgba(rgb, 2, 1);
      expect(rgba.length, 8);
      expect(rgba.sublist(0, 4), [1, 2, 3, 255]);
      expect(rgba.sublist(4, 8), [0, 0, 0, 0]);
    });
  });

  group('baseName', () {
    test('取文件名（兼容 \\ 与 /）', () {
      expect(baseName(r'C:\dir\movie.mp4'), 'movie.mp4');
      expect(baseName('C:/dir/movie.mp4'), 'movie.mp4');
      expect(baseName('movie.mp4'), 'movie.mp4');
      expect(baseName(''), '');
    });
  });
}
