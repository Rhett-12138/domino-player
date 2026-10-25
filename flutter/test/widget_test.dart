import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:domino_flutter/core/engine.dart';
import 'package:domino_flutter/main.dart';
import 'package:domino_flutter/ui/task_center.dart';

/// 内存版引擎桩：不触 FFI/DLL，用于驱动 UI 逻辑测试。
class FakeEngine implements EngineApi {
  bool openOk = true;
  bool eofState = false;
  bool pausedState = true;
  int openCalls = 0;
  int playCalls = 0;
  int pauseCalls = 0;
  String? lastOpened;
  int createCalls = 0;
  int lastTaskQuality = 2;
  int lastTaskModel = 0;
  String? dialogResult = r'C:\picked\movie.mp4';

  final List<_FakeTask> tasks = [];
  String diagJson =
      '{"version":"0.3.0","backend":"tensorrt","output":"d3d11","zeroCopy":{"active":true}}';
  int backendModeState = 0;
  bool backendModeApplied = true;
  bool backendModePendingState = false;
  int backendModeCalls = 0;

  @override
  String diagnostics() => diagJson;

  @override
  int get backendMode => backendModeState;

  @override
  bool get backendModePending => backendModePendingState;

  @override
  bool setBackendMode(int mode) {
    backendModeCalls++;
    backendModeState = mode;
    backendModePendingState = !backendModeApplied;
    return backendModeApplied;
  }

  @override
  String? openDialog(String currentPath) => dialogResult;

  bool fullscreenState = false;

  @override
  bool setFullscreen(bool on) {
    fullscreenState = on;
    return on;
  }

  @override
  bool open(String path) {
    openCalls++;
    lastOpened = path;
    if (openOk) hasVideoState = true;
    return openOk;
  }

  @override
  void close() {
    hasVideoState = false;
  }

  bool hasVideoState = false;
  bool srActiveState = false;
  bool srFailedState = false;
  bool srAutoState = false;
  int srTargetState = 0;

  @override
  bool get hasVideo => hasVideoState;

  @override
  void setSR(bool enabled, int targetW) {
    srActiveState = enabled;
    srFailedState = false;
    srAutoState = false;
    srTargetState = enabled ? targetW : 0;
  }

  @override
  bool get srActive => srActiveState;

  @override
  bool get srFailed => srFailedState;

  @override
  int get srTargetW => srTargetState;

  @override
  bool get srAutoDegraded => srAutoState;

  @override
  void play() {
    playCalls++;
    pausedState = false;
    eofState = false;
  }

  @override
  void pause() {
    pauseCalls++;
    pausedState = true;
  }

  @override
  bool get paused => pausedState;

  @override
  void seek(double t) {}

  @override
  void setVolume(double v) {}

  @override
  double get position => 0;

  @override
  double get duration => 2;

  @override
  bool get eof => eofState;

  @override
  int get frameW => 1280;

  @override
  int get frameH => 720;

  @override
  void setBeauty(double smooth, double white) {}

  @override
  void setFilter(int idx, double amt) {}

  @override
  void pump() {}

  @override
  int get textureId => 1;

  @override
  double get fps => 30;

  @override
  int taskCreate(String source, int targetW, String desc, {int quality = 2, int model = 0}) {
    createCalls++;
    lastTaskQuality = quality;
    lastTaskModel = model;
    tasks.add(_FakeTask(status: 0, total: 100, temp: 'C:/out/${tasks.length}.mp4', model: model));
    return tasks.length - 1;
  }

  @override
  void taskStart(int idx) => tasks[idx].status = 1;

  @override
  void taskStop(int idx) => tasks[idx].status = 2;

  @override
  void taskRemove(int idx) => tasks.removeAt(idx);

  @override
  int get taskCount => tasks.length;

  @override
  int taskStatus(int idx) => tasks[idx].status;

  @override
  int taskModel(int idx) => tasks[idx].model;

  @override
  int taskFrames(int idx) => tasks[idx].frames;

  @override
  int taskTotal(int idx) => tasks[idx].total;

  @override
  double taskResumeTime(int idx) => 0;

  @override
  String taskName(int idx) => 'video$idx.mp4';

  @override
  String taskSource(int idx) => 'C:/src/$idx.mp4';

