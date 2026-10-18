import 'dart:async';
import 'dart:io';
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter/services.dart';

import 'core/engine.dart';
import 'core/format.dart';
import 'ui/task_center.dart';

void main() {
  runApp(const DominoFlutterApp());
}

class DominoFlutterApp extends StatelessWidget {
  const DominoFlutterApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Domino Player',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
            seedColor: const Color(0xFF52A8F2), brightness: Brightness.dark),
        useMaterial3: true,
      ),
      home: const HomeScreen(),
    );
  }
}

class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key, this.engine});
  /// 可注入引擎（测试用 FakeEngine）；为空时自建 DominoEngine。
  final EngineApi? engine;

  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  DominoEngine? _ownedEngine; // 仅自建时持有，负责释放
  EngineApi? _engine;
  bool _showTasks = false;
  bool _playerFullscreen = false;
  final _playerKey = GlobalKey<PlayerScreenState>();

  @override
  void initState() {
    super.initState();
    if (widget.engine != null) {
      _engine = widget.engine;
    } else {
      // 模型目录相对 exe（构建时由 CMake 拷到 runner 目录），避免硬编码仓库路径
      final exeDir = File(Platform.resolvedExecutable).parent.path;
      _ownedEngine = DominoEngine.create(
        '$exeDir\\assets\\models\\sr\\ncnn',
        '${Platform.environment['APPDATA']}\\DominoPlayer',
      );
      _engine = _ownedEngine;
    }
  }

  @override
  void dispose() {
    _ownedEngine?.close();
    _ownedEngine?.dispose();
    super.dispose();
  }

  void _playFromTasks(String path) {
    // 边播放任务产物：切回播放页并打开（IndexedStack 下 PlayerScreen 常驻，currentState 可用）
    setState(() => _showTasks = false);
    _playerKey.currentState?.openPath(path);
  }

  void _setTab(bool showTasks) {
    if (showTasks == _showTasks) return;
    final player = _playerKey.currentState;
    if (showTasks) {
      player?.exitFullscreen(); // 全屏是播放页的沉浸态，切任务中心前先退出
      player?.pauseForLeave(); // 离开播放页：暂停（避免隐藏后仍有声音）
    } else {
      player?.resumeForReturn(); // 回到播放页：若之前在播放则恢复
    }
    setState(() => _showTasks = showTasks);
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Column(
        children: [
          if (!_playerFullscreen) _buildTopBar(),
          Expanded(
            // 常驻两个页面：切标签不卸载 PlayerScreen（否则会重开默认视频、丢进度、音频继续）
            child: IndexedStack(
              index: _showTasks ? 1 : 0,
              children: [
                PlayerScreen(
                  key: _playerKey,
                  engine: _engine!,
                  onFullscreenChanged: (on) {
                    if (mounted && _playerFullscreen != on) {
                      setState(() => _playerFullscreen = on);
                    }
                  },
                ),
                TaskCenterScreen(
                  engine: _engine!,
                  onPlay: _playFromTasks,
                  visible: _showTasks,
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildTopBar() {
    return Container(
      height: 44,
      padding: const EdgeInsets.symmetric(horizontal: 8),
      color: const Color(0xFF22242E),
      child: Row(
        children: [
          SegmentedButton<bool>(
            segments: const [
              ButtonSegment(value: false, label: Text('播放')),
              ButtonSegment(value: true, label: Text('任务中心')),
            ],
            selected: {_showTasks},
            onSelectionChanged: (s) => _setTab(s.first),
          ),
          const Spacer(),
          Text('Domino', style: TextStyle(color: Colors.white70, fontSize: 13)),
        ],
      ),
    );
  }
}

class PlayerScreen extends StatefulWidget {
  const PlayerScreen({super.key, required this.engine, this.onFullscreenChanged});
  final EngineApi engine;
  final ValueChanged<bool>? onFullscreenChanged;

  @override
  State<PlayerScreen> createState() => PlayerScreenState();
}

class PlayerScreenState extends State<PlayerScreen> with SingleTickerProviderStateMixin {
  static const _filters = filterNames;

  late final Ticker _ticker; // vsync 驱动（比固定 Timer 更平滑）
  bool _opening = false;
  bool _loop = false; // 播到结尾自动重播（默认关）
  bool _hasVideo = false; // 是否已打开视频（决定显示视频还是占位提示）
  bool _showBeauty = false; // 是否呼出美颜/滤镜面板（浮层，不占布局）
  bool _fullscreen = false;
  bool _fsControlsVisible = false;
  Timer? _fsHideTimer;
  final FocusNode _fsFocus = FocusNode();

  String _currentPath = ''; // 当前视频路径（不常显，仅作对话框初始目录与文件名）
  double _smooth = 0.0;
  double _white = 0.0;
  double _volume = 1.0;
  int _filterIdx = 0;
  double _position = 0.0;
  double _duration = 0.0;
  bool _paused = true;
  double _seekDrag = -1.0; // 拖动中的临时位置（-1=未拖动）

  // 视频帧尺寸（用于显示宽高比；0 时回退 16:9）
  int _videoW = 0;
  int _videoH = 0;
  // 切标签离开时的播放状态（用于返回时恢复）
  bool _wasPlayingBeforeLeave = false;

  // FPS 显示（引擎侧真实呈现帧率）
  double _fps = 0;

  // 实时超分：_srTargetW 是用户档位；_srActualTargetW 是自动降档后的实际档位
  int _srTargetW = 0;
  int _srActualTargetW = 0;
  bool _srFailed = false;
  bool _srAutoDegraded = false;

  // 滤镜缩略图（按需生成：打开/美颜变化时，避免播放中闪动）
  List<ui.Image> _previewImgs = [];
  bool _previewDirty = true;
  bool _previewBusy = false;

  @override
  void initState() {
    super.initState();
    // 不自动打开视频：由用户点「打开」选择文件
    // 用 Ticker 在每次 vsync 驱动引擎（呈现对齐刷新，比 16ms Timer 平滑）
    _ticker = createTicker((_) => _pump())..start();
  }

  @override
  void dispose() {
    _fsHideTimer?.cancel();
    _fsFocus.dispose();
    if (_fullscreen) widget.engine.setFullscreen(false);
    _ticker.dispose();
    for (final im in _previewImgs) {
      im.dispose();
    }
    super.dispose();
  }

  Future<void> _refreshPreviews() async {
    final e = widget.engine;
    if (e.textureId <= 0 || _previewBusy) return;
    _previewBusy = true;
    final n = e.makePreviews(_filters.length);
    if (n != _filters.length || e.previewW <= 0) {
      _previewBusy = false;
      return;
    }
    final w = e.previewW;
    final h = e.previewH;
    final imgs = <ui.Image>[];
    try {
      for (int i = 0; i < n; i++) {
        final rgba = e.previewBytes(i);
        final img = await _decodePixels(rgba, w, h);
        imgs.add(img);
      }
    } catch (_) {
      for (final im in imgs) {
        im.dispose();
      }
      _previewBusy = false;
      return;
    }
    _previewBusy = false;
    if (!mounted) {
      for (final im in imgs) {
        im.dispose();
      }
      return;
    }
    setState(() {
      _previewDirty = false; // 成功生成后才清，失败保留以便下 tick 重试
      final old = _previewImgs;
      _previewImgs = imgs;
      for (final im in old) {
        im.dispose();
      }
    });
  }

  Future<ui.Image> _decodePixels(Uint8List rgba, int w, int h) {
    final c = Completer<ui.Image>();
    ui.decodeImageFromPixels(rgba, w, h, ui.PixelFormat.rgba8888, (img) {
      c.complete(img);
    });
    return c.future;
  }

  void _pump() {
    final e = widget.engine;
    if (_opening) return;
    e.pump();
    // EOF：循环模式自动重播（引擎已暂停在结尾）
    if (_loop && e.eof) {
      e.play();
    }
    final pos = e.position;
    final dur = e.duration;
    final paused = e.paused;
    final vw = e.frameW;
    final vh = e.frameH;
    final hasVideo = e.hasVideo;
    if ((pos - _position).abs() > 0.01 ||
        (dur - _duration).abs() > 0.01 ||
        paused != _paused ||
        vw != _videoW ||
        vh != _videoH ||
        hasVideo != _hasVideo) {
      setState(() {
        _position = pos;
        _duration = dur;
        _paused = paused;
        _videoW = vw;
        _videoH = vh;
        _hasVideo = hasVideo;
      });
    }
    // FPS：取引擎真实呈现帧率
    final fps = e.fps;
    if (fps != _fps) {
      setState(() => _fps = fps);
    }
    final srFailed = _srActualTargetW > 0 && e.srFailed;
    if (srFailed != _srFailed) {
      setState(() => _srFailed = srFailed);
    }
    // 自动降档由引擎决定；只同步实际档位，保留用户档位供下次打开/重试
    final srTarget = e.srTargetW;
    final srAuto = e.srAutoDegraded;
    if (srTarget != _srActualTargetW || srAuto != _srAutoDegraded) {
      setState(() {
        _srActualTargetW = srTarget;
        _srAutoDegraded = srAuto;
      });
    }
    // 滤镜缩略图：按需生成（打开后首帧 / 美颜变化）；成功后才清 dirty（失败保留以便重试）
    if (_previewDirty && vw > 0 && !_previewBusy) {
      _refreshPreviews();
    }
  }

  /// 打开指定路径（供"边播放"等外部调用）。
  void openPath(String path) {
    _currentPath = path;
    _open();
  }

  /// 切到其他标签：暂停（避免隐藏后仍有声音），记住原状态。
  void pauseForLeave() {
    _wasPlayingBeforeLeave = !widget.engine.paused;
    if (_wasPlayingBeforeLeave) {
      widget.engine.pause();
      setState(() => _paused = true);
    }
  }

  /// 切回播放页：若离开前在播放则恢复。
  void resumeForReturn() {
    if (_wasPlayingBeforeLeave) {
      widget.engine.play();
      setState(() => _paused = false);
    }
    _wasPlayingBeforeLeave = false;
  }

  /// 「打开」：弹出系统文件选择框，选择后播放。
  Future<void> _pickAndOpen() async {
    final e = widget.engine;
    final path = e.openDialog(_currentPath);
    if (path == null || path.isEmpty) return;
    setState(() => _currentPath = path);
    _open();
  }

  /// 全屏切换。
  void _toggleFullscreen() => _setFullscreen(!_fullscreen);

  void _setFullscreen(bool on) {
    if (_fullscreen == on) return;
    _fsHideTimer?.cancel();
    final actual = widget.engine.setFullscreen(on);
    setState(() {
      _fullscreen = actual;
      _fsControlsVisible = actual;
    });
    widget.onFullscreenChanged?.call(actual);
    if (actual) {
      _scheduleFsHide();
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted && _fullscreen) _fsFocus.requestFocus();
      });
    } else {
      _fsFocus.unfocus();
    }
  }

  /// 供 HomeScreen 在切换任务中心/销毁前退出全屏。
  void exitFullscreen() {
    if (_fullscreen) _setFullscreen(false);
  }

  void _scheduleFsHide() {
    _fsHideTimer?.cancel();
    if (!_fullscreen || _showBeauty) return;
    _fsHideTimer = Timer(const Duration(seconds: 3), () {
      if (!mounted || !_fullscreen || _showBeauty) return;
      setState(() => _fsControlsVisible = false);
    });
  }

  void _showFsControls() {
    if (!_fullscreen) return;
    if (!_fsControlsVisible) setState(() => _fsControlsVisible = true);
    _scheduleFsHide();
  }

  KeyEventResult _onFullscreenKey(FocusNode node, KeyEvent event) {
    if (_fullscreen && event is KeyDownEvent &&
        event.logicalKey == LogicalKeyboardKey.escape) {
      _setFullscreen(false);
      return KeyEventResult.handled;
    }
    return KeyEventResult.ignored;
  }

  void _open() {
    final e = widget.engine;
    if (_currentPath.isEmpty) {
      _pickAndOpen();
      return;
    }
    setState(() {
      _opening = true;
      _srFailed = false;
    });
    e.close();
    e.setSR(_srTargetW > 0, _srTargetW);
    final ok = e.open(_currentPath);
    setState(() {
      _opening = false;
      _hasVideo = ok;
      if (ok) {
        _position = 0;
        _duration = e.duration;
        _paused = e.paused;
        _seekDrag = -1.0;
        for (final im in _previewImgs) {
          im.dispose();
        }
        _previewImgs = [];
        _previewDirty = true;
        _videoW = 0;
        _videoH = 0;
      }
    });
    if (!ok && mounted) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('打开失败：文件不存在或无法解码')),
      );
    }
  }

  String _fmt(double s) => formatTime(s);

  String get _srLabel {
    final auto = _srAutoDegraded ? '（自动）' : '';
    if (_srActualTargetW <= 0) return 'SR 关闭$auto';
    if (_srActualTargetW >= 3840) return 'SR 4K$auto';
    if (_srActualTargetW >= 2560) return 'SR 2K$auto';
    return 'SR 1080p$auto';
  }

  @override
  Widget build(BuildContext context) {
    final e = widget.engine;
    final video = _buildVideoArea(e);
    if (!_fullscreen) {
      return Scaffold(
        backgroundColor: Colors.black,
        body: Column(
          children: [
            Expanded(child: video),
            _buildControlBar(e),
          ],
        ),
      );
    }

    // 沉浸式全屏：只显示视频；鼠标移动时淡入控制条，3 秒后自动隐藏。
    return Scaffold(
      backgroundColor: Colors.black,
      body: Focus(
        focusNode: _fsFocus,
        onKeyEvent: _onFullscreenKey,
        child: MouseRegion(
          cursor: _fsControlsVisible ? MouseCursor.defer : SystemMouseCursors.none,
          onHover: (_) => _showFsControls(),
          child: Listener(
            behavior: HitTestBehavior.translucent,
            onPointerDown: (_) => _showFsControls(),
            child: Stack(
              fit: StackFit.expand,
              children: [
                video,
                Positioned(
                  left: 0,
                  right: 0,
                  bottom: 0,
                  child: AnimatedOpacity(
                    opacity: _fsControlsVisible ? 1 : 0,
                    duration: const Duration(milliseconds: 180),
                    child: IgnorePointer(
                      ignoring: !_fsControlsVisible,
                      child: _buildControlBar(e),
                    ),
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }

  Widget _buildVideoArea(EngineApi e) {
    final texId = e.textureId;
    return Stack(
      children: [
        if (_hasVideo && texId > 0)
          Positioned.fill(
            child: Center(
              child: AspectRatio(
                aspectRatio: (_videoW > 0 && _videoH > 0) ? _videoW / _videoH : 16 / 9,
                child: Texture(textureId: texId, filterQuality: FilterQuality.medium),
              ),
            ),
          )
        else if (!_opening)
          Center(
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                Icon(Icons.movie_outlined, size: 64, color: Colors.white24),
                const SizedBox(height: 12),
                const Text('未打开视频', style: TextStyle(color: Colors.white54)),
                const SizedBox(height: 4),
                const Text('点击下方「打开」选择文件',
                    style: TextStyle(color: Colors.white38, fontSize: 12)),
                const SizedBox(height: 12),
                FilledButton.icon(
                  onPressed: _pickAndOpen,
                  icon: const Icon(Icons.folder_open),
                  label: const Text('打开文件'),
                ),
              ],
            ),
          ),
        if (_opening) const Center(child: CircularProgressIndicator()),
        // FPS 移到右上角；全屏控制条隐藏时一并隐藏，保持沉浸感。
        if (_hasVideo && (!_fullscreen || _fsControlsVisible))
          Positioned(
            top: 12,
            right: 12,
            child: Container(
              padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
              decoration: BoxDecoration(
                color: Colors.black54,
                borderRadius: BorderRadius.circular(6),
              ),
              child: Text('${_fps} FPS', style: const TextStyle(fontSize: 13, color: Colors.white70)),
            ),
          ),
        if (_showBeauty)
          Positioned(
            left: 8,
            right: 8,
            bottom: 8,
            child: _buildBeautyPanel(e),
          ),
      ],
    );
  }

  Widget _buildControlBar(EngineApi e) {
    final shownPos = _seekDrag >= 0 ? _seekDrag : _position;
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
      color: const Color(0xFF1B1D25),
      child: Column(
        children: [
          LayoutBuilder(builder: (context, constraints) {
            return SingleChildScrollView(
              scrollDirection: Axis.horizontal,
              child: SizedBox(
                width: constraints.maxWidth < 760 ? 760.0 : constraints.maxWidth,
                child: Row(
                  children: [
              IconButton(
                tooltip: _paused ? '播放' : '暂停',
                icon: Icon(_paused ? Icons.play_arrow : Icons.pause),
                onPressed: () {
                  if (_paused) {
                    e.play();
                  } else {
                    e.pause();
                  }
                },
              ),
              IconButton(
                tooltip: _loop ? '循环播放：开' : '循环播放：关',
                icon: const Icon(Icons.repeat),
                color: _loop ? Colors.lightBlueAccent : Colors.white54,
                onPressed: () => setState(() => _loop = !_loop),
              ),
              IconButton(
                tooltip: '美颜 / 滤镜',
                icon: const Icon(Icons.auto_fix_high),
                color: _showBeauty ? Colors.lightBlueAccent : Colors.white54,
                onPressed: () {
                  setState(() => _showBeauty = !_showBeauty);
                  if (_showBeauty) {
                    _previewDirty = true; // 呼出时刷新缩略图
                    _fsHideTimer?.cancel();
                  } else {
                    _scheduleFsHide();
                  }
                },
              ),
              IconButton(
                tooltip: _fullscreen ? '退出全屏' : '全屏',
                icon: Icon(_fullscreen ? Icons.fullscreen_exit : Icons.fullscreen),
                color: _fullscreen ? Colors.lightBlueAccent : Colors.white54,
                onPressed: _toggleFullscreen,
              ),
              Flexible(
                child: PopupMenuButton<int>(
                tooltip: '实时超分',
                initialValue: _srTargetW,
                onSelected: (v) {
                  setState(() {
                    _srTargetW = v;
                    _srActualTargetW = v;
                    _srFailed = false;
                    _srAutoDegraded = false;
                  });
                  e.setSR(v > 0, v);
                },
                itemBuilder: (ctx) => const [
                  PopupMenuItem(value: 0, child: Text('超分：关闭')),
                  PopupMenuItem(value: 1920, child: Text('超分：1080p')),
                  PopupMenuItem(value: 2560, child: Text('超分：2K')),
                  PopupMenuItem(value: 3840, child: Text('超分：4K')),
                ],
                child: Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 6),
                  child: Text(
                    _srFailed ? 'SR 失败' : _srLabel,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: TextStyle(
                      fontSize: 12,
                      color: _srFailed
                          ? Colors.redAccent
                          : _srAutoDegraded
                              ? Colors.orangeAccent
                              : (_srActualTargetW > 0 ? Colors.lightBlueAccent : Colors.white54),
                    ),
                  ),
                ),
                ),
              ),
              const SizedBox(width: 8),
              if (_currentPath.isNotEmpty)
                ConstrainedBox(
                  constraints: const BoxConstraints(maxWidth: 120),
                  child: Text(
                    baseName(_currentPath),
                    overflow: TextOverflow.ellipsis,
                    style: const TextStyle(fontSize: 12, color: Colors.white60),
                  ),
                ),
              const SizedBox(width: 8),
              Text('${_fmt(shownPos)} / ${_fmt(_duration)}'),
              const SizedBox(width: 8),
              Expanded(
                child: Slider(
                  value: _duration > 0 ? (shownPos / _duration).clamp(0.0, 1.0) : 0.0,
                  onChanged: (v) => setState(() => _seekDrag = v * _duration),
                  onChangeEnd: (v) {
                    e.seek(v * _duration);
                    setState(() => _seekDrag = -1.0);
                  },
                ),
              ),
              const Icon(Icons.volume_up, size: 18),
              const SizedBox(width: 4),
              SizedBox(
                width: 96,
                child: Slider(
                  value: _volume,
                  onChanged: (v) {
                    setState(() => _volume = v);
                    e.setVolume(v);
                  },
                ),
              ),
              Text('${(_volume * 100).round()}%'),
              IconButton(
                tooltip: '打开文件',
                icon: const Icon(Icons.folder_open),
                onPressed: _pickAndOpen,
              ),
                  ],
                ),
              ),
            );
          }),
        ],
      ),
    );
  }

  Widget _buildBeautyPanel(EngineApi e) {
    return Container(
      padding: const EdgeInsets.fromLTRB(12, 4, 12, 10),
      decoration: BoxDecoration(
        color: const Color(0xF014151C),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: Colors.white12),
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Row(
            children: [
              const Icon(Icons.auto_fix_high, size: 18, color: Colors.white70),
              const SizedBox(width: 6),
              const Text('美颜 / 滤镜', style: TextStyle(fontWeight: FontWeight.bold)),
              const Spacer(),
              IconButton(
                tooltip: '关闭',
                icon: const Icon(Icons.close, size: 18),
                onPressed: () {
                  setState(() => _showBeauty = false);
                  _scheduleFsHide();
                },
              ),
            ],
          ),
          Row(
            children: [
              const Text('磨皮'),
              Expanded(
                child: Slider(
                  value: _smooth,
                  onChanged: (v) {
                    setState(() => _smooth = v);
                    e.setBeauty(v, _white);
                  },
                  onChangeEnd: (_) => _previewDirty = true,
                ),
              ),
              Text('${(_smooth * 100).round()}%'),
              const SizedBox(width: 16),
              const Text('美白'),
              Expanded(
                child: Slider(
                  value: _white,
                  onChanged: (v) {
                    setState(() => _white = v);
                    e.setBeauty(_smooth, v);
                  },
                  onChangeEnd: (_) => _previewDirty = true,
                ),
              ),
              Text('${(_white * 100).round()}%'),
            ],
          ),
          const SizedBox(height: 4),
          SingleChildScrollView(
            scrollDirection: Axis.horizontal,
            child: Row(
              children: [
                const Text('滤镜'),
                const SizedBox(width: 8),
                ...List.generate(_filters.length, (i) {
                  final preview = i < _previewImgs.length ? _previewImgs[i] : null;
                  return Padding(
                    padding: const EdgeInsets.only(right: 8),
                    child: InkWell(
                      borderRadius: BorderRadius.circular(8),
                      onTap: () {
                        setState(() => _filterIdx = i);
                        e.setFilter(i, 1.0);
                      },
                      child: Column(
                        mainAxisSize: MainAxisSize.min,
                        children: [
                          Container(
                            width: 96,
                            height: 54,
                            decoration: BoxDecoration(
                              borderRadius: BorderRadius.circular(8),
                              border: Border.all(
                                color: _filterIdx == i
                                    ? Theme.of(context).colorScheme.primary
                                    : Colors.white24,
                                width: 2,
                              ),
                            ),
                            child: ClipRRect(
                              borderRadius: BorderRadius.circular(6),
                              child: preview != null
                                  ? RawImage(
                                      image: preview,
                                      fit: BoxFit.cover,
                                      filterQuality: FilterQuality.low,
                                    )
                                  : const ColoredBox(color: Color(0xFF1E2029)),
                            ),
                          ),
                          const SizedBox(height: 2),
                          Text(_filters[i], style: const TextStyle(fontSize: 11)),
                        ],
                      ),
                    ),
                  );
                }),
              ],
            ),
          ),
        ],
      ),
    );
  }
}
