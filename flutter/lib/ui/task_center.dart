import 'dart:async';

import 'package:flutter/material.dart';

import '../core/engine.dart';
import '../core/format.dart';

/// 任务中心：离线超分任务列表 + 新建/开始/继续/停止/边播放/删除
class TaskCenterScreen extends StatefulWidget {
  const TaskCenterScreen({super.key, required this.engine, this.onPlay, this.visible = true});
  final EngineApi engine;

  /// 点击"边播放"时回调产物路径（由外层切换回播放页并打开）。
  final void Function(String path)? onPlay;

  /// 是否可见（IndexedStack 常驻时用于门控轮询，避免隐藏页空转）。
  final bool visible;

  @override
  State<TaskCenterScreen> createState() => _TaskCenterScreenState();
}

class _TaskCenterScreenState extends State<TaskCenterScreen> {
  static const _targets = ['720p', '1080p', '2K', '4K'];
  static const _targetW = [1280, 1920, 2560, 3840];
  static const _srQualities = ['快速', '平衡', '高质量'];
  static const _srQuality = [0, 1, 2];
  static const _contents = ['真人/实拍', '动画'];
  static const _modelTiers = ['标准', '高质量'];

  int _count = 0;
  List<int> _lastStatus = const [];
  List<int> _lastFrames = const [];
  bool _showNew = false;
  final _srcCtrl = TextEditingController(); // 源路径（默认空，由「浏览」或手动填写）
  final _descCtrl = TextEditingController();
  int _targetIdx = 1;
  int _qualityIdx = 2;
  int _contentIdx = 0;
  int _tierIdx = 0;

  int get _modelIndex => (_contentIdx == 1 ? 1 : 0) + (_tierIdx == 1 ? 2 : 0);

  static String _modelLabel(int model) {
    switch (model) {
      case 1: return '动画 · animevideov3 x4';
      case 2: return '真人 · x4plus';
      case 3: return '动画 · x4plus-anime';
      default: return '真人 · General x4';
    }
  }
  Timer? _timer;

  @override
  void initState() {
    super.initState();
    _refresh();
    _timer = Timer.periodic(const Duration(milliseconds: 300), (_) => _refresh());
  }

  @override
  void dispose() {
    _timer?.cancel();
    _srcCtrl.dispose();
    _descCtrl.dispose();
    super.dispose();
  }

  @override
  void didUpdateWidget(covariant TaskCenterScreen oldWidget) {
    super.didUpdateWidget(oldWidget);
    // 由隐藏变为可见时立即刷新一次
    if (widget.visible && !oldWidget.visible) _refresh();
  }

  void _refresh() {
    if (!widget.visible) return; // 隐藏时不轮询
    final e = widget.engine;
    final n = e.taskCount;
    final status = List<int>.generate(n, e.taskStatus);
    final frames = List<int>.generate(n, e.taskFrames);
    // 任何任务数量/状态/进度变化都要刷新（尤其是 运行中→完成/失败 的终态切换）
    bool changed = n != _lastStatus.length;
    if (!changed) {
      for (int i = 0; i < n; i++) {
        if (status[i] != _lastStatus[i] || frames[i] != _lastFrames[i]) {
          changed = true;
          break;
        }
      }
    }
    if (changed) {
      setState(() {
        _count = n;
        _lastStatus = status;
        _lastFrames = frames;
      });
    }
  }

