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
  while (t->status.load() == ST_RUNNING && wait < 400) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); wait++; }
  EXPECT_TRUE(t->status.load() == ST_DONE);
  EXPECT_TRUE(std::filesystem::exists(t->temp));
  EXPECT_TRUE(t->frames > 40);
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
