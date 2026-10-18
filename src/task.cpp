#include "task.h"
#include "config.h"
#include "log.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <cstdint>
#include <algorithm>
#include <filesystem>

static long long s_idSeq = 0;
static std::string nowId() {
  return "task-" + std::to_string((long long)time(nullptr)) + "-" + std::to_string(s_idSeq++);
}

static std::string taskMetaPath(const std::string& folder) {
  return folder + "/meta.tsv";
}

static SRInputQuality qualityFromInt(int v) {
  switch (v) {
    case 0: return SRInputQuality::Fast;
    case 1: return SRInputQuality::Balanced;
    case 2: return SRInputQuality::FullSource;
    default: return SRInputQuality::Fast;   // 旧 meta 没有质量字段：沿用旧 target/4 行为
  }
}

static bool pathExists(const std::string& p) {
  std::error_code ec;
  return std::filesystem::exists(p, ec);
}

static std::uintmax_t pathSize(const std::string& p) {
  std::error_code ec;
  auto n = std::filesystem::file_size(p, ec);
  return ec ? 0 : n;
}

// 探测媒体时长（秒）；失败/未知返回 0。用于把断点对齐到文件真实末尾。
static double mediaDurationSec(const std::string& path) {
  AVFormatContext* fmt = nullptr;
  if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return 0;
  double d = 0;
  if (avformat_find_stream_info(fmt, nullptr) >= 0 && fmt->duration > 0)
    d = (double)fmt->duration / AV_TIME_BASE;
  avformat_close_input(&fmt);
  return d;
}

// 用 src 替换 dst：先把 dst 备份，再改名；失败时回滚，避免 Windows 上 rename 不能覆盖已存在文件。
static bool replaceFile(const std::string& src, const std::string& dst) {
  std::error_code ec;
  if (!pathExists(src)) { logx::error("replace: missing %s", src.c_str()); return false; }
  const std::string bak = dst + ".bak";
  std::filesystem::remove(bak, ec);
  if (ec) logx::warn("replace: remove stale %s failed: %s", bak.c_str(), ec.message().c_str());
  if (pathExists(dst)) {
    std::filesystem::rename(dst, bak, ec);
    if (ec) {
      logx::error("replace: backup %s failed: %s", dst.c_str(), ec.message().c_str());
      return false;
    }
  }
  std::filesystem::rename(src, dst, ec);
  if (ec) {
    logx::error("replace: rename to %s failed: %s", dst.c_str(), ec.message().c_str());
    std::error_code e2;
    if (pathExists(bak)) std::filesystem::rename(bak, dst, e2);
    return false;
  }
  std::filesystem::remove(bak, ec);
  if (ec) logx::warn("replace: remove %s failed: %s", bak.c_str(), ec.message().c_str());
  return true;
}

// 把续编分段合并进主文件并替换。分段先改名为 .merging，合并/替换成功后才删除；
// mergeToMp4 会识别“已合并过”的重叠分段，因此崩溃窗口不会重复拼接。
static bool mergePartIntoMain(const std::string& main, const std::string& partPath,
                              const std::string& tmp) {
  const std::string merging = partPath + ".merging";
  std::string seg = pathExists(merging) ? merging : partPath;
  if (!pathExists(main) || !pathExists(seg)) return false;

  if (seg == partPath) {
    std::error_code ec;
    std::filesystem::rename(partPath, merging, ec);
    if (ec) {
      logx::error("merge: rename %s -> .merging failed: %s", partPath.c_str(), ec.message().c_str());
      return false;
    }
    seg = merging;
  }

  std::error_code ec;
  std::filesystem::remove(tmp, ec);
  if (!mergeToMp4(main.c_str(), seg.c_str(), tmp.c_str())) {
    logx::error("merge: %s + %s failed", main.c_str(), seg.c_str());
    std::filesystem::remove(tmp, ec);
    return false;   // 保留 .merging，下次重试
  }
  if (!replaceFile(tmp, main)) {
    std::filesystem::remove(tmp, ec);
    return false;   // 保留 .merging，下次重试
  }
  std::filesystem::remove(seg, ec);
  if (ec) {
    logx::warn("merge: remove %s failed (will retry cleanup next start): %s",
               seg.c_str(), ec.message().c_str());
  }
  return true;
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
      char id[512] = {}, src[4096] = {}, desc[2048] = {};
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
        // 质量档在第二行；旧 meta 没有该行时沿用旧 target/4
        char extra[256];
        int qi = -1;
        if (fgets(extra, sizeof(extra), f)) {
          if (sscanf(extra, "sr_input=%d", &qi) != 1) qi = -1;
        }
        tp->srInput = qualityFromInt(qi);
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
  fprintf(f, "sr_input=%d\n", (int)t->srInput);
  fclose(f);
}

