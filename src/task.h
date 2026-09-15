#pragma once
#include "encode.h"
#include <string>
#include <vector>
#include <thread>
#include <memory>

// 一个超分任务：源视频 + 输出质量 → 临时 fMP4
// 存储结构：sr-tasks/<task-id>/ 下含 task.mp4（产物）与 meta.tsv（描述信息）
enum TaskStatus { ST_CREATED = 0, ST_RUNNING = 1, ST_PAUSED = 2, ST_DONE = 3, ST_FAILED = 4 };

struct Task {
  std::string id;
  std::string source;
  int targetW = 1920;
  std::string temp;                  // 产物路径（<dir>/<id>/task.mp4）
  std::string folder;                // 任务文件夹（<dir>/<id>/）
  std::string description;           // 附加描述（用户可填）
  std::atomic<int> status{ ST_CREATED };  // 跨线程读写，用 atomic
  long long frames = 0;
  long long totalFrames = 0;   // 源总帧数（持久化，供暂停/出错时显示进度）
  double fps = 0;
  long long createdAt = 0;
  double resumeTime = 0;      // 断点续编：上次编码到的源时间（秒）
  long long startFrame = 0;   // 断点续编：帧号偏移
  std::atomic<bool> fileFree{ false };  // 直播播放已释放 task.mp4 句柄（供续编合并替换文件）
  std::thread thread;         // 编码线程（start 后可 join）
  bool threadJoined = false;
  std::unique_ptr<EncodeProgress> prog;  // 编码进度（atomic 不可拷贝，用指针）

  // RAII：完成后未 join 的线程在此回收，避免 std::thread 析构在 joinable 态触发 terminate
  ~Task() { if (thread.joinable()) thread.join(); }
};

// 任务中心：每任务一个文件夹，元数据存 meta.tsv，持久化到 %APPDATA%/DominoPlayer/sr-tasks
class TaskCenter {
public:
  void init(const std::string& root = "");  // 数据根目录（空=Config 默认 %APPDATA%/DominoPlayer）
  void save(Task* t);        // 写单个任务的 meta.tsv
  Task* create(const char* source, int targetW);
  void start(Task* t);       // 启动编码线程（有检查点则断点续编）
  void stop(Task* t);        // 取消并等待线程结束，记录断点
  void remove(Task* t);      // 停止 + 递归删除任务文件夹
  Task* running();           // 当前运行的任务（无则 nullptr）
  void setPreview(EncodePreviewFn fn, void* ud) { previewFn_ = fn; previewUd_ = ud; }
  // 超分模型目录（前端注入；空则用 Config::defaults()）
  void setModelBase(const std::string& m) { modelBase_ = m; }

  std::vector<std::unique_ptr<Task>>& tasks() { return tasks_; }
  const std::string& dir() { return dir_; }

private:
  std::vector<std::unique_ptr<Task>> tasks_;  // atomic 成员使 Task 不可移动，用指针存储
  std::string dir_;
  std::string modelBase_;   // 超分模型目录（前端注入）
  EncodePreviewFn previewFn_ = nullptr;
  void* previewUd_ = nullptr;
};
