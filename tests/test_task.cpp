#include "test_common.h"
#include "task.h"
#include "config.h"
#include <filesystem>
#include <thread>
#include <chrono>
#include <cstdio>

// 任务中心：创建 → 短暂运行 → 停止（断点）→ 重启扫描 → 继续 → 完成；产物时长应完整
TEST(TaskPersistenceAndResume) {
  std::string root = testcfg::dataDir() + "/tasks_root";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  TaskCenter tc;
  tc.init(root);
  Task* t = tc.create(assetPath("test_long.mp4").c_str(), 1280);  // 1280=源宽，无超分，15s
  EXPECT_TRUE(t != nullptr);
  tc.start(t);
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  tc.stop(t);
  EXPECT_TRUE(t->status.load() == ST_PAUSED);
  EXPECT_TRUE(t->resumeTime > 0.05);
  EXPECT_TRUE(std::filesystem::exists(t->temp));   // 部分产物在

  // 模拟重启：全新实例重新扫描 meta
  TaskCenter fresh;
  fresh.init(root);
  Task* t2 = nullptr;
  for (auto& tp : fresh.tasks()) if (tp->id == t->id) t2 = tp.get();
  EXPECT_TRUE(t2 != nullptr);
  EXPECT_NEAR(t2->resumeTime, t->resumeTime, 0.05);

  // 继续 → 等完成
  fresh.start(t2);
  int wait = 0;
  while (t2->status.load() == ST_RUNNING && wait < 600) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); wait++; }
  EXPECT_TRUE(t2->status.load() == ST_DONE);
  EXPECT_TRUE(std::filesystem::exists(t2->temp));

  // 校验产物可打开（时长 ~15s）
  AVFormatContext* fmt = nullptr;
  EXPECT_TRUE(avformat_open_input(&fmt, t2->temp.c_str(), nullptr, nullptr) >= 0);
  if (fmt) {
    avformat_find_stream_info(fmt, nullptr);
    double d = fmt->duration > 0 ? (double)fmt->duration / AV_TIME_BASE : 0;
    avformat_close_input(&fmt);
    EXPECT_NEAR(d, 15.0, 1.0);
  }
}

// 失败任务必须落到 ST_FAILED；再次点「开始」不能因 joinable 线程 terminate
TEST(TaskFailureAndRetry) {
  std::string root = testcfg::dataDir() + "/tasks_fail";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  TaskCenter tc;
  tc.init(root);
  Task* t = tc.create(assetPath("no_such_file.mp4").c_str(), 1280);
  EXPECT_TRUE(t != nullptr);

  for (int round = 0; round < 2; round++) {
    tc.start(t);
    int wait = 0;
    while (t->status.load() == ST_RUNNING && wait < 100) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      wait++;
    }
    EXPECT_TRUE(t->status.load() == ST_FAILED);
  }
  tc.remove(t);
}

// 续编被取消时分段必须保留；下次启动先合并旧分段再继续，最终产物完整
TEST(TaskResumeCancelKeepsSegment) {
  std::string root = testcfg::dataDir() + "/tasks_resume_cancel";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  TaskCenter tc;
  tc.init(root);
  Task* t = tc.create(assetPath("test_long.mp4").c_str(), 1280);
  EXPECT_TRUE(t != nullptr);
  std::string part = t->temp + ".resume";

  // 第一段：从头编码后取消 → 部分主文件
  tc.start(t);
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  tc.stop(t);
  EXPECT_TRUE(t->status.load() == ST_PAUSED);
  EXPECT_TRUE(t->resumeTime > 0.05);
  EXPECT_TRUE(std::filesystem::exists(t->temp));
  double firstResume = t->resumeTime;

  // 第二段：续编后取消 → 分段应保留，不能被删除
  tc.start(t);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  tc.stop(t);
  EXPECT_TRUE(t->status.load() == ST_PAUSED);
  EXPECT_TRUE(t->resumeTime >= firstResume);
  EXPECT_TRUE(std::filesystem::exists(part));

  // 第三段：启动时合并旧分段，继续到完成
  tc.start(t);
  int wait = 0;
  while (t->status.load() == ST_RUNNING && wait < 600) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    wait++;
  }
  EXPECT_TRUE(t->status.load() == ST_DONE);
  EXPECT_FALSE(std::filesystem::exists(part));

  AVFormatContext* fmt = nullptr;
  EXPECT_TRUE(avformat_open_input(&fmt, t->temp.c_str(), nullptr, nullptr) >= 0);
  if (fmt) {
    avformat_find_stream_info(fmt, nullptr);
    double d = fmt->duration > 0 ? (double)fmt->duration / AV_TIME_BASE : 0;
    avformat_close_input(&fmt);
    EXPECT_NEAR(d, 15.0, 0.6);
  }

  // 模拟“已合并但清理失败”：再放一个与主文件相同内容的分段，重启后应识别为已合并，
  // 只清理分段，不能把内容再拼一遍（否则时长约 30s）。
  std::error_code ec2;
  std::filesystem::copy_file(t->temp, part, std::filesystem::copy_options::overwrite_existing, ec2);
  EXPECT_TRUE(!ec2);
  tc.start(t);
  wait = 0;
  while (t->status.load() == ST_RUNNING && wait < 300) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    wait++;
  }
  EXPECT_TRUE(t->status.load() == ST_DONE);
  EXPECT_FALSE(std::filesystem::exists(part));
  fmt = nullptr;
  EXPECT_TRUE(avformat_open_input(&fmt, t->temp.c_str(), nullptr, nullptr) >= 0);
  if (fmt) {
    avformat_find_stream_info(fmt, nullptr);
    double d = fmt->duration > 0 ? (double)fmt->duration / AV_TIME_BASE : 0;
    avformat_close_input(&fmt);
    EXPECT_NEAR(d, 15.0, 0.6);   // 若重复拼接会接近 30s
  }
}