  @override
  String taskTemp(int idx) => tasks[idx].temp;

  @override
  int makePreviews(int count) => 0;

  @override
  int get previewW => 0;

  @override
  int get previewH => 0;

  @override
  Uint8List previewBytes(int idx) => Uint8List(0);
}

class _FakeTask {
  _FakeTask({required this.status, required this.total, required this.temp, this.model = 0});
  int status;
  int model;
  int frames = 0;
  int total;
  String temp;
}

Future<void> _dispose(WidgetTester tester) async {
  await tester.pumpWidget(const SizedBox.shrink());
}

void main() {
  testWidgets('HomeScreen：切标签不重建 PlayerScreen（不重开视频）+ 暂停/恢复',
      (WidgetTester tester) async {
    final engine = FakeEngine();
    await tester.pumpWidget(MaterialApp(home: HomeScreen(engine: engine)));
    await tester.pump();
    expect(engine.openCalls, 0); // 启动不自动打开

    engine.play(); // 模拟播放中
    await tester.pump(const Duration(milliseconds: 20));
    final opened = engine.openCalls;

    await tester.tap(find.text('任务中心'));
    await tester.pump();
    expect(engine.pauseCalls, 1); // 离开暂停（不再有隐藏音频）

    await tester.tap(find.text('播放'));
    await tester.pump();
    expect(engine.pausedState, false); // 返回恢复
    expect(engine.openCalls, opened); // 关键回归：未重开视频
    await _dispose(tester);
  });

  testWidgets('任务中心：新建任务对话框 → 创建并启动', (WidgetTester tester) async {
    final engine = FakeEngine();
    await tester.pumpWidget(MaterialApp(
      home: TaskCenterScreen(engine: engine, onPlay: (_) {}),
    ));
    await tester.pump();
    expect(find.text('超分任务中心'), findsOneWidget);
    expect(find.text('暂无任务'), findsNothing); // 空列表不渲染任务卡

    await tester.tap(find.text('新建任务'));
    await tester.pump();
    expect(find.text('开始任务'), findsOneWidget);

    await tester.tap(find.text('开始任务'));
    await tester.pump();
    expect(engine.createCalls, 1);
    expect(engine.lastTaskQuality, 2);
    expect(engine.lastTaskModel, 0);
    expect(engine.tasks.length, 1);
    expect(engine.tasks.first.status, 1); // 自动启动
    expect(find.text('video0.mp4'), findsOneWidget);
    await _dispose(tester);
  });

  testWidgets('任务中心：浏览按钮用文件框结果填源路径', (WidgetTester tester) async {
    final engine = FakeEngine()..dialogResult = r'D:\clips\a.mp4';
    await tester.pumpWidget(MaterialApp(
      home: TaskCenterScreen(engine: engine, onPlay: (_) {}),
    ));
    await tester.pump();
    await tester.tap(find.text('新建任务'));
    await tester.pump();

    await tester.tap(find.text('浏览'));
    await tester.pump();
    expect(find.text(r'D:\clips\a.mp4'), findsOneWidget);
    await _dispose(tester);
  });

  testWidgets('任务中心：删除需二次确认，取消则不删', (WidgetTester tester) async {
    final engine = FakeEngine();
    engine.tasks.add(_FakeTask(status: 3, total: 60, temp: 'C:/out/done.mp4'));
    await tester.pumpWidget(MaterialApp(
      home: TaskCenterScreen(engine: engine, onPlay: (_) {}),
    ));
    await tester.pump();

    await tester.tap(find.text('删除'));
    await tester.pump();
    expect(find.text('取消'), findsOneWidget); // 确认框出现
    await tester.tap(find.text('取消'));
    await tester.pump();
    expect(engine.tasks.length, 1); // 未删除
    await _dispose(tester);
  });

  testWidgets('任务中心：确认删除后移除任务', (WidgetTester tester) async {
    final engine = FakeEngine();
    engine.tasks.add(_FakeTask(status: 3, total: 60, temp: 'C:/out/done.mp4'));
    await tester.pumpWidget(MaterialApp(
      home: TaskCenterScreen(engine: engine, onPlay: (_) {}),
    ));
    await tester.pump();

    await tester.tap(find.text('删除'));
    await tester.pump();
    await tester.tap(find.widgetWithText(FilledButton, '删除')); // 确认框内的删除
    await tester.pump();
    expect(engine.tasks.length, 0);
    await _dispose(tester);
  });

  testWidgets('任务中心：任务完成后 UI 刷新为「完成」（不再卡在最后一帧）',
      (WidgetTester tester) async {
    final engine = FakeEngine();
    engine.tasks.add(_FakeTask(status: 1, total: 100, temp: 'C:/out/x.mp4'));
    engine.tasks[0].frames = 99;
    await tester.pumpWidget(MaterialApp(
      home: TaskCenterScreen(engine: engine, onPlay: (_) {}),
    ));
    await tester.pump();
    expect(find.text('进行中'), findsOneWidget);

    // 引擎侧完成：状态 1→3（模拟编码线程结束）
    engine.tasks[0].status = 3;
    engine.tasks[0].frames = 100;
    await tester.pump(const Duration(milliseconds: 350)); // 触发 300ms 轮询
    expect(find.text('完成'), findsOneWidget);
    await _dispose(tester);
  });

  testWidgets('任务中心：完成任务的边播放回调产物路径', (WidgetTester tester) async {
    final engine = FakeEngine();
    engine.tasks.add(_FakeTask(status: 3, total: 60, temp: 'C:/out/done.mp4'));
    String? played;
    await tester.pumpWidget(MaterialApp(
      home: TaskCenterScreen(engine: engine, onPlay: (p) => played = p),
    ));
    await tester.pump();

    expect(find.text('完成'), findsOneWidget);
    await tester.tap(find.text('边播放'));
    await tester.pump();
    expect(played, 'C:/out/done.mp4');
    await _dispose(tester);
  });

  testWidgets('播放页：启动不自动打开视频，显示占位提示', (WidgetTester tester) async {
    final engine = FakeEngine();
    await tester.pumpWidget(MaterialApp(home: PlayerScreen(engine: engine)));
    await tester.pump();
    expect(engine.openCalls, 0);
    expect(find.text('未打开视频'), findsOneWidget);
    await _dispose(tester);
  });

  testWidgets('播放页：循环默认关闭', (WidgetTester tester) async {
    final engine = FakeEngine();
    await tester.pumpWidget(MaterialApp(home: PlayerScreen(engine: engine)));
    await tester.pump();
    final btn = tester.widget<IconButton>(find.widgetWithIcon(IconButton, Icons.repeat));
    expect(btn.color, Colors.white54); // 关闭态
    await _dispose(tester);
  });

  testWidgets('播放页：播放/暂停按钮驱动引擎', (WidgetTester tester) async {
    final engine = FakeEngine();
    await tester.pumpWidget(MaterialApp(home: PlayerScreen(engine: engine)));
    await tester.pump();

    expect(engine.openCalls, 0); // 不自动打开
    expect(find.byIcon(Icons.play_arrow), findsOneWidget);

    await tester.tap(find.byIcon(Icons.play_arrow));
    await tester.pump(const Duration(milliseconds: 20)); // 驱动 16ms pump timer 刷新状态
    expect(engine.playCalls, 1);
    expect(engine.pausedState, false);
    expect(find.byIcon(Icons.pause), findsOneWidget);

    await tester.tap(find.byIcon(Icons.pause));
    await tester.pump(const Duration(milliseconds: 20));
    expect(engine.pauseCalls, 1);
    expect(engine.pausedState, true);
    await _dispose(tester);
  });

  testWidgets('播放页：离开标签暂停、返回恢复', (WidgetTester tester) async {
    final engine = FakeEngine();
    final key = GlobalKey<PlayerScreenState>();
    await tester.pumpWidget(MaterialApp(home: PlayerScreen(key: key, engine: engine)));
    await tester.pump();

    engine.play(); // 模拟正在播放
    await tester.pump(const Duration(milliseconds: 20));
    expect(engine.pausedState, false);

    key.currentState!.pauseForLeave(); // 切到任务中心
    await tester.pump();
    expect(engine.pauseCalls, 1);
    expect(engine.pausedState, true);

    key.currentState!.resumeForReturn(); // 切回播放
    await tester.pump();
    expect(engine.pausedState, false);
    await _dispose(tester);
  });

  testWidgets('播放页：本就暂停时离开/返回不改变状态', (WidgetTester tester) async {
    final engine = FakeEngine(); // 默认 paused=true
    final key = GlobalKey<PlayerScreenState>();
    await tester.pumpWidget(MaterialApp(home: PlayerScreen(key: key, engine: engine)));
    await tester.pump();
    expect(engine.pausedState, true);

    key.currentState!.pauseForLeave();
    await tester.pump();
    expect(engine.pauseCalls, 0); // 本就暂停，不调用

    key.currentState!.resumeForReturn();
    await tester.pump();
    expect(engine.playCalls, 0); // 不应自动播放
    await _dispose(tester);
  });

  testWidgets('播放页：显示宽高比取自引擎帧尺寸', (WidgetTester tester) async {
    final engine = FakeEngine();
    final key = GlobalKey<PlayerScreenState>();
    await tester.pumpWidget(MaterialApp(home: PlayerScreen(key: key, engine: engine)));
    await tester.pump();

    key.currentState!.openPath('C:/v.mp4'); // 需先打开才有 Texture
    await tester.pump(const Duration(milliseconds: 20));
    final ar = tester.widget<AspectRatio>(find.byType(AspectRatio));
    expect(ar.aspectRatio, closeTo(1280 / 720, 1e-6));
    await _dispose(tester);
  });

  testWidgets('顶部栏打开影片并显示文件名', (WidgetTester tester) async {
    final engine = FakeEngine();
    await tester.pumpWidget(MaterialApp(home: HomeScreen(engine: engine)));
    await tester.pump();

    await tester.tap(find.byTooltip('打开文件'));
    await tester.pump();
    expect(engine.lastOpened, r'C:\picked\movie.mp4');
    expect(find.text('movie.mp4'), findsOneWidget);
    expect(find.text('30.0 FPS'), findsOneWidget);
    expect(tester.getTopLeft(find.text('Domino')).dx, greaterThan(700));
    await _dispose(tester);
  });

  testWidgets('全屏时隐藏顶部标签栏，退出后恢复', (WidgetTester tester) async {
    final engine = FakeEngine();
    await tester.pumpWidget(MaterialApp(home: HomeScreen(engine: engine)));
    await tester.pump();
    expect(find.text('任务中心'), findsOneWidget);

    await tester.tap(find.byIcon(Icons.fullscreen));
    await tester.pump();
    expect(engine.fullscreenState, true);
    expect(find.text('任务中心'), findsNothing);

    await tester.tap(find.byIcon(Icons.fullscreen_exit));
    await tester.pump();
    expect(engine.fullscreenState, false);
    expect(find.text('任务中心'), findsOneWidget);
    await _dispose(tester);
  });

  testWidgets('全屏控制条 3 秒后自动隐藏', (WidgetTester tester) async {
    final engine = FakeEngine();
    await tester.pumpWidget(MaterialApp(home: PlayerScreen(engine: engine)));
    await tester.pump();

    await tester.tap(find.byIcon(Icons.fullscreen));
    await tester.pump();
    final shown = tester.widget<AnimatedOpacity>(find.byType(AnimatedOpacity).last);
    expect(shown.opacity, 1);

    await tester.pump(const Duration(seconds: 4));
    final hidden = tester.widget<AnimatedOpacity>(find.byType(AnimatedOpacity).last);
    expect(hidden.opacity, 0);
    await _dispose(tester);
  });

  testWidgets('播放页：打开的影片进入播放历史侧栏', (WidgetTester tester) async {
    final engine = FakeEngine();
    await tester.pumpWidget(MaterialApp(home: HomeScreen(engine: engine)));
    await tester.pump();
    await tester.tap(find.byTooltip('打开文件'));
    await tester.pump();

    await tester.tap(find.byIcon(Icons.tune));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 250));
    expect(tester.getTopLeft(find.text('选项')).dx, greaterThan(300));
    expect(tester.getTopLeft(find.text('选项')).dx, lessThan(460));
    await tester.tap(find.text('播放历史'));
    await tester.pump();
    expect(find.text('movie.mp4'), findsWidgets);
    expect(find.textContaining(r'C:\picked\movie.mp4'), findsOneWidget);
    await _dispose(tester);
  });

  testWidgets('播放页：选项侧栏包含美化与播放历史', (WidgetTester tester) async {
    final engine = FakeEngine();
    await tester.pumpWidget(MaterialApp(home: PlayerScreen(engine: engine)));
    await tester.pump();

    await tester.tap(find.byIcon(Icons.tune));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 250));
    expect(tester.getTopLeft(find.text('选项')).dx, greaterThan(300));
    expect(tester.getTopLeft(find.text('选项')).dx, lessThan(460));
    expect(find.text('美化'), findsOneWidget);
    expect(find.text('播放历史'), findsOneWidget);
    expect(find.text('诊断'), findsOneWidget);
    await _dispose(tester);
  });

  testWidgets('播放页：诊断页显示后端与零拷贝状态', (WidgetTester tester) async {
    final engine = FakeEngine();
    await tester.pumpWidget(MaterialApp(home: PlayerScreen(engine: engine)));
    await tester.pump();

    await tester.tap(find.byIcon(Icons.tune));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 250));
    await tester.tap(find.text('诊断'));
    await tester.pump();
    expect(find.textContaining('tensorrt'), findsOneWidget);
    expect(find.textContaining('d3d11'), findsOneWidget);
    await _dispose(tester);
  });

  testWidgets('播放页：诊断页切换后端策略', (WidgetTester tester) async {
    final engine = FakeEngine();
    await tester.pumpWidget(MaterialApp(home: PlayerScreen(engine: engine)));
    await tester.pump();

    await tester.tap(find.byIcon(Icons.tune));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 250));
    await tester.tap(find.text('诊断'));
    await tester.pump();
    await tester.tap(find.byType(DropdownButton<int>));
    await tester.pump(const Duration(milliseconds: 250));
    await tester.tap(find.text('兼容').last);
    await tester.pump(const Duration(milliseconds: 250));
    expect(engine.backendModeCalls, 1);
    expect(engine.backendModeState, 1);
    await _dispose(tester);
  });

  testWidgets('播放页：后端策略待生效提示', (WidgetTester tester) async {
    final engine = FakeEngine()..backendModeApplied = false;
    await tester.pumpWidget(MaterialApp(home: PlayerScreen(engine: engine)));
    await tester.pump();

    await tester.tap(find.byIcon(Icons.tune));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 250));
    await tester.tap(find.text('诊断'));
    await tester.pump();
    await tester.tap(find.byType(DropdownButton<int>));
    await tester.pump(const Duration(milliseconds: 250));
    await tester.tap(find.text('兼容').last);
    await tester.pump(const Duration(milliseconds: 250));
    expect(engine.backendModeCalls, 1);
    expect(find.text('待生效'), findsOneWidget);
    expect(find.textContaining('下次打开视频'), findsOneWidget);
    await _dispose(tester);
  });

  testWidgets('播放页：美化面板滑杆分行、滤镜三列', (WidgetTester tester) async {
    final engine = FakeEngine();
    await tester.pumpWidget(MaterialApp(home: PlayerScreen(engine: engine)));
    await tester.pump();
    await tester.tap(find.byIcon(Icons.tune));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 250));

    expect(find.text('磨皮'), findsOneWidget);
    expect(find.text('美白'), findsOneWidget);
    expect(find.text('滤镜'), findsOneWidget);
    expect(tester.getTopLeft(find.text('磨皮')).dy,
        isNot(tester.getTopLeft(find.text('美白')).dy));
    final grid = tester.widget<GridView>(find.byType(GridView));
    final delegate = grid.gridDelegate as SliverGridDelegateWithFixedCrossAxisCount;
    expect(delegate.crossAxisCount, 3);
    await _dispose(tester);
  });

  testWidgets('播放页：全屏按钮切换引擎全屏', (WidgetTester tester) async {
    final engine = FakeEngine();
    await tester.pumpWidget(MaterialApp(home: PlayerScreen(engine: engine)));
    await tester.pump();
    expect(tester.getCenter(find.byIcon(Icons.fullscreen)).dx, greaterThan(700));

    await tester.tap(find.byIcon(Icons.fullscreen));
    await tester.pump();
    expect(engine.fullscreenState, true);
    expect(find.byIcon(Icons.fullscreen_exit), findsOneWidget);

    await tester.tap(find.byIcon(Icons.fullscreen_exit));
    await tester.pump();
    expect(engine.fullscreenState, false);
    await _dispose(tester);
  });

  testWidgets('播放页：点「打开」弹选择框并播放选中文件', (WidgetTester tester) async {
    final engine = FakeEngine();
    await tester.pumpWidget(MaterialApp(home: HomeScreen(engine: engine)));
    await tester.pump();
    expect(engine.openCalls, 0); // 启动不自动打开

    await tester.tap(find.byTooltip('打开文件'));
    await tester.pump();
    expect(engine.lastOpened, r'C:\picked\movie.mp4'); // 选择了文件 → 播放
    expect(engine.openCalls, 1);
    expect(find.text('movie.mp4'), findsOneWidget); // 顶部栏显示文件名
    await _dispose(tester);
  });

  testWidgets('播放页：取消选择框不做任何事', (WidgetTester tester) async {
    final engine = FakeEngine()..dialogResult = null;
    await tester.pumpWidget(MaterialApp(home: HomeScreen(engine: engine)));
    await tester.pump();

    await tester.tap(find.byTooltip('打开文件'));
    await tester.pump();
    expect(engine.openCalls, 0); // 没有打开
    await _dispose(tester);
  });

  testWidgets('播放页：窄窗口控制条不溢出', (WidgetTester tester) async {
    await tester.binding.setSurfaceSize(const Size(640, 480));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final engine = FakeEngine();
    await tester.pumpWidget(MaterialApp(home: PlayerScreen(engine: engine)));
    await tester.pump();
    expect(tester.takeException(), isNull);
    expect(tester.getSize(find.byType(Slider).last).width, 125);
    await _dispose(tester);
  });

  testWidgets('播放页：宽窗口音量滑杆保持 150', (WidgetTester tester) async {
    await tester.binding.setSurfaceSize(const Size(1280, 720));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final engine = FakeEngine();
    await tester.pumpWidget(MaterialApp(home: PlayerScreen(engine: engine)));
    await tester.pump();
    expect(tester.getSize(find.byType(Slider).last).width, 150);
    await _dispose(tester);
  });

  testWidgets('播放页：窄窗口全屏按钮仍可见可点', (WidgetTester tester) async {
    await tester.binding.setSurfaceSize(const Size(640, 480));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final engine = FakeEngine();
    await tester.pumpWidget(MaterialApp(home: PlayerScreen(engine: engine)));
    await tester.pump();

    final center = tester.getCenter(find.byIcon(Icons.fullscreen));
    expect(center.dx, lessThan(640));
    await tester.tap(find.byIcon(Icons.fullscreen));
    await tester.pump();
    expect(engine.fullscreenState, true);
    await _dispose(tester);
  });

  testWidgets('顶部栏：窄窗口长文件名不溢出', (WidgetTester tester) async {
    await tester.binding.setSurfaceSize(const Size(640, 480));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final engine = FakeEngine()
      ..dialogResult = r'C:\picked\a_very_long_movie_file_name_for_narrow_window.mp4';
    await tester.pumpWidget(MaterialApp(home: HomeScreen(engine: engine)));
    await tester.pump();

    await tester.tap(find.byTooltip('打开文件'));
    await tester.pump();
    expect(tester.takeException(), isNull);
    expect(find.textContaining('a_very_long_movie'), findsOneWidget);
    await _dispose(tester);
  });

  testWidgets('播放页：SR 下拉切换目标并驱动引擎', (WidgetTester tester) async {
    final engine = FakeEngine();
    await tester.pumpWidget(MaterialApp(home: PlayerScreen(engine: engine)));
    await tester.pump();
    expect(find.text('SR 关闭'), findsOneWidget);

    await tester.tap(find.text('SR 关闭'));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));
    await tester.tap(find.text('超分：1080p'));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));
    expect(engine.srActiveState, true);
    expect(engine.srTargetState, 1920);
    expect(find.text('SR 1080p'), findsOneWidget);
    await _dispose(tester);
  });

  testWidgets('播放页：SR 自动降档后标签同步', (WidgetTester tester) async {
    final engine = FakeEngine();
    await tester.pumpWidget(MaterialApp(home: PlayerScreen(engine: engine)));
    await tester.pump();

    engine.srActiveState = true;
    engine.srTargetState = 3840;
    engine.srAutoState = false;
    await tester.pump(const Duration(milliseconds: 50));
    expect(find.text('SR 4K'), findsOneWidget);

    engine.srTargetState = 1920;
    engine.srAutoState = true;
    await tester.pump(const Duration(milliseconds: 50));
    expect(find.text('SR 1080p（自动）'), findsOneWidget);
    await _dispose(tester);
  });

  testWidgets('播放页：打开失败显示 SnackBar', (WidgetTester tester) async {
    final engine = FakeEngine()..openOk = false;
    await tester.pumpWidget(MaterialApp(home: HomeScreen(engine: engine)));
    await tester.pump();
    await tester.tap(find.byTooltip('打开文件'));
    await tester.pump();
    expect(find.text('打开失败：文件不存在或无法解码'), findsOneWidget);
    expect(find.text('未打开文件'), findsOneWidget);
    await _dispose(tester);
  });

  testWidgets('Task center: quality dropdown defaults to high and offers all modes', (WidgetTester tester) async {
    final engine = FakeEngine();
    await tester.pumpWidget(MaterialApp(
      home: TaskCenterScreen(engine: engine, onPlay: (_) {}),
    ));
    await tester.pump();
    await tester.tap(find.byIcon(Icons.add));
    await tester.pump();
    expect(find.text('高质量'), findsOneWidget);
    await tester.tap(find.text('高质量'));
    await tester.pump();
    expect(find.text('快速'), findsOneWidget);
    expect(find.text('平衡'), findsOneWidget);
    expect(find.text('高质量'), findsWidgets);
    await _dispose(tester);
  });

  testWidgets('Task center: high quality model option is passed to engine', (WidgetTester tester) async {
    final engine = FakeEngine();
    await tester.pumpWidget(MaterialApp(
      home: TaskCenterScreen(engine: engine, onPlay: (_) {}),
    ));
    await tester.pump();
    await tester.tap(find.byIcon(Icons.add));
    await tester.pump();
    final dropdowns = find.byType(DropdownButton<int>);
    expect(dropdowns, findsNWidgets(4));
    await tester.tap(dropdowns.at(3)); // 模型档位
    await tester.pump();
    await tester.tap(find.text('高质量').last);
    await tester.pump();
    await tester.tap(find.text('开始任务'));
    await tester.pump();
    expect(engine.createCalls, 1);
    expect(engine.lastTaskModel, 2); // 真人 + 高质量
    await _dispose(tester);
  });

  testWidgets('Task center: content dropdown offers realistic and anime', (WidgetTester tester) async {
    final engine = FakeEngine();
    await tester.pumpWidget(MaterialApp(
      home: TaskCenterScreen(engine: engine, onPlay: (_) {}),
    ));
    await tester.pump();
    await tester.tap(find.byIcon(Icons.add));
    await tester.pump();
    expect(find.text('真人/实拍'), findsOneWidget);
    await tester.tap(find.text('真人/实拍'));
    await tester.pump();
    expect(find.text('动画'), findsOneWidget);
    await tester.tap(find.text('动画'));
    await tester.pump();
    expect(find.text('动画'), findsOneWidget);
    await _dispose(tester);
  });

  testWidgets('任务中心：失败任务显示失败与重试，点击重试重新启动', (WidgetTester tester) async {
    final engine = FakeEngine();
    engine.tasks.add(_FakeTask(status: 4, total: 100, temp: 'C:/out/fail.mp4'));
    await tester.pumpWidget(MaterialApp(
      home: TaskCenterScreen(engine: engine, onPlay: (_) {}),
    ));
    await tester.pump();

    expect(find.text('失败'), findsOneWidget);
    expect(find.text('失败 · 已编码 0 帧'), findsOneWidget);
    expect(find.text('重试'), findsOneWidget);

    await tester.tap(find.text('重试'));
    await tester.pump();
    expect(engine.tasks.first.status, 1);       // 已重新启动
    await tester.pump(const Duration(milliseconds: 350));
    expect(find.text('进行中'), findsOneWidget);
    await _dispose(tester);
  });
}