Task* TaskCenter::create(const char* source, int targetW, SRInputQuality quality) {
  auto tp = std::make_unique<Task>();
  tp->id = nowId();
  tp->source = source;
  tp->targetW = targetW;
  tp->srInput = quality;
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
  // 上次线程可能已自然结束但仍处于 joinable；移动赋值给 joinable std::thread 会 terminate
  if (t->thread.joinable()) t->thread.join();
  t->threadJoined = false;
  if (!t->prog) t->prog = std::make_unique<EncodeProgress>();  // 从 meta 加载的任务 prog 为空，需懒创建
  t->status.store(ST_RUNNING);
  t->prog->reset();
  EncodeOptions eo;
  eo.verbose = false;
  eo.prog = t->prog.get();
  eo.preview = previewFn_;
  eo.previewUserdata = previewUd_;
  eo.modelBase = modelBase_.empty() ? Config::defaults().modelBase : modelBase_;
  eo.srInput = t->srInput;
  std::string partPath = t->temp + ".resume";
  std::string tmpPath = t->temp + ".merged";
  eo.startTime = t->resumeTime > 0.01 ? t->resumeTime : 0.0;
  eo.startFrame = t->resumeTime > 0.01 ? t->startFrame : 0;
  // 编码中周期持久化断点（供应用被杀/重启后续编）
  t->prog->checkpointCb = [this, t] {
    t->resumeTime = t->prog->curTime.load();
    t->startFrame = t->prog->frame.load();
    t->frames = t->prog->frame.load();
    t->totalFrames = t->prog->totalFrames.load();
    save(t);
  };
  save(t);   // 先持久化 RUNNING，避免与编码线程的首次 checkpoint 并发写 meta
  t->thread = std::thread([this, t, eo, partPath, tmpPath]() mutable {
    auto finish = [&](bool encOk, bool archiveOk, bool cancelled, bool resetCheckpoint = false) {
      if (resetCheckpoint) {
        t->frames = 0;
        t->totalFrames = 0;
        t->resumeTime = 0;
        t->startFrame = 0;
      } else {
        long long frames = t->prog->frame.load();
        double curTime = t->prog->curTime.load();
        if (frames > 0 || curTime > 0.01) {
          t->frames = frames;
          t->totalFrames = t->prog->totalFrames.load();
          t->fps = t->prog->fps.load();
          t->resumeTime = curTime;
          t->startFrame = frames;
        }
      }
      int st = (encOk && archiveOk) ? ST_DONE : (cancelled ? ST_PAUSED : ST_FAILED);
      t->status.store(st);
      save(t);
    };

    // 分段状态恢复：
    //  - 主文件缺失 + 首次编码分段：分段本身就是 [0, checkpoint]，提升为主文件；
    //  - 主文件与续编分段都存在：先合并，避免下次从头编码造成时间轴缺口；
    //  - .merging 是合并中的续编分段；.bak 是替换主文件时的备份，先恢复再判断。
    const std::string bak = t->temp + ".bak";
    if (!pathExists(t->temp) && pathExists(bak)) {
      std::error_code ec;
      std::filesystem::rename(bak, t->temp, ec);
      if (ec) { finish(false, false, false); return; }
    } else if (pathExists(t->temp) && pathExists(bak)) {
      std::error_code ec;
      std::filesystem::remove(bak, ec);   // 主文件已提交，备份可丢弃
    }

    const std::string merging = partPath + ".merging";
    const std::string contMark = partPath + ".cont";
    bool haveMain = pathExists(t->temp);
    bool havePart = pathExists(partPath);
    bool haveMerging = pathExists(merging);
    if (havePart && haveMerging) {
      // 状态异常：不猜测哪个更新，保留两份并报失败，避免删掉较新的分段
      logx::error("task %s: both .resume and .resume.merging exist", t->id.c_str());
      finish(false, false, false);
      return;
    }

    if (!haveMain && havePart && t->resumeTime > 0.01) {
      // 续编分段不能当作首段提升：主文件丢失时宁可失败，也不能静默丢掉前半段
      if (pathExists(contMark)) { finish(false, false, false); return; }
      if (!replaceFile(partPath, t->temp)) { finish(false, false, false); return; }
      haveMain = true;
      havePart = false;
    }
    if (!haveMain && haveMerging) { finish(false, false, false); return; }
    if (haveMain && (havePart || haveMerging)) {
      if (!mergePartIntoMain(t->temp, partPath, tmpPath)) {
        finish(false, false, false);
        return;
      }
      havePart = false;
      haveMerging = false;
    }

    // 检查点每秒落盘，可能落后于文件真实末尾；续编前把断点对齐到主文件末尾，
    // 避免下一段从旧检查点重编造成接缝重复。
    if (haveMain) {
      double mainDur = mediaDurationSec(t->temp);
      if (mainDur > 0.01 && mainDur > t->resumeTime + 0.05) {
        t->resumeTime = mainDur;
        double srcDur = mediaDurationSec(t->source);
        if (srcDur > 0.01 && t->totalFrames > 0)
          t->startFrame = (long long)llround(mainDur / srcDur * (double)t->totalFrames);
        eo.startTime = t->resumeTime;
        eo.startFrame = t->startFrame;
      }
    }

    const bool resume = haveMain && t->resumeTime > 0.01;
    if (!resume) { eo.startTime = 0; eo.startFrame = 0; }
    // 标记续编分段：主文件丢失时据此拒绝“把续编段当首段”
    {
      std::error_code ec;
      if (resume) {
        FILE* mark = fopen(contMark.c_str(), "wb");
        if (!mark) { finish(false, false, false); return; }
        fclose(mark);
      } else {
        std::filesystem::remove(contMark, ec);
      }
    }

    bool encOk = encodeOffline(t->source.c_str(), partPath.c_str(), t->targetW, eo);
    bool cancelled = t->prog->cancel.load();
    bool archiveOk = true;
    bool resetCheckpoint = false;
    // 从检查点已经到结尾：没有新帧，不要合并空分段；但要确认主文件确实完整
    if (resume && encOk && t->prog->frame.load() == 0) {
      double outDur = mediaDurationSec(t->temp);
      double srcDur = mediaDurationSec(t->source);
      // 编码目前按整数 fps 输出，非整数帧率的长片会略短；用 1%（至少 1s）容差，
      // 明显短于源才判定为未完成。
      double tol = srcDur > 0 ? (std::max)(1.0, srcDur * 0.01) : 0.0;
      if (outDur > 0 && srcDur > 0 && outDur + tol < srcDur) {
        finish(false, false, false);   // 源比产物长：不能假完成
        return;
      }
      std::error_code ec;
      std::filesystem::remove(partPath, ec);
      std::filesystem::remove(contMark, ec);
      if (t->totalFrames > 0) t->frames = t->totalFrames;
      finish(true, true, false);
      return;
    }
    if (encOk) {
      // 首次编码直接把分段提升为主文件；续编则与主文件合并
      archiveOk = resume ? mergePartIntoMain(t->temp, partPath, tmpPath)
                         : replaceFile(partPath, t->temp);
      if (!archiveOk && !resume) {
        // 首次编码产物无法替换：丢弃分段并清零断点，避免下次被误认为续编分段
        std::error_code ec;
        std::filesystem::remove(partPath, ec);
        resetCheckpoint = true;
      }
    } else if (!resume) {
      // 首次编码被取消/失败：有部分产物就提升为主文件，保留断点供续编
      if (pathExists(partPath) && pathSize(partPath) > 0) {
        archiveOk = replaceFile(partPath, t->temp);
        if (!archiveOk) {
          std::error_code ec;
          std::filesystem::remove(partPath, ec);
          resetCheckpoint = true;
        }
      } else {
        archiveOk = false;
        resetCheckpoint = true;
      }
    } else {
      // 续编被取消/失败：保留 partPath，下次启动先合并再继续（不覆盖已有进度）
      archiveOk = false;
    }
    if (archiveOk) {
      std::error_code ec;
      std::filesystem::remove(contMark, ec);
    }
    finish(encOk, archiveOk, cancelled, resetCheckpoint);
  });
  t->threadJoined = true;
}

void TaskCenter::stop(Task* t) {
  if (!t) return;
  if (t->prog) t->prog->cancel.store(true);
  if (t->thread.joinable()) t->thread.join();
  t->threadJoined = false;
  if (t->status.load() == ST_RUNNING) t->status.store(ST_PAUSED);
  // 只有真正编码过才更新断点；启动即失败（如模型路径错误）不能把已有检查点清零
  if (t->prog && (t->prog->frame.load() > 0 || t->prog->curTime.load() > 0.01)) {
    t->resumeTime = t->prog->curTime.load();
    t->startFrame = t->prog->frame.load();
    t->frames = t->prog->frame.load();
    t->totalFrames = t->prog->totalFrames.load();
  }
  save(t);
}

void TaskCenter::shutdown() {
  // 先给所有任务发取消信号，再逐个 join，避免第一个任务等待时其余任务继续写文件
  for (auto& tp : tasks_) {
    if (tp->prog) tp->prog->cancel.store(true);
  }
  for (auto& tp : tasks_) {
    // 已完成/未运行且线程已回收的任务无需重写 meta
    if (tp->thread.joinable() || tp->status.load() == ST_RUNNING) stop(tp.get());
  }
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