// 主文件丢失时，带 .cont 标记的续编分段不能被当作首段提升（否则会静默丢掉前半段）
TEST(TaskContinuationWithoutMainFails) {
  std::string root = testcfg::dataDir() + "/tasks_cont";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  TaskCenter tc;
  tc.init(root);
  Task* t = tc.create(assetPath("test_av.mp4").c_str(), 1280);
  EXPECT_TRUE(t != nullptr);
  std::string part = t->temp + ".resume";
  EXPECT_TRUE(std::filesystem::copy_file(assetPath("test_av.mp4"), part, ec));
  FILE* mark = fopen((part + ".cont").c_str(), "wb");
  EXPECT_TRUE(mark != nullptr);
  if (mark) fclose(mark);
  t->resumeTime = 1.0;
  t->startFrame = 30;
  tc.save(t);

  tc.start(t);
  int wait = 0;
  while (t->status.load() == ST_RUNNING && wait < 100) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    wait++;
  }
  EXPECT_TRUE(t->status.load() == ST_FAILED);
  EXPECT_TRUE(std::filesystem::exists(part));   // 保留分段供人工恢复
  tc.remove(t);
}

// 合并恢复文件：.merging 已合并时只清理；.bak 在主文件缺失时恢复
TEST(TaskMergeRecoveryFiles) {
  std::string root = testcfg::dataDir() + "/tasks_merge_recovery";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  TaskCenter tc;
  tc.init(root);
  Task* t = tc.create(assetPath("test_av.mp4").c_str(), 1280);
  EXPECT_TRUE(t != nullptr);

  // 先完成一个 2s 任务，得到完整主文件
  tc.start(t);
  int wait = 0;
  while (t->status.load() == ST_RUNNING && wait < 200) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    wait++;
  }
  EXPECT_TRUE(t->status.load() == ST_DONE);

  // 模拟“已合并但未清理”的 .merging：内容与主文件相同，重启后应只清理，不应重复拼接
  std::string merging = t->temp + ".resume.merging";
  EXPECT_TRUE(std::filesystem::copy_file(t->temp, merging,
    std::filesystem::copy_options::overwrite_existing, ec));
  t->resumeTime = 2.0;
  t->startFrame = 60;
  tc.save(t);
  tc.start(t);
  wait = 0;
  while (t->status.load() == ST_RUNNING && wait < 200) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    wait++;
  }
  EXPECT_TRUE(t->status.load() == ST_DONE);
  EXPECT_FALSE(std::filesystem::exists(merging));

  // 模拟替换主文件时崩溃：只剩 .bak，启动后应恢复为主文件
  std::string bak = t->temp + ".bak";
  std::filesystem::rename(t->temp, bak, ec);
  EXPECT_TRUE(!ec);
  tc.start(t);
  wait = 0;
  while (t->status.load() == ST_RUNNING && wait < 200) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    wait++;
  }
  EXPECT_TRUE(t->status.load() == ST_DONE);
  EXPECT_TRUE(std::filesystem::exists(t->temp));
  EXPECT_FALSE(std::filesystem::exists(bak));
  tc.remove(t);
}

