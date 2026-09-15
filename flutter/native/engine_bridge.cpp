#include "engine_bridge.h"
#include <flutter_export.h>
#include <flutter_plugin_registrar.h>
#include <flutter_texture_registrar.h>
#include <flutter_windows.h>

#include <windows.h>
#include <shobjidl.h>
#include <mutex>
#include <vector>
#include <memory>
#include <cstring>
#include <string>

#include "../../src/config.h"
#include "../../src/player.h"
#include "../../src/audio.h"
#include "../../src/frame_output.h"
#include "../../src/task.h"

// 引擎句柄：Player + 帧输出 + Flutter 纹理 + 任务中心
struct EngineHandle {
  Config cfg;
  Player* player = nullptr;
  EngineFrameOutput* out = nullptr;
  std::unique_ptr<TaskCenter> tasks;      // 离线超分任务中心
  FlutterDesktopTextureRegistrarRef registrar = nullptr;
  int64_t texture_id = -1;
  HWND hwnd = nullptr;            // 宿主窗口（全屏用）
  bool fullscreen = false;
  WINDOWPLACEMENT savedPlacement = {};
  LONG savedStyle = 0;
  // RGBA8888 像素纹理双缓冲：Flutter 上一帧读完前不重写
  std::vector<uint8_t> rgba[2];
  int rgbaW = 0, rgbaH = 0;
  int rgbaFlip = 0;
  FlutterDesktopPixelBuffer pb = {};
};

static EngineHandle* g_engine = nullptr;

// 空帧占位（首帧到达前给 Flutter 一个合法 1x1 黑色像素缓冲）
static uint8_t g_black_px[4] = { 0, 0, 0, 255 };

// Flutter 光栅线程取帧回调：把最新帧（含美颜/滤镜）以 RGBA8888 返回。
// 只在 out 锁内做 RGBA 转换（memcpy 量级），解码/超分/美颜全程不阻塞本线程。
static const FlutterDesktopPixelBuffer* CopyPixels(size_t w, size_t h, void* user_data) {
  EngineHandle* e = (EngineHandle*)user_data;
  int ow = 0, oh = 0;
  const uint8_t* src = nullptr;
  {
    std::lock_guard<std::mutex> lk(e->out->lock());
    ow = e->out->width();
    oh = e->out->height();
    if (ow <= 0 || oh <= 0) {
      e->pb.buffer = g_black_px;
      e->pb.width = 1;
      e->pb.height = 1;
      return &e->pb;
    }
    src = e->out->frame();
    if ((size_t)ow * oh * 4 != e->rgba[e->rgbaFlip].size())
      e->rgba[e->rgbaFlip].resize((size_t)ow * oh * 4);
    uint8_t* dst = e->rgba[e->rgbaFlip].data();
    uint32_t* d = (uint32_t*)dst;
    int n = ow * oh;
    for (int i = 0; i < n; i++) {
      d[i] = 0xFF000000u | src[(size_t)i * 3 + 0] | ((uint32_t)src[(size_t)i * 3 + 1] << 8) |
             ((uint32_t)src[(size_t)i * 3 + 2] << 16);
    }
    e->rgbaW = ow;
    e->rgbaH = oh;
    e->rgbaFlip ^= 1;
  }
  e->pb.buffer = e->rgba[e->rgbaFlip ^ 1].data();
  e->pb.width = (size_t)e->rgbaW;
  e->pb.height = (size_t)e->rgbaH;
  return &e->pb;
}

void DominoRegisterPlugin(FlutterDesktopPluginRegistrarRef registrar) {
  if (!g_engine) g_engine = new EngineHandle();
  g_engine->registrar = FlutterDesktopRegistrarGetTextureRegistrar(registrar);
  // 取宿主 view 窗口句柄（全屏切换用；实际作用于其顶层窗口）
  FlutterDesktopViewRef view = FlutterDesktopPluginRegistrarGetView(registrar);
  if (view) g_engine->hwnd = FlutterDesktopViewGetHWND(view);
  FlutterDesktopTextureInfo info = {};
  info.type = kFlutterDesktopPixelBufferTexture;
  info.pixel_buffer_config.callback = CopyPixels;
  info.pixel_buffer_config.user_data = g_engine;
  g_engine->texture_id =
      FlutterDesktopTextureRegistrarRegisterExternalTexture(g_engine->registrar, &info);
}