  void _newTask() {
    final ok = widget.engine.taskCreate(
        _srcCtrl.text, _targetW[_targetIdx], _descCtrl.text,
        quality: _srQuality[_qualityIdx], model: _modelIndex);
    if (ok >= 0) {
      widget.engine.taskStart(ok);
      setState(() => _showNew = false);
      _refresh();
    } else {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('创建任务失败：源文件无法打开')),
      );
    }
  }

  void _browseSource() {
    final path = widget.engine.openDialog(_srcCtrl.text);
    if (path != null && path.isNotEmpty) {
      setState(() => _srcCtrl.text = path);
    }
  }

  Future<void> _confirmRemove(int i) async {
    final e = widget.engine;
    final yes = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('删除任务'),
        content: Text('确定删除任务「${e.taskName(i)}」？其输出与临时文件将一并删除，不可恢复。'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('取消')),
          FilledButton(
            onPressed: () => Navigator.pop(ctx, true),
            child: const Text('删除'),
          ),
        ],
      ),
    );
    if (yes == true) {
      if (!mounted) return;
      e.taskRemove(i);
      _refresh();
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: const Color(0xFF1B1D25),
      body: Stack(
        children: [
          Column(
            children: [
              // 顶栏
              Container(
                height: 46,
                padding: const EdgeInsets.symmetric(horizontal: 12),
                color: const Color(0xFF283046),
                child: Row(
                  children: [
                    const Text('超分任务中心', style: TextStyle(fontWeight: FontWeight.bold)),
                    const Spacer(),
                    FilledButton.icon(
                      onPressed: () => setState(() => _showNew = true),
                      icon: const Icon(Icons.add),
                      label: const Text('新建任务'),
                    ),
                  ],
                ),
              ),
              // 任务列表
              Expanded(
                child: ListView.builder(
                  padding: const EdgeInsets.all(12),
                  itemCount: _count,
                  itemBuilder: (_, i) => _buildTaskCard(i),
                ),
              ),
            ],
          ),
          // 新建任务对话框（覆盖层）
          if (_showNew) _buildNewDialog(),
        ],
      ),
    );
  }

  Widget _buildNewDialog() {
    return Container(
      color: Colors.black54,
      alignment: Alignment.center,
      child: Container(
        width: 460,
        padding: const EdgeInsets.all(16),
        decoration: BoxDecoration(
          color: const Color(0xFF22242E),
          borderRadius: BorderRadius.circular(10),
        ),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                const Text('新建任务', style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold)),
                const SizedBox(height: 12),
                Row(
                  children: [
                    Expanded(
                      child: TextField(
                        controller: _srcCtrl,
                        decoration: const InputDecoration(labelText: '源视频路径'),
                      ),
                    ),
                    const SizedBox(width: 8),
                    OutlinedButton.icon(
                      onPressed: _browseSource,
                      icon: const Icon(Icons.folder_open, size: 18),
                      label: const Text('浏览'),
                    ),
                  ],
                ),
                const SizedBox(height: 8),
                Row(
                  children: [
                    const Text('输出分辨率'),
                    const SizedBox(width: 8),
                    DropdownButton<int>(
                      value: _targetIdx,
                      items: List.generate(
                          _targets.length,
                          (i) => DropdownMenuItem(value: i, child: Text(_targets[i]))),
                      onChanged: (v) => setState(() => _targetIdx = v ?? 1),
                    ),
                    const SizedBox(width: 20),
                    const Text('超分质量'),
                    const SizedBox(width: 8),
                    DropdownButton<int>(
                      value: _qualityIdx,
                      items: List.generate(
                          _srQualities.length,
                          (i) => DropdownMenuItem(value: i, child: Text(_srQualities[i]))),
                      onChanged: (v) => setState(() => _qualityIdx = v ?? 2),
                    ),
                  ],
                ),
                const SizedBox(height: 8),
                Row(
                  children: [
                    const Text('内容类型'),
                    const SizedBox(width: 8),
                    DropdownButton<int>(
                      value: _contentIdx,
                      items: List.generate(
                          _contents.length,
                          (i) => DropdownMenuItem(value: i, child: Text(_contents[i]))),
                      onChanged: (v) => setState(() => _contentIdx = v ?? 0),
                    ),
                    const SizedBox(width: 20),
                    const Text('模型档位'),
                    const SizedBox(width: 8),
                    DropdownButton<int>(
                      value: _tierIdx,
                      items: List.generate(
                          _modelTiers.length,
                          (i) => DropdownMenuItem(value: i, child: Text(_modelTiers[i]))),
                      onChanged: (v) => setState(() => _tierIdx = v ?? 0),
                    ),
                  ],
                ),
                const SizedBox(height: 4),
                const Text('高质量：真人 x4plus / 动画 x4plus-anime，速度更慢',
                    style: TextStyle(fontSize: 11, color: Colors.white38)),
                const SizedBox(height: 8),
                TextField(
                  controller: _descCtrl,
                  decoration: const InputDecoration(labelText: '备注（可选）'),
                ),
                const SizedBox(height: 16),
                Row(
                  mainAxisAlignment: MainAxisAlignment.end,
                  children: [
                    TextButton(onPressed: () => setState(() => _showNew = false), child: const Text('取消')),
                    const SizedBox(width: 8),
                    FilledButton(onPressed: _newTask, child: const Text('开始任务')),
                  ],
                ),
              ],
            ),
          ),
    );
  }

  Widget _buildTaskCard(int i) {
    final e = widget.engine;
    final status = e.taskStatus(i);
    final frames = e.taskFrames(i);
    final total = e.taskTotal(i);
    final resume = e.taskResumeTime(i);
    final name = e.taskName(i);
    final temp = e.taskTemp(i);
    final model = e.taskModel(i);
    final progress = total > 0 ? (frames / total).clamp(0.0, 1.0) : 0.0;

    return Card(
      color: const Color(0xFF262833),
      margin: const EdgeInsets.only(bottom: 10),
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Expanded(
                  child: Text(name, style: const TextStyle(fontWeight: FontWeight.bold)),
                ),
                Text(taskStatusName(status),
                    style: TextStyle(color: _statusColor(status))),
              ],
            ),
            const SizedBox(height: 2),
            Text(_modelLabel(model),
                style: const TextStyle(fontSize: 11, color: Colors.white38)),
            const SizedBox(height: 8),
            LinearProgressIndicator(value: progress, minHeight: 8, borderRadius: BorderRadius.circular(4)),
            const SizedBox(height: 4),
            Text(
              status == 3
                  ? '完成 · ${frames} 帧'
                  : status == 4
                      ? '失败 · 已编码 ${frames} 帧'
                      : status == 2 && resume > 0.01
                          ? '已编码 ${frames} 帧 · 断点 ${resume.toStringAsFixed(1)}s'
                          : '进度 ${frames} / ${total} 帧',
              style: const TextStyle(fontSize: 13, color: Colors.white70),
            ),
            const SizedBox(height: 8),
            Row(
              children: [
                if (status == 0 || status == 2 || status == 4)
                  _action(status == 4 ? '重试' : '开始', () => e.taskStart(i))
                else if (status == 1)
                  _action('停止', () => e.taskStop(i)),
                const SizedBox(width: 8),
                if (status == 3 || status == 2)
                  _action('边播放', () => widget.onPlay?.call(temp)),
                const Spacer(),
                _action('删除', () => _confirmRemove(i)),
              ],
            ),
          ],
        ),
      ),
    );
  }

  Widget _action(String label, VoidCallback onTap) {
    return OutlinedButton(onPressed: onTap, child: Text(label));
  }

  Color _statusColor(int status) {
    switch (status) {
      case 0:
        return Colors.white54; // 未开始
      case 1:
        return Colors.greenAccent; // 进行中
      case 2:
        return Colors.amber; // 未完成（有断点）
      case 3:
        return Colors.lightBlueAccent; // 完成
      default:
        return Colors.redAccent; // 失败 / 非法状态
    }
  }
}
