#include "task.h"
#include "config.h"
#include "log.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>

static long long s_idSeq = 0;
static std::string nowId() {
  return "task-" + std::to_string((long long)time(nullptr)) + "-" + std::to_string(s_idSeq++);
}

static std::string taskMetaPath(const std::string& folder) {
  return folder + "/meta.tsv";
}

void TaskCenter::init(const std::string& root) {
  dir_ = root.empty() ? (Config::defaults().dataRoot + "/sr-tasks") : (root + "/sr-tasks");
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
  // 扫描 <dir>/*/meta.tsv
  for (auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
    if (!entry.is_directory(ec)) continue;
    std::string meta = taskMetaPath(entry.path().string());
    FILE* f = fopen(meta.c_str(), "r");
    if (!f) continue;
    char line[8192];
    if (fgets(line, sizeof(line), f)) {
      char id[512], src[4096], desc[2048];
      int tgt = 0, st = 0;
      long long fr = 0, ct = 0, tf = 0, sfr = 0;
      double f2 = 0, rt = 0;
      if (sscanf(line, "%511[^\t]\t%4095[^\t]\t%d\t%d\t%lld\t%lf\t%lld\t%lld\t%lf\t%lld\t%2047[^\n]",
          id, src, &tgt, &st, &fr, &f2, &ct, &tf, &rt, &sfr, desc) >= 7) {
        auto tp = std::make_unique<Task>();
        tp->id = id;
        // 剥离 UTF-8 BOM（若 meta 由带 BOM 的工具写入）
        if (tp->id.size() >= 3 && (unsigned char)tp->id[0] == 0xEF && (unsigned char)tp->id[1] == 0xBB && (unsigned char)tp->id[2] == 0xBF)
          tp->id = tp->id.substr(3);
        tp->source = src;
        tp->targetW = tgt;
        tp->folder = entry.path().string();
        tp->temp = tp->folder + "/task.mp4";
        tp->status.store(st);
        tp->frames = fr;
        tp->fps = f2;
        tp->createdAt = ct;
        tp->totalFrames = tf;
        tp->resumeTime = rt;
        tp->startFrame = sfr;
        tp->description = desc;
        tasks_.push_back(std::move(tp));
      }
    }
    fclose(f);
  }
  for (auto& tp : tasks_) {
    if (tp->status.load() == ST_RUNNING) tp->status.store(ST_PAUSED);  // 上次退出未完成
  }
}

void TaskCenter::save(Task* t) {
  if (!t) return;
  std::error_code ec;
  std::filesystem::create_directories(t->folder, ec);
  FILE* f = fopen(taskMetaPath(t->folder).c_str(), "w");
  if (!f) return;
  fprintf(f, "%s\t%s\t%d\t%d\t%lld\t%lf\t%lld\t%lld\t%lf\t%lld\t%s\n",
    t->id.c_str(), t->source.c_str(), t->targetW, t->status.load(),
    t->frames, t->fps, t->createdAt, t->totalFrames, t->resumeTime, t->startFrame, t->description.c_str());
  fclose(f);
}

Task* TaskCenter::create(const char* source, int targetW) {
  auto tp = std::make_unique<Task>();
  tp->id = nowId();
  tp->source = source;
  tp->targetW = targetW;
  tp->folder = dir_ + "/" + tp->id;
  tp->temp = tp->folder + "/task.mp4";
  tp->createdAt = (long long)time(nullptr);
  tp->status.store(ST_CREATED);
  tp->prog = std::make_unique<EncodeProgress>();
  std::error_code ec;
  std::filesystem::create_directories(tp->folder, ec);
  tasks_.push_back(std::move(tp));
  save(tasks_.back().get());
  return tasks_.back().get();
}

