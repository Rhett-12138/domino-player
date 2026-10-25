#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
#define DOMINO_EXPORT __declspec(dllexport)
#else
#define DOMINO_EXPORT __attribute__((visibility("default")))
#endif

// Flutter runner 注册插件：取得纹理 registrar（在 flutter_window.cpp 创建引擎后调用）
struct FlutterDesktopPluginRegistrar;
typedef struct FlutterDesktopPluginRegistrar* FlutterDesktopPluginRegistrarRef;
extern "C" DOMINO_EXPORT void DominoRegisterPlugin(FlutterDesktopPluginRegistrarRef registrar);

// ---- Dart FFI 导出 ----
#ifdef __cplusplus
extern "C" {
#endif

// 生命周期：model_dir/data_root 传路径；返回引擎句柄
DOMINO_EXPORT void* domino_create(const char* model_dir, const char* data_root);
DOMINO_EXPORT void  domino_destroy(void* h);

// 播放
DOMINO_EXPORT int    domino_open(void* h, const char* path);
DOMINO_EXPORT void   domino_close(void* h);
DOMINO_EXPORT int    domino_has_video(void* h);
DOMINO_EXPORT void   domino_play(void* h);          // 取消暂停
DOMINO_EXPORT void   domino_pause(void* h);
DOMINO_EXPORT int    domino_paused(void* h);
DOMINO_EXPORT void   domino_seek(void* h, double t);
DOMINO_EXPORT void   domino_volume(void* h, double v);
DOMINO_EXPORT double domino_position(void* h);
DOMINO_EXPORT double domino_duration(void* h);

// 实时超分：enabled=0 关闭；enabled=1 时 targetW 为目标宽（如 1920/2560/3840）
DOMINO_EXPORT void   domino_set_sr(void* h, int enabled, int targetW);
// SR 当前是否启用 / 是否初始化失败（失败时播放回退源帧）
DOMINO_EXPORT int    domino_sr_active(void* h);
DOMINO_EXPORT int    domino_sr_failed(void* h);
// 当前实际 SR 目标宽（自动降档后变化；关闭时为 0）/ 是否处于自动降档
DOMINO_EXPORT int    domino_sr_target_w(void* h);
DOMINO_EXPORT int    domino_sr_auto(void* h);

// 美颜/滤镜
DOMINO_EXPORT void   domino_beauty(void* h, float smooth, float white);
DOMINO_EXPORT void   domino_filter(void* h, int idx, float amt);

// 每帧驱动（前台定时器调用）：解码/超分/调度，有新帧则标记纹理可用
DOMINO_EXPORT void   domino_pump(void* h);
// Flutter Texture widget 用的纹理 id
DOMINO_EXPORT int64_t domino_texture_id(void* h);
// 实际呈现帧率（引擎侧统计）
DOMINO_EXPORT double domino_fps(void* h);
// 是否已播到结尾（EOF；重新播放/seek 后复位）
DOMINO_EXPORT int    domino_eof(void* h);
// 全屏切换：on=1 进入 / on=0 退出，返回当前是否全屏
DOMINO_EXPORT int    domino_set_fullscreen(void* h, int on);
// 当前视频帧尺寸（宽/高）：优先帧输出尺寸，无帧时退回源尺寸；无文件时 0。
DOMINO_EXPORT int    domino_frame_w(void* h);
DOMINO_EXPORT int    domino_frame_h(void* h);

// 诊断 JSON（UTF-8）：GPU/adapter、backend、SR、cudart/TensorRT、Vulkan、fallback reason
DOMINO_EXPORT int    domino_diagnostics(void* h, char* buf, int cap);

// ---- 任务中心（离线超分） ----
// 创建任务，返回任务索引（-1 失败）；source=源视频，targetW=输出宽，desc=备注
DOMINO_EXPORT int    domino_task_create(void* h, const char* source, int targetW, const char* desc);
// 带质量档：quality 0=Fast 1=Balanced 2=FullSource；旧入口默认 FullSource
DOMINO_EXPORT int    domino_task_create_ex(void* h, const char* source, int targetW, int quality, const char* desc);
// 质量 + 模型：0=真人标准 1=动画标准 2=真人高质量 3=动画高质量
DOMINO_EXPORT int    domino_task_create_ex2(void* h, const char* source, int targetW, int quality, int model, const char* desc);
DOMINO_EXPORT int    domino_task_model(void* h, int idx);   // 0=真人标准 1=动画标准 2=真人高质量 3=动画高质量
DOMINO_EXPORT void   domino_task_start(void* h, int idx);    // 开始/继续（有断点自动续编）
DOMINO_EXPORT void   domino_task_stop(void* h, int idx);     // 暂停
DOMINO_EXPORT void   domino_task_remove(void* h, int idx);   // 删除
DOMINO_EXPORT int    domino_task_count(void* h);
DOMINO_EXPORT int    domino_task_status(void* h, int idx);   // 0未开始 1进行中 2暂停 3完成 4失败
DOMINO_EXPORT long long domino_task_frames(void* h, int idx);
DOMINO_EXPORT long long domino_task_total(void* h, int idx);
DOMINO_EXPORT int    domino_task_name(void* h, int idx, char* buf, int cap);      // 源文件名
DOMINO_EXPORT int    domino_task_source(void* h, int idx, char* buf, int cap);    // 源路径
DOMINO_EXPORT int    domino_task_temp(void* h, int idx, char* buf, int cap);      // 产物路径
DOMINO_EXPORT double domino_task_resume_time(void* h, int idx);

// ---- 滤镜缩略图（生成当前帧 × 各滤镜的小预览图） ----
DOMINO_EXPORT int    domino_make_previews(void* h, int count);
DOMINO_EXPORT int    domino_preview_w(void* h);
DOMINO_EXPORT int    domino_preview_h(void* h);
DOMINO_EXPORT int    domino_preview_buf(void* h, int idx, uint8_t* dst, int cap);

// ---- 系统文件选择框 ----
// initialPath 提供初始目录（可空），选中返回 1 且 buf 写入 UTF-8 路径，取消/失败返回 0。
DOMINO_EXPORT int    domino_open_dialog(void* h, const char* initialPath, char* buf, int cap);

#ifdef __cplusplus
}
#endif