// meta 中空描述不能读到未初始化内存
TEST(TaskMetaEmptyDescription) {
  std::string root = testcfg::dataDir() + "/tasks_meta_desc";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  TaskCenter tc;
  tc.init(root);
  Task* t = tc.create(assetPath("test_av.mp4").c_str(), 1280);
  EXPECT_TRUE(t != nullptr);
  EXPECT_STREQ(t->description.c_str(), "");

  TaskCenter fresh;
  fresh.init(root);
  Task* loaded = nullptr;
  for (auto& tp : fresh.tasks()) if (tp->id == t->id) loaded = tp.get();
  EXPECT_TRUE(loaded != nullptr);
  if (loaded) {
    EXPECT_STREQ(loaded->description.c_str(), "");
    EXPECT_NEAR(loaded->resumeTime, 0.0, 1e-9);
  }
}

// 质量档必须持久化，续编不能换输入策略（否则合并处画质跳变）
TEST(TaskQualityPersists) {
  std::string root = testcfg::dataDir() + "/tasks_quality";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  TaskCenter tc;
  tc.init(root);
  Task* t = tc.create(assetPath("test_av.mp4").c_str(), 1920,
                      SRInputQuality::Balanced, SRModel::AnimeHQ);
  EXPECT_TRUE(t != nullptr);
  tc.save(t);

  TaskCenter fresh;
  fresh.init(root);
  Task* loaded = nullptr;
  for (auto& tp : fresh.tasks()) if (tp->id == t->id) loaded = tp.get();
  EXPECT_TRUE(loaded != nullptr);
  if (loaded) {
    EXPECT_TRUE(loaded->srInput == SRInputQuality::Balanced);
    EXPECT_TRUE(loaded->model == SRModel::AnimeHQ);
  }
}

// 带超分的任务应跑到完成（回归"卡在最后一帧"：确认引擎侧能到 ST_DONE）
TEST(TaskWithSRCompletes) {
  std::string root = testcfg::dataDir() + "/tasks_sr";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  TaskCenter tc;
  tc.init(root);
  // targetW=1920 > 源 1280 → 触发 NCNN 超分
  Task* t = tc.create(assetPath("test_av.mp4").c_str(), 1920);
  EXPECT_TRUE(t != nullptr);
  tc.start(t);
  int wait = 0;
  // FullSource 离线 SR 比旧 target/4 慢，给慢机器更充足时间
  while (t->status.load() == ST_RUNNING && wait < 1200) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); wait++; }
  EXPECT_TRUE(t->status.load() == ST_DONE);
  EXPECT_TRUE(std::filesystem::exists(t->temp));
  EXPECT_TRUE(t->frames > 40);
  MediaStream out;
  EXPECT_TRUE(out.open(t->temp.c_str()));
  EXPECT_EQ(out.vw, 1920);   // FullSource 720p → 1920 输出，不能被中间尺寸截断
  EXPECT_EQ(out.vh, 1080);
  out.close();
}

// 动画模型任务：应使用 animevideov3 并正常完成
TEST(TaskWithAnimeModelCompletes) {
  std::string root = testcfg::dataDir() + "/tasks_anime";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  TaskCenter tc;
  tc.init(root);
  Task* t = tc.create(assetPath("test_av.mp4").c_str(), 1920,
                      SRInputQuality::FullSource, SRModel::Anime);
  EXPECT_TRUE(t != nullptr);
  EXPECT_TRUE(t->model == SRModel::Anime);
  tc.start(t);
  int wait = 0;
  while (t->status.load() == ST_RUNNING && wait < 1200) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    wait++;
  }
  EXPECT_TRUE(t->status.load() == ST_DONE);
  EXPECT_TRUE(t->frames > 40);
  MediaStream out;
  EXPECT_TRUE(out.open(t->temp.c_str()));
  EXPECT_EQ(out.vw, 1920);
  out.close();
}

// 删除任务：文件夹递归删除
TEST(TaskRemove) {
  std::string root = testcfg::dataDir() + "/tasks_remove";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  TaskCenter tc;
  tc.init(root);
  Task* t = tc.create(assetPath("test_av.mp4").c_str(), 1280);
  std::string folder = t->folder;
  EXPECT_TRUE(std::filesystem::exists(folder));
  tc.remove(t);
  EXPECT_FALSE(std::filesystem::exists(folder));
  EXPECT_TRUE(tc.tasks().empty());
}