void TaskCenter::start(Task* t) {
  if (!t || t->status.load() == ST_RUNNING) return;
  if (!t->prog) t->prog = std::make_unique<EncodeProgress>();  // 从 meta 加载的任务 prog 为空，需懒创建
  t->status.store(ST_RUNNING);
  t->prog->reset();
  EncodeOptions eo;
  eo.verbose = false;
  eo.prog = t->prog.get();
  eo.preview = previewFn_;
  eo.previewUserdata = previewUd_;
  eo.modelBase = modelBase_.empty() ? Config::defaults().modelBase : modelBase_;
  // 有检查点且主文件存在才断点续编；主文件缺失（如被杀前未写出分片）则从头编码
  bool resume = t->resumeTime > 0.01 && std::filesystem::exists(t->temp);
  std::string partPath = t->temp + ".resume";
  std::string tmpPath = t->temp + ".merged";
  eo.startTime = resume ? t->resumeTime : 0.0;
  eo.startFrame = resume ? t->startFrame : 0;
  // 编码中周期持久化断点（供应用被杀/重启后续编）
  t->prog->checkpointCb = [this, t] {
    t->resumeTime = t->prog->curTime.load();
    t->startFrame = t->prog->frame.load();
    t->frames = t->prog->frame.load();
    t->totalFrames = t->prog->totalFrames.load();
    save(t);
  };
  t->thread = std::thread([this, t, eo, resume, partPath, tmpPath] {
    bool ok = encodeOffline(t->source.c_str(), partPath.c_str(), t->targetW, eo);
    if (ok && resume) {
      // 等待直播播放释放 task.mp4 句柄（否则 remove/rename 被占用而失败，产物停留在旧的部分文件）
      for (int i = 0; i < 50 && !t->fileFree.load(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      ok = mergeToMp4(t->temp.c_str(), partPath.c_str(), tmpPath.c_str());  // 重封装合并
      if (ok) {
        std::error_code ec;
        std::filesystem::remove(t->temp, ec);
        std::filesystem::rename(tmpPath, t->temp, ec);
      }
    } else if (!resume) {
      // 首次编码：直接把产物放到最终路径
      std::error_code ec;
      std::filesystem::rename(partPath, t->temp, ec);
    }
    std::error_code ec;
    std::filesystem::remove(partPath, ec);
    t->frames = t->prog->frame.load();
    t->totalFrames = t->prog->totalFrames.load();
    t->fps = t->prog->fps.load();
    t->status.store(ok ? ST_DONE : ST_PAUSED);
    if (!ok) { t->resumeTime = t->prog->curTime.load(); t->startFrame = t->frames; }  // 保留断点供再次继续
    save(t);
  });
  t->threadJoined = true;
  save(t);
}

void TaskCenter::stop(Task* t) {
  if (!t) return;
  if (t->prog) t->prog->cancel.store(true);
  if (t->threadJoined) {
    if (t->thread.joinable()) t->thread.join();
    t->threadJoined = false;
  }
  if (t->status.load() == ST_RUNNING) t->status.store(ST_PAUSED);
  // 记录断点（prog->frame 为绝对帧号，含续编偏移；线程退出时也会保存）
  if (t->prog) { t->resumeTime = t->prog->curTime.load(); t->startFrame = t->prog->frame.load(); t->frames = t->prog->frame.load(); t->totalFrames = t->prog->totalFrames.load(); }
  save(t);
}

void TaskCenter::remove(Task* t) {
  if (!t) return;
  stop(t);
  // 递归删除任务文件夹（若文件仍被占用则重试几次）
  std::error_code ec;
  for (int i = 0; i < 10; i++) {
    if (std::filesystem::remove_all(t->folder, ec) > 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  for (auto it = tasks_.begin(); it != tasks_.end(); ++it) {
    if ((*it)->id == t->id) { tasks_.erase(it); break; }
  }
}

Task* TaskCenter::running() {
  for (auto& tp : tasks_) if (tp->status.load() == ST_RUNNING) return tp.get();
  return nullptr;
}