// ---- FFI 导出 ----
void* domino_create(const char* model_dir, const char* data_root) {
  if (!g_engine) g_engine = new EngineHandle();
  EngineHandle* e = g_engine;
  if (!e->player) {
    e->cfg.modelBase = model_dir ? model_dir : "";
    e->cfg.dataRoot = data_root ? data_root : "";
    e->out = new EngineFrameOutput();
    e->player = new Player(*e->out);
    e->player->setConfig(e->cfg);
    e->player->setNoSR(true);
    e->tasks = std::make_unique<TaskCenter>();
    e->tasks->init(e->cfg.dataRoot);
    e->tasks->setModelBase(e->cfg.modelBase);
  }
  return e;
}

void domino_destroy(void* h) {
  EngineHandle* e = (EngineHandle*)h;
  if (!e) return;
  e->player->shutdown();
  delete e->player; e->player = nullptr;
  delete e->out; e->out = nullptr;
  if (g_engine == e) g_engine = nullptr;
  delete e;
}

int domino_open(void* h, const char* path) {
  EngineHandle* e = (EngineHandle*)h;
  return e && e->player && e->player->open(path ? path : "") ? 1 : 0;
}
void domino_close(void* h) { if (h) ((EngineHandle*)h)->player->close(); }
int domino_has_video(void* h) { return h && ((EngineHandle*)h)->player->hasVideo() ? 1 : 0; }
void domino_play(void* h) { if (h) ((EngineHandle*)h)->player->setPaused(false); }
void domino_pause(void* h) { if (h) ((EngineHandle*)h)->player->setPaused(true); }
int domino_paused(void* h) { return h && ((EngineHandle*)h)->player->paused() ? 1 : 0; }
void domino_seek(void* h, double t) { if (h) ((EngineHandle*)h)->player->requestSeek(t); }
void domino_volume(void* h, double v) { if (h) ((EngineHandle*)h)->player->setVolume(v); }
double domino_position(void* h) { return h ? ((EngineHandle*)h)->player->position() : 0; }
double domino_duration(void* h) { return h ? ((EngineHandle*)h)->player->duration() : 0; }

void domino_beauty(void* h, float smooth, float white) {
  EngineHandle* e = (EngineHandle*)h;
  if (!e) return;
  e->player->setBeauty(smooth, white);
  EngineBeautyParams bp = e->player->beauty();
  e->out->setBeautyParams(bp);
}
void domino_filter(void* h, int idx, float amt) {
  EngineHandle* e = (EngineHandle*)h;
  if (!e) return;
  e->player->setFilter(idx, amt);
  EngineBeautyParams bp = e->player->beauty();
  e->out->setBeautyParams(bp);
}

void domino_pump(void* h) {
  EngineHandle* e = (EngineHandle*)h;
  if (!e || !e->player) return;
  // 无锁驱动：presentFrame 内部锁内只做缓冲交换，解码/超分/美颜不阻塞光栅线程
  e->player->pump();
  if (e->out->hasNewFrame()) {
    FlutterDesktopTextureRegistrarMarkExternalTextureFrameAvailable(e->registrar, e->texture_id);
    e->out->clearNewFrame();
  }
}

int64_t domino_texture_id(void* h) {
  return h ? ((EngineHandle*)h)->texture_id : -1;
}

// 真实呈现帧率（Player::presentFrame 统计，每 1s 更新）
double domino_fps(void* h) {
  return h && ((EngineHandle*)h)->player ? ((EngineHandle*)h)->player->fpsDisplay() : 0;
}

// 是否已播到结尾（EOF；重新播放/seek 后复位）
int domino_eof(void* h) {
  return h && ((EngineHandle*)h)->player && ((EngineHandle*)h)->player->eof() ? 1 : 0;
}

// 全屏切换：on=1 进入无边框全屏（覆盖所在显示器），on=0 还原。返回当前是否全屏。
int domino_set_fullscreen(void* h, int on) {
  EngineHandle* e = (EngineHandle*)h;
  if (!e || !e->hwnd) return 0;
  // Flutter 的 view 是子窗口，全屏要作用于顶层窗口
  HWND top = GetAncestor(e->hwnd, GA_ROOT);
  if (!top) top = e->hwnd;
  if (on && !e->fullscreen) {
    e->savedPlacement.length = sizeof(WINDOWPLACEMENT);
    GetWindowPlacement(top, &e->savedPlacement);
    e->savedStyle = GetWindowLongPtr(top, GWL_STYLE);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    GetMonitorInfo(MonitorFromWindow(top, MONITOR_DEFAULTTONEAREST), &mi);
    int mw = mi.rcMonitor.right - mi.rcMonitor.left;
    int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    SetWindowLongPtr(top, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(top, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mw, mh, SWP_FRAMECHANGED);
    e->fullscreen = true;
  } else if (!on && e->fullscreen) {
    SetWindowLongPtr(top, GWL_STYLE, e->savedStyle);
    SetWindowPos(top, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    SetWindowPlacement(top, &e->savedPlacement);
    e->fullscreen = false;
  }
  return e->fullscreen ? 1 : 0;
}

// 当前视频帧尺寸（宽/高）：优先取帧输出实际尺寸，无帧时退回源尺寸
int domino_frame_w(void* h) {
  EngineHandle* e = (EngineHandle*)h;
  if (!e || !e->out) return 0;
  std::lock_guard<std::mutex> lk(e->out->lock());
  int w = e->out->width();
  if (w > 0) return w;
  return e->player ? e->player->srcW() : 0;
}
int domino_frame_h(void* h) {
  EngineHandle* e = (EngineHandle*)h;
  if (!e || !e->out) return 0;
  std::lock_guard<std::mutex> lk(e->out->lock());
  int hgt = e->out->height();
  if (hgt > 0) return hgt;
  return e->player ? e->player->srcH() : 0;
}

// ---- 任务中心 ----
static Task* TaskAt(EngineHandle* e, int idx) {
  if (!e || !e->tasks) return nullptr;
  auto& tasks = e->tasks->tasks();
  if (idx < 0 || idx >= (int)tasks.size()) return nullptr;
  return tasks[idx].get();
}
static void CopyOut(const std::string& s, char* buf, int cap) {
  if (!buf || cap <= 0) return;
  int n = (int)s.size();
  if (n >= cap) n = cap - 1;
  memcpy(buf, s.c_str(), (size_t)n);
  buf[n] = 0;
}

int domino_task_create(void* h, const char* source, int targetW, const char* desc) {
  EngineHandle* e = (EngineHandle*)h;
  if (!e || !e->tasks || !source) return -1;
  Task* t = e->tasks->create(source, targetW);
  if (!t) return -1;
  if (desc) t->description = desc;
  e->tasks->save(t);
  return (int)e->tasks->tasks().size() - 1;
}
void domino_task_start(void* h, int idx) {
  EngineHandle* e = (EngineHandle*)h;
  if (Task* t = TaskAt(e, idx)) e->tasks->start(t);
}
void domino_task_stop(void* h, int idx) {
  EngineHandle* e = (EngineHandle*)h;
  if (Task* t = TaskAt(e, idx)) e->tasks->stop(t);
}
void domino_task_remove(void* h, int idx) {
  EngineHandle* e = (EngineHandle*)h;
  if (Task* t = TaskAt(e, idx)) e->tasks->remove(t);
}
int domino_task_count(void* h) {
  EngineHandle* e = (EngineHandle*)h;
  return (e && e->tasks) ? (int)e->tasks->tasks().size() : 0;
}
int domino_task_status(void* h, int idx) {
  Task* t = TaskAt((EngineHandle*)h, idx);
  return t ? t->status.load() : -1;
}
long long domino_task_frames(void* h, int idx) {
  Task* t = TaskAt((EngineHandle*)h, idx);
  if (!t) return 0;
  if (t->status.load() == ST_RUNNING && t->prog) return t->prog->frame.load();
  return t->frames;
}
long long domino_task_total(void* h, int idx) {
  Task* t = TaskAt((EngineHandle*)h, idx);
  if (!t) return 0;
  if (t->status.load() == ST_RUNNING && t->prog) return t->prog->totalFrames.load();
  return t->totalFrames;
}
int domino_task_name(void* h, int idx, char* buf, int cap) {
  Task* t = TaskAt((EngineHandle*)h, idx);
  if (!t) { if (buf && cap > 0) buf[0] = 0; return 0; }
  std::string s = t->source;
  auto slash = s.find_last_of("\\/");
  if (slash != std::string::npos) s = s.substr(slash + 1);
  CopyOut(s, buf, cap);
  return (int)s.size();
}
int domino_task_source(void* h, int idx, char* buf, int cap) {
  Task* t = TaskAt((EngineHandle*)h, idx);
  if (!t) { if (buf && cap > 0) buf[0] = 0; return 0; }
  CopyOut(t->source, buf, cap);
  return (int)t->source.size();
}
int domino_task_temp(void* h, int idx, char* buf, int cap) {
  Task* t = TaskAt((EngineHandle*)h, idx);
  if (!t) { if (buf && cap > 0) buf[0] = 0; return 0; }
  CopyOut(t->temp, buf, cap);
  return (int)t->temp.size();
}
double domino_task_resume_time(void* h, int idx) {
  Task* t = TaskAt((EngineHandle*)h, idx);
  return t ? t->resumeTime : 0;
}

// ---- 系统文件选择框 ----
struct DialogCtx {
  std::wstring folder;   // 初始目录（可为空）
  char* buf = nullptr;   // UTF-8 输出
  int cap = 0;
  int result = 0;        // 1=选中 0=取消/失败
};

// 独立 STA 线程跑模态对话框：规避 UI 线程 COM 公寓状态冲突，对话框自带消息泵。
static DWORD WINAPI dialogThreadProc(LPVOID param) {
  DialogCtx* d = (DialogCtx*)param;
  HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (SUCCEEDED(hr) || hr == S_FALSE || hr == RPC_E_CHANGED_MODE) {
    IFileOpenDialog* pfd = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&pfd)))) {
      COMDLG_FILTERSPEC filters[] = {
          { L"媒体文件", L"*.mp4;*.mkv;*.avi;*.mov;*.webm;*.flv;*.ts;*.mpg;*.mpeg;*.mp3;*.wav;*.flac;*.aac;*.m4a;*.ogg" },
          { L"所有文件", L"*.*" },
      };
      pfd->SetFileTypes(2, filters);
      pfd->SetTitle(L"选择要播放的文件");
      if (!d->folder.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(d->folder.c_str(), nullptr,
                                                  IID_PPV_ARGS(&folder)))) {
          pfd->SetFolder(folder);
          folder->Release();
        }
      }
      HRESULT show = pfd->Show(nullptr);
      if (show == S_OK) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(pfd->GetResult(&item)) && item) {
          PWSTR path = nullptr;
          if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
            int len = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
            if (d->buf && d->cap > 0 && len > 1) {
              if (len > d->cap) len = d->cap;
              WideCharToMultiByte(CP_UTF8, 0, path, -1, d->buf, len, nullptr, nullptr);
              d->buf[len - 1] = 0;
              d->result = 1;
            }
            CoTaskMemFree(path);
          }
          item->Release();
        }
      }
      pfd->Release();
    }
  }
  return 0;
}

// 弹出系统文件选择框；initialPath 提供初始目录，选中返回 1 且 buf 写入 UTF-8 路径，取消返回 0。
int domino_open_dialog(void* h, const char* initialPath, char* buf, int cap) {
  DialogCtx d;
  d.buf = buf;
  d.cap = cap;
  if (initialPath && *initialPath) {
    std::string s(initialPath);
    auto slash = s.find_last_of("\\/");
    std::string dir = slash == std::string::npos ? s : s.substr(0, slash);
    int wlen = MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0);
    if (wlen > 0) {
      d.folder.resize((size_t)wlen);
      MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, &d.folder[0], wlen);
      d.folder.pop_back();  // 去结尾 NUL
    }
  }
  HANDLE th = CreateThread(nullptr, 0, dialogThreadProc, &d, 0, nullptr);
  if (!th) return 0;
  WaitForSingleObject(th, INFINITE);
  CloseHandle(th);
  return d.result;
}

// ---- 滤镜缩略图 ----
int domino_make_previews(void* h, int count) {
  EngineHandle* e = (EngineHandle*)h;
  if (!e || !e->out || count < 1) return 0;
  e->out->makeFilterPreviews(count);
  return e->out->previewCount();
}
int domino_preview_w(void* h) {
  EngineHandle* e = (EngineHandle*)h;
  return (e && e->out) ? e->out->previewWidth() : 0;
}
int domino_preview_h(void* h) {
  EngineHandle* e = (EngineHandle*)h;
  return (e && e->out) ? e->out->previewHeight() : 0;
}
int domino_preview_buf(void* h, int idx, uint8_t* dst, int cap) {
  EngineHandle* e = (EngineHandle*)h;
  if (!e || !e->out) return 0;
  const uint8_t* p = e->out->previewBuffer(idx);
  if (!p) return 0;
  int n = e->out->previewWidth() * e->out->previewHeight() * 3;
  if (dst && cap >= n) memcpy(dst, p, (size_t)n);
  return n;
}
