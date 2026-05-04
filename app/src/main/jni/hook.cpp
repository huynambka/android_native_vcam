#include <android/log.h>
#include <dlfcn.h>
#include <dirent.h>
#include <inttypes.h>
#include <link.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "libyuv_runtime.h"
#include "shadowhook.h"
#include "video2camera_service.h"
#include "video2camera_ipc.h"

#define LOG_TAG "awesomeCAM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" void *g_orig_create_configured_surface = nullptr;
extern "C" void *g_orig_camera3device_create_stream = nullptr;
extern "C" void *g_orig_return_buffer_checked_locked = nullptr;

extern "C" void log_create_configured_surface_result(void *stream_info_ptr,
                                                       uint64_t is_stream_info_valid);
extern "C" __attribute__((naked, noinline)) void hook_create_configured_surface();

namespace {

namespace android {
template <typename T>
class sp;
class Fence;
namespace camera3 {
struct camera_stream {
  int stream_type;
  uint32_t width;
  uint32_t height;
  int format;
  uint64_t usage;
  uint32_t max_buffers;
  void *priv;
  int32_t data_space;
};

struct camera_stream_buffer {
  camera_stream *stream;
  void *buffer;
  int status;
  int acquire_fence;
  int release_fence;
};
}  // namespace camera3
}  // namespace android

struct android_ycbcr {
  void *y;
  void *cb;
  void *cr;
  size_t ystride;
  size_t cstride;
  size_t chroma_step;
  uint32_t reserved[8];
};

constexpr const char *kTargetModuleBasename = "cameraserver";
constexpr uintptr_t kCreateConfiguredSurfaceOffset = 0x2b8e00;
constexpr uintptr_t kCamera3DeviceCreateStreamOffset = 0x1f05c0;
constexpr uintptr_t kReturnBufferCheckedLockedOffset = 0x22cd70;

constexpr const char *kCreateConfiguredSurfaceSymbol =
    "android::camera3::SessionConfigurationUtils::createConfiguredSurface "
    "(createSurfaceFromGbp equivalent)";
constexpr const char *kCamera3DeviceCreateStreamSymbol =
    "android::Camera3Device::createStream(std::vector<SurfaceHolder> const&, ...)";
constexpr const char *kReturnBufferCheckedLockedSymbol =
    "android::camera3::Camera3OutputStream::returnBufferCheckedLocked";

constexpr const char *kSourceMetaPath = "/data/camera/source.meta";
constexpr const char *kSourceFramesDir = "/data/camera/frames";
constexpr const char *kSingleFrameCandidates[] = {
    "/data/camera/frame.i420",   "/data/camera/frame.nv12",
    "/data/camera/frame.nv21",   "/data/camera/input.i420",
    "/data/camera/input.nv12",   "/data/camera/input.nv21",
    "/data/camera/replace.i420", "/data/camera/replace.nv12",
    "/data/camera/replace.nv21", "/data/camera/video.i420",
    "/data/camera/video.nv12",   "/data/camera/video.nv21",
};

constexpr uint32_t kGraphicBufferCpuLockUsage = 0x23;
constexpr uintptr_t kAnwHandleOffset = 0x60;
constexpr uint64_t kSourceRescanIntervalNs = 1000000000ULL;
constexpr int kDefaultSourceFps = 30;
constexpr uint64_t kNsPerSecond = 1000000000ULL;

enum class SourcePixelFormat {
  kUnknown = 0,
  kI420,
  kNV12,
  kNV21,
};

struct OutputStreamInfoPrefix {
  int32_t width;
  int32_t height;
  int32_t format;
  int32_t data_space;
};

struct StreamRecord {
  int stream_id;
  uint32_t width;
  uint32_t height;
  int format;
  int32_t data_space;
  uint64_t consumer_usage;
  int stream_set_id;
  bool deferred;
  bool shared;
  bool multi_resolution;
  int timestamp_base;
  int32_t color_space;
  bool use_readout_timestamp;
  uint64_t return_buffer_log_count;
};

struct SourceDescriptor {
  bool valid = false;
  bool sequence = false;
  std::string path;
  std::string dir;
  int width = 0;
  int height = 0;
  int fps = kDefaultSourceFps;
  SourcePixelFormat format = SourcePixelFormat::kUnknown;
};

struct SourceState {
  std::mutex mutex;
  SourceDescriptor descriptor;
  std::vector<std::string> sequence_paths;
  size_t next_sequence_index = 0;
  std::string loaded_path;
  time_t loaded_mtime = 0;
  off_t loaded_size = 0;
  std::vector<uint8_t> frame_bytes;
  uint64_t frame_interval_ns = kNsPerSecond / kDefaultSourceFps;
  uint64_t next_frame_deadline_ns = 0;
  uint64_t last_scan_ns = 0;
  bool missing_logged = false;
  uint64_t load_generation = 0;
};

enum class ScaledFrameSourceKind {
  kNone = 0,
  kBinder = 1,
  kLocal = 2,
};

struct ScaledFrameCacheEntry {
  int dst_width = 0;
  int dst_height = 0;
  SourceDescriptor source_descriptor;
  std::vector<uint8_t> scaled_i420;
};

struct ScaledFrameCacheState {
  ScaledFrameSourceKind source_kind = ScaledFrameSourceKind::kNone;
  uint64_t source_generation = 0;
  std::vector<ScaledFrameCacheEntry> entries;
};

struct I420View {
  const uint8_t *y = nullptr;
  const uint8_t *u = nullptr;
  const uint8_t *v = nullptr;
  int width = 0;
  int height = 0;
  int y_stride = 0;
  int u_stride = 0;
  int v_stride = 0;
};

struct GraphicBufferSpRet {
  void *ptr;
  void *pad1;
  void *pad2;
};

using GraphicBufferFromFn = GraphicBufferSpRet (*)(void *anw_buffer);
using GraphicBufferLockYCbCrFn = int (*)(void *graphic_buffer, unsigned int usage,
                                         android_ycbcr *layout);
using GraphicBufferLockFn = int (*)(void *graphic_buffer, unsigned int usage,
                                    void **vaddr, int *bytes_per_pixel,
                                    int *bytes_per_stride);
using GraphicBufferUnlockFn = int (*)(void *graphic_buffer);
using RefBaseDecStrongFn = void (*)(void *self, const void *id);

struct GraphicBufferApi {
  std::atomic<bool> resolved{false};
  GraphicBufferFromFn from = nullptr;
  GraphicBufferLockYCbCrFn lock_ycbcr = nullptr;
  GraphicBufferLockFn lock = nullptr;
  GraphicBufferUnlockFn unlock = nullptr;
  RefBaseDecStrongFn dec_strong = nullptr;
};

std::atomic<bool> g_started{false};
std::atomic<uint64_t> g_surface_hit_count{0};
std::atomic<uint64_t> g_create_stream_hit_count{0};
std::atomic<uint64_t> g_return_buffer_hit_count{0};
std::atomic<uint64_t> g_replaced_frame_count{0};

void *g_create_surface_stub = nullptr;
void *g_camera3device_create_stream_stub = nullptr;
void *g_return_buffer_checked_locked_stub = nullptr;

std::mutex g_stream_records_mutex;
std::vector<StreamRecord> g_stream_records;
std::mutex g_frame_process_mutex;
SourceState g_source_state;
ScaledFrameCacheState g_scaled_frame_cache;
GraphicBufferApi g_graphic_buffer_api;
void *g_self_handle = nullptr;

bool ends_with(const char *value, const char *suffix) {
  if (value == nullptr || suffix == nullptr) return false;
  const size_t value_len = strlen(value);
  const size_t suffix_len = strlen(suffix);
  if (suffix_len > value_len) return false;
  return memcmp(value + value_len - suffix_len, suffix, suffix_len) == 0;
}

bool ends_with_ci(const std::string &value, const char *suffix) {
  const size_t suffix_len = strlen(suffix);
  if (suffix_len > value.size()) return false;
  for (size_t i = 0; i < suffix_len; ++i) {
    char a = value[value.size() - suffix_len + i];
    char b = suffix[i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

std::string trim_copy(const std::string &value) {
  size_t begin = 0;
  while (begin < value.size()) {
    const char c = value[begin];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
    ++begin;
  }

  size_t end = value.size();
  while (end > begin) {
    const char c = value[end - 1];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
    --end;
  }

  return value.substr(begin, end - begin);
}

std::string basename_copy(const std::string &path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) return path;
  return path.substr(slash + 1);
}

bool parse_int_value(const std::string &text, int *out) {
  if (out == nullptr) return false;
  char *end = nullptr;
  const long parsed = strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || (end != nullptr && *end != '\0')) return false;
  *out = static_cast<int>(parsed);
  return true;
}

bool parse_dimensions_from_string(const std::string &text, int *width, int *height) {
  if (width == nullptr || height == nullptr) return false;
  for (size_t i = 0; i < text.size(); ++i) {
    int w = 0;
    int h = 0;
    if (sscanf(text.c_str() + i, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
      *width = w;
      *height = h;
      return true;
    }
  }
  return false;
}

const char *source_pixel_format_name(SourcePixelFormat format) {
  switch (format) {
    case SourcePixelFormat::kI420:
      return "I420";
    case SourcePixelFormat::kNV12:
      return "NV12";
    case SourcePixelFormat::kNV21:
      return "NV21";
    default:
      return "unknown";
  }
}

SourcePixelFormat infer_source_pixel_format(const std::string &path) {
  if (ends_with_ci(path, ".i420")) return SourcePixelFormat::kI420;
  if (ends_with_ci(path, ".nv12")) return SourcePixelFormat::kNV12;
  if (ends_with_ci(path, ".nv21")) return SourcePixelFormat::kNV21;
  return SourcePixelFormat::kUnknown;
}

bool parse_source_pixel_format(const std::string &text, SourcePixelFormat *out) {
  if (out == nullptr) return false;
  std::string lower = trim_copy(text);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
    return static_cast<char>(::tolower(c));
  });
  if (lower == "i420") {
    *out = SourcePixelFormat::kI420;
    return true;
  }
  if (lower == "nv12") {
    *out = SourcePixelFormat::kNV12;
    return true;
  }
  if (lower == "nv21") {
    *out = SourcePixelFormat::kNV21;
    return true;
  }
  return false;
}

uint64_t monotonic_time_ns() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * kNsPerSecond +
         static_cast<uint64_t>(ts.tv_nsec);
}

bool stat_path(const std::string &path, struct stat *st) {
  if (st == nullptr) return false;
  return stat(path.c_str(), st) == 0;
}

bool is_directory_path(const std::string &path) {
  struct stat st {};
  return stat_path(path, &st) && S_ISDIR(st.st_mode);
}

bool load_file_bytes(const std::string &path, std::vector<uint8_t> *out, time_t *mtime,
                     off_t *size) {
  if (out == nullptr) return false;

  struct stat st {};
  if (!stat_path(path, &st) || !S_ISREG(st.st_mode)) return false;

  FILE *fp = fopen(path.c_str(), "rb");
  if (fp == nullptr) return false;

  std::vector<uint8_t> bytes(static_cast<size_t>(st.st_size));
  const size_t read = bytes.empty() ? 0 : fread(bytes.data(), 1, bytes.size(), fp);
  fclose(fp);
  if (read != bytes.size()) return false;

  *out = std::move(bytes);
  if (mtime != nullptr) *mtime = st.st_mtime;
  if (size != nullptr) *size = st.st_size;
  return true;
}

std::vector<std::string> list_supported_frame_files(const std::string &dir) {
  std::vector<std::string> files;
  DIR *dp = opendir(dir.c_str());
  if (dp == nullptr) return files;

  while (struct dirent *entry = readdir(dp)) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    std::string full = dir;
    if (!full.empty() && full.back() != '/') full.push_back('/');
    full += entry->d_name;

    struct stat st {};
    if (!stat_path(full, &st) || !S_ISREG(st.st_mode)) continue;
    if (infer_source_pixel_format(full) == SourcePixelFormat::kUnknown) continue;
    files.push_back(full);
  }

  closedir(dp);
  std::sort(files.begin(), files.end());
  return files;
}

bool parse_source_meta(SourceDescriptor *descriptor) {
  if (descriptor == nullptr) return false;

  FILE *fp = fopen(kSourceMetaPath, "re");
  if (fp == nullptr) return false;

  SourceDescriptor meta;
  meta.fps = kDefaultSourceFps;

  char line[512];
  while (fgets(line, sizeof(line), fp) != nullptr) {
    std::string raw = trim_copy(line);
    if (raw.empty() || raw[0] == '#') continue;
    const size_t eq = raw.find('=');
    if (eq == std::string::npos) continue;

    const std::string key = trim_copy(raw.substr(0, eq));
    const std::string value = trim_copy(raw.substr(eq + 1));

    if (key == "path") {
      meta.path = value;
    } else if (key == "dir") {
      meta.dir = value;
    } else if (key == "width") {
      parse_int_value(value, &meta.width);
    } else if (key == "height") {
      parse_int_value(value, &meta.height);
    } else if (key == "fps") {
      parse_int_value(value, &meta.fps);
    } else if (key == "format") {
      parse_source_pixel_format(value, &meta.format);
    }
  }

  fclose(fp);

  meta.sequence = !meta.dir.empty();
  meta.valid = meta.sequence ? !meta.dir.empty() : !meta.path.empty();
  *descriptor = meta;
  return meta.valid;
}

bool infer_dimensions_from_file(const std::string &path, off_t file_size, int hint_width,
                                int hint_height, int *width, int *height) {
  if (width == nullptr || height == nullptr) return false;

  if (parse_dimensions_from_string(basename_copy(path), width, height)) return true;

  if (hint_width > 0 && hint_height > 0) {
    const size_t chroma_width = static_cast<size_t>((hint_width + 1) / 2);
    const size_t chroma_height = static_cast<size_t>((hint_height + 1) / 2);
    const size_t expected = static_cast<size_t>(hint_width) * hint_height +
                            2 * chroma_width * chroma_height;
    if (static_cast<off_t>(expected) == file_size) {
      *width = hint_width;
      *height = hint_height;
      return true;
    }
  }

  return false;
}

SourceDescriptor discover_source_descriptor(int hint_width, int hint_height,
                                            std::vector<std::string> *sequence_paths) {
  SourceDescriptor descriptor;
  if (sequence_paths != nullptr) sequence_paths->clear();

  if (parse_source_meta(&descriptor)) {
    if (descriptor.sequence) {
      if (sequence_paths != nullptr) {
        *sequence_paths = list_supported_frame_files(descriptor.dir);
      }
      descriptor.valid = sequence_paths != nullptr && !sequence_paths->empty();
      if (descriptor.valid && descriptor.format == SourcePixelFormat::kUnknown) {
        descriptor.format = infer_source_pixel_format((*sequence_paths)[0]);
      }
      if (descriptor.valid && (descriptor.width <= 0 || descriptor.height <= 0)) {
        struct stat st {};
        if (stat_path((*sequence_paths)[0], &st)) {
          infer_dimensions_from_file((*sequence_paths)[0], st.st_size, hint_width,
                                     hint_height, &descriptor.width,
                                     &descriptor.height);
        }
      }
      return descriptor;
    }

    descriptor.sequence = false;
    descriptor.valid = !descriptor.path.empty();
    if (descriptor.valid && descriptor.format == SourcePixelFormat::kUnknown) {
      descriptor.format = infer_source_pixel_format(descriptor.path);
    }
    if (descriptor.valid && (descriptor.width <= 0 || descriptor.height <= 0)) {
      struct stat st {};
      if (stat_path(descriptor.path, &st)) {
        infer_dimensions_from_file(descriptor.path, st.st_size, hint_width, hint_height,
                                   &descriptor.width, &descriptor.height);
      }
    }
    return descriptor;
  }

  if (is_directory_path(kSourceFramesDir)) {
    std::vector<std::string> discovered = list_supported_frame_files(kSourceFramesDir);
    if (!discovered.empty()) {
      descriptor.valid = true;
      descriptor.sequence = true;
      descriptor.dir = kSourceFramesDir;
      descriptor.fps = kDefaultSourceFps;
      descriptor.format = infer_source_pixel_format(discovered[0]);
      if (sequence_paths != nullptr) *sequence_paths = discovered;
      struct stat st {};
      if (stat_path(discovered[0], &st)) {
        infer_dimensions_from_file(discovered[0], st.st_size, hint_width, hint_height,
                                   &descriptor.width, &descriptor.height);
      }
      return descriptor;
    }
  }

  for (const char *candidate : kSingleFrameCandidates) {
    struct stat st {};
    if (!stat_path(candidate, &st) || !S_ISREG(st.st_mode)) continue;

    descriptor.valid = true;
    descriptor.sequence = false;
    descriptor.path = candidate;
    descriptor.fps = kDefaultSourceFps;
    descriptor.format = infer_source_pixel_format(candidate);
    infer_dimensions_from_file(candidate, st.st_size, hint_width, hint_height,
                               &descriptor.width, &descriptor.height);
    return descriptor;
  }

  descriptor.valid = false;
  return descriptor;
}

bool ensure_source_frame_locked(int hint_width, int hint_height,
                                SourceDescriptor *active_descriptor) {
  const uint64_t now_ns = monotonic_time_ns();
  const bool should_rescan =
      (g_source_state.last_scan_ns == 0 ||
       now_ns - g_source_state.last_scan_ns >= kSourceRescanIntervalNs ||
       !g_source_state.descriptor.valid);

  if (should_rescan) {
    std::vector<std::string> sequence_paths;
    SourceDescriptor discovered =
        discover_source_descriptor(hint_width, hint_height, &sequence_paths);
    g_source_state.last_scan_ns = now_ns;

    if (!discovered.valid || discovered.format == SourcePixelFormat::kUnknown ||
        discovered.width <= 0 || discovered.height <= 0) {
      g_source_state.descriptor = SourceDescriptor{};
      g_source_state.sequence_paths.clear();
      g_source_state.frame_bytes.clear();
      g_source_state.loaded_path.clear();
      g_source_state.loaded_mtime = 0;
      g_source_state.loaded_size = 0;
      if (!g_source_state.missing_logged) {
        LOGW("No usable source under /data/camera (expected source.meta, frames/*.i420|nv12|nv21, or frame/input/replace/video raw files)");
        g_source_state.missing_logged = true;
      }
      return false;
    }

    g_source_state.descriptor = discovered;
    g_source_state.sequence_paths = std::move(sequence_paths);
    g_source_state.frame_interval_ns =
        kNsPerSecond /
        static_cast<uint64_t>(std::max(1, std::min(240, discovered.fps)));
    g_source_state.missing_logged = false;

    if (!discovered.sequence) {
      g_source_state.next_sequence_index = 0;
    } else if (!g_source_state.sequence_paths.empty()) {
      g_source_state.next_sequence_index %= g_source_state.sequence_paths.size();
    }
  }

  if (!g_source_state.descriptor.valid) return false;

  bool need_reload = false;
  std::string selected_path;

  if (g_source_state.descriptor.sequence) {
    if (g_source_state.sequence_paths.empty()) return false;
    if (g_source_state.loaded_path.empty() || now_ns >= g_source_state.next_frame_deadline_ns) {
      selected_path = g_source_state.sequence_paths[g_source_state.next_sequence_index];
      g_source_state.next_sequence_index =
          (g_source_state.next_sequence_index + 1) % g_source_state.sequence_paths.size();
      g_source_state.next_frame_deadline_ns = now_ns + g_source_state.frame_interval_ns;
      need_reload = true;
    } else {
      selected_path = g_source_state.loaded_path;
    }
  } else {
    selected_path = g_source_state.descriptor.path;
    struct stat st {};
    if (!stat_path(selected_path, &st)) return false;
    if (g_source_state.loaded_path != selected_path ||
        g_source_state.loaded_mtime != st.st_mtime ||
        g_source_state.loaded_size != st.st_size || g_source_state.frame_bytes.empty()) {
      need_reload = true;
    }
  }

  if (need_reload) {
    std::vector<uint8_t> bytes;
    time_t mtime = 0;
    off_t size = 0;
    if (!load_file_bytes(selected_path, &bytes, &mtime, &size)) {
      LOGE("Failed to load source frame: %s", selected_path.c_str());
      return false;
    }

    int width = g_source_state.descriptor.width;
    int height = g_source_state.descriptor.height;
    if ((width <= 0 || height <= 0) &&
        !infer_dimensions_from_file(selected_path, size, hint_width, hint_height, &width,
                                    &height)) {
      LOGE("Cannot infer source dimensions for %s (size=%jd)", selected_path.c_str(),
           static_cast<intmax_t>(size));
      return false;
    }

    const size_t chroma_width = static_cast<size_t>((width + 1) / 2);
    const size_t chroma_height = static_cast<size_t>((height + 1) / 2);
    const size_t expected_size = static_cast<size_t>(width) * height +
                                 2 * chroma_width * chroma_height;
    if (bytes.size() < expected_size) {
      LOGE("Source frame too small: %s size=%zu expected=%zu", selected_path.c_str(),
           bytes.size(), expected_size);
      return false;
    }

    g_source_state.descriptor.width = width;
    g_source_state.descriptor.height = height;
    g_source_state.frame_bytes = std::move(bytes);
    g_source_state.loaded_path = selected_path;
    g_source_state.loaded_mtime = mtime;
    g_source_state.loaded_size = size;
    g_source_state.load_generation += 1;

    LOGI("Loaded source #%llu path=%s width=%d height=%d format=%s fps=%d sequence=%d",
         static_cast<unsigned long long>(g_source_state.load_generation),
         selected_path.c_str(), g_source_state.descriptor.width,
         g_source_state.descriptor.height,
         source_pixel_format_name(g_source_state.descriptor.format),
         g_source_state.descriptor.fps, g_source_state.descriptor.sequence);
  }

  if (active_descriptor != nullptr) {
    *active_descriptor = g_source_state.descriptor;
  }
  return !g_source_state.frame_bytes.empty();
}

bool resolve_graphic_buffer_api() {
  if (g_graphic_buffer_api.resolved.load(std::memory_order_acquire)) {
    return g_graphic_buffer_api.from != nullptr && g_graphic_buffer_api.lock_ycbcr != nullptr &&
           g_graphic_buffer_api.lock != nullptr && g_graphic_buffer_api.unlock != nullptr &&
           g_graphic_buffer_api.dec_strong != nullptr;
  }

  g_graphic_buffer_api.from = reinterpret_cast<GraphicBufferFromFn>(
      dlsym(RTLD_DEFAULT, "_ZN7android13GraphicBuffer4fromEP19ANativeWindowBuffer"));
  g_graphic_buffer_api.lock_ycbcr = reinterpret_cast<GraphicBufferLockYCbCrFn>(
      dlsym(RTLD_DEFAULT, "_ZN7android13GraphicBuffer9lockYCbCrEjP13android_ycbcr"));
  g_graphic_buffer_api.lock = reinterpret_cast<GraphicBufferLockFn>(
      dlsym(RTLD_DEFAULT, "_ZN7android13GraphicBuffer4lockEjPPvPiS3_"));
  g_graphic_buffer_api.unlock = reinterpret_cast<GraphicBufferUnlockFn>(
      dlsym(RTLD_DEFAULT, "_ZN7android13GraphicBuffer6unlockEv"));
  g_graphic_buffer_api.dec_strong = reinterpret_cast<RefBaseDecStrongFn>(
      dlsym(RTLD_DEFAULT, "_ZNK7android7RefBase9decStrongEPKv"));
  g_graphic_buffer_api.resolved.store(true, std::memory_order_release);

  const bool ok = g_graphic_buffer_api.from != nullptr &&
                  g_graphic_buffer_api.lock_ycbcr != nullptr &&
                  g_graphic_buffer_api.lock != nullptr &&
                  g_graphic_buffer_api.unlock != nullptr &&
                  g_graphic_buffer_api.dec_strong != nullptr;
  if (!ok) {
    LOGE("Failed to resolve GraphicBuffer symbols from=%p lockYCbCr=%p lock=%p unlock=%p decStrong=%p",
         reinterpret_cast<void *>(g_graphic_buffer_api.from),
         reinterpret_cast<void *>(g_graphic_buffer_api.lock_ycbcr),
         reinterpret_cast<void *>(g_graphic_buffer_api.lock),
         reinterpret_cast<void *>(g_graphic_buffer_api.unlock),
         reinterpret_cast<void *>(g_graphic_buffer_api.dec_strong));
  }
  return ok;
}

uintptr_t find_exe_base_from_maps() {
  FILE *fp = fopen("/proc/self/maps", "re");
  if (fp == nullptr) return 0;

  char line[1024];
  while (fgets(line, sizeof(line), fp) != nullptr) {
    if (strstr(line, "/system/bin/cameraserver") == nullptr) continue;
    if (strstr(line, "00000000") == nullptr) continue;

    uintptr_t start = 0;
    if (sscanf(line, "%lx-%*lx", &start) == 1) {
      fclose(fp);
      return start;
    }
  }

  fclose(fp);
  return 0;
}

struct ModuleSearch {
  uintptr_t base = 0;
  char exe_path[PATH_MAX] = {};
};

int find_module_base_cb(struct dl_phdr_info *info, size_t, void *data) {
  auto *search = static_cast<ModuleSearch *>(data);

  const char *name = info->dlpi_name;
  if (name == nullptr || name[0] == '\0') {
    if (search->exe_path[0] == '\0') {
      const ssize_t len = readlink("/proc/self/exe", search->exe_path,
                                   sizeof(search->exe_path) - 1);
      if (len > 0) {
        search->exe_path[len] = '\0';
      }
    }
    name = search->exe_path;
  }

  if (name != nullptr && ends_with(name, kTargetModuleBasename)) {
    search->base = static_cast<uintptr_t>(info->dlpi_addr);
    return 1;
  }

  return 0;
}

uintptr_t find_cameraserver_base() {
  ModuleSearch search;
  dl_iterate_phdr(find_module_base_cb, &search);
  if (search.base != 0) return search.base;
  return find_exe_base_from_maps();
}

void remember_stream_record(int stream_id, uint32_t width, uint32_t height, int format,
                            int32_t data_space, uint64_t consumer_usage,
                            int stream_set_id, bool deferred, bool shared,
                            bool multi_resolution, int timestamp_base,
                            int32_t color_space, bool use_readout_timestamp) {
  if (stream_id < 0) return;

  std::lock_guard<std::mutex> lock(g_stream_records_mutex);
  for (auto &record : g_stream_records) {
    if (record.stream_id == stream_id) {
      record.width = width;
      record.height = height;
      record.format = format;
      record.data_space = data_space;
      record.consumer_usage = consumer_usage;
      record.stream_set_id = stream_set_id;
      record.deferred = deferred;
      record.shared = shared;
      record.multi_resolution = multi_resolution;
      record.timestamp_base = timestamp_base;
      record.color_space = color_space;
      record.use_readout_timestamp = use_readout_timestamp;
      return;
    }
  }

  g_stream_records.push_back(StreamRecord{stream_id,
                                          width,
                                          height,
                                          format,
                                          data_space,
                                          consumer_usage,
                                          stream_set_id,
                                          deferred,
                                          shared,
                                          multi_resolution,
                                          timestamp_base,
                                          color_space,
                                          use_readout_timestamp,
                                          0});
}

bool find_stream_record_by_props(uint32_t width, uint32_t height, int format,
                                 int32_t data_space, StreamRecord *out,
                                 uint64_t *log_count_after_increment) {
  std::lock_guard<std::mutex> lock(g_stream_records_mutex);

  auto exact_match = [&](StreamRecord &record) {
    return record.width == width && record.height == height && record.format == format &&
           record.data_space == data_space;
  };

  auto soft_match = [&](StreamRecord &record) {
    return record.width == width && record.height == height &&
           ((record.format == format) || (record.format == 0x22 && format == 0x23));
  };

  for (auto &record : g_stream_records) {
    if (!exact_match(record)) continue;
    record.return_buffer_log_count += 1;
    if (out != nullptr) *out = record;
    if (log_count_after_increment != nullptr) {
      *log_count_after_increment = record.return_buffer_log_count;
    }
    return true;
  }

  for (auto &record : g_stream_records) {
    if (!soft_match(record)) continue;
    record.return_buffer_log_count += 1;
    if (record.format == 0x22 && format == 0x23) {
      record.format = format;
      record.data_space = data_space;
    }
    if (out != nullptr) *out = record;
    if (log_count_after_increment != nullptr) {
      *log_count_after_increment = record.return_buffer_log_count;
    }
    return true;
  }

  return false;
}

size_t chroma_width_for(int width) { return static_cast<size_t>((width + 1) / 2); }
size_t chroma_height_for(int height) { return static_cast<size_t>((height + 1) / 2); }

bool build_source_i420_view_locked(const SourceDescriptor &descriptor,
                                   const std::vector<uint8_t> &frame_bytes,
                                   std::vector<uint8_t> *scratch,
                                   I420View *view) {
  if (view == nullptr || descriptor.width <= 0 || descriptor.height <= 0) return false;

  const size_t y_size = static_cast<size_t>(descriptor.width) * descriptor.height;
  const size_t chroma_width = chroma_width_for(descriptor.width);
  const size_t chroma_height = chroma_height_for(descriptor.height);
  const size_t chroma_size = chroma_width * chroma_height;
  const size_t required_size = y_size + 2 * chroma_size;
  if (frame_bytes.size() < required_size) return false;

  if (descriptor.format == SourcePixelFormat::kI420) {
    view->width = descriptor.width;
    view->height = descriptor.height;
    view->y = frame_bytes.data();
    view->u = frame_bytes.data() + y_size;
    view->v = frame_bytes.data() + y_size + chroma_size;
    view->y_stride = descriptor.width;
    view->u_stride = static_cast<int>(chroma_width);
    view->v_stride = static_cast<int>(chroma_width);
    return true;
  }

  if (scratch == nullptr) return false;
  scratch->resize(required_size);
  uint8_t *dst_y = scratch->data();
  uint8_t *dst_u = dst_y + y_size;
  uint8_t *dst_v = dst_u + chroma_size;
  memcpy(dst_y, frame_bytes.data(), y_size);

  const uint8_t *src_uv = frame_bytes.data() + y_size;
  for (size_t y = 0; y < chroma_height; ++y) {
    const uint8_t *src_row = src_uv + y * chroma_width * 2;
    uint8_t *u_row = dst_u + y * chroma_width;
    uint8_t *v_row = dst_v + y * chroma_width;
    for (size_t x = 0; x < chroma_width; ++x) {
      const uint8_t a = src_row[x * 2 + 0];
      const uint8_t b = src_row[x * 2 + 1];
      if (descriptor.format == SourcePixelFormat::kNV12) {
        u_row[x] = a;
        v_row[x] = b;
      } else {
        v_row[x] = a;
        u_row[x] = b;
      }
    }
  }

  view->width = descriptor.width;
  view->height = descriptor.height;
  view->y = dst_y;
  view->u = dst_u;
  view->v = dst_v;
  view->y_stride = descriptor.width;
  view->u_stride = static_cast<int>(chroma_width);
  view->v_stride = static_cast<int>(chroma_width);
  return true;
}

void scale_plane_crop_nn(const uint8_t *src, int src_stride, int crop_x, int crop_y,
                         int crop_w, int crop_h, uint8_t *dst, int dst_stride,
                         int dst_w, int dst_h) {
  for (int y = 0; y < dst_h; ++y) {
    const int src_y = crop_y + (y * crop_h) / std::max(1, dst_h);
    const uint8_t *src_row = src + src_y * src_stride;
    uint8_t *dst_row = dst + y * dst_stride;
    for (int x = 0; x < dst_w; ++x) {
      const int src_x = crop_x + (x * crop_w) / std::max(1, dst_w);
      dst_row[x] = src_row[src_x];
    }
  }
}

void compute_center_crop(int src_w, int src_h, int dst_w, int dst_h, int *crop_x,
                         int *crop_y, int *crop_w, int *crop_h) {
  if (crop_x == nullptr || crop_y == nullptr || crop_w == nullptr || crop_h == nullptr) {
    return;
  }

  int out_x = 0;
  int out_y = 0;
  int out_w = src_w;
  int out_h = src_h;

  if (src_w > 0 && src_h > 0 && dst_w > 0 && dst_h > 0) {
    const int64_t lhs = static_cast<int64_t>(src_w) * dst_h;
    const int64_t rhs = static_cast<int64_t>(dst_w) * src_h;
    if (lhs > rhs) {
      out_w = static_cast<int>((static_cast<int64_t>(src_h) * dst_w) / dst_h);
      out_x = (src_w - out_w) / 2;
    } else if (lhs < rhs) {
      out_h = static_cast<int>((static_cast<int64_t>(src_w) * dst_h) / dst_w);
      out_y = (src_h - out_h) / 2;
    }
  }

  out_x &= ~1;
  out_y &= ~1;
  if ((out_w & 1) != 0 && out_w > 1) --out_w;
  if ((out_h & 1) != 0 && out_h > 1) --out_h;
  out_w = std::max(2, out_w);
  out_h = std::max(2, out_h);

  *crop_x = out_x;
  *crop_y = out_y;
  *crop_w = out_w;
  *crop_h = out_h;
}

bool build_scaled_i420_locked(const SourceDescriptor &descriptor,
                              const std::vector<uint8_t> &frame_bytes, int dst_width,
                              int dst_height, std::vector<uint8_t> *scaled_i420) {
  if (scaled_i420 == nullptr || dst_width <= 0 || dst_height <= 0) return false;

  std::vector<uint8_t> source_scratch;
  I420View src;
  if (!build_source_i420_view_locked(descriptor, frame_bytes, &source_scratch, &src)) {
    return false;
  }

  const size_t dst_y_size = static_cast<size_t>(dst_width) * dst_height;
  const size_t dst_chroma_width = chroma_width_for(dst_width);
  const size_t dst_chroma_height = chroma_height_for(dst_height);
  const size_t dst_chroma_size = dst_chroma_width * dst_chroma_height;
  scaled_i420->resize(dst_y_size + 2 * dst_chroma_size);

  uint8_t *dst_y = scaled_i420->data();
  uint8_t *dst_u = dst_y + dst_y_size;
  uint8_t *dst_v = dst_u + dst_chroma_size;

  int crop_x = 0;
  int crop_y = 0;
  int crop_w = src.width;
  int crop_h = src.height;
  compute_center_crop(src.width, src.height, dst_width, dst_height, &crop_x, &crop_y,
                      &crop_w, &crop_h);

  const uint8_t *src_y = src.y + static_cast<size_t>(crop_y) * src.y_stride + crop_x;
  const uint8_t *src_u =
      src.u + static_cast<size_t>(crop_y / 2) * src.u_stride + (crop_x / 2);
  const uint8_t *src_v =
      src.v + static_cast<size_t>(crop_y / 2) * src.v_stride + (crop_x / 2);
  constexpr int kLibYuvFilterBilinear = 2;
  if (awesomecam::LibYuvI420Scale(src_y, src.y_stride,
                                  src_u, src.u_stride,
                                  src_v, src.v_stride,
                                  crop_w, crop_h,
                                  dst_y, dst_width,
                                  dst_u, static_cast<int>(dst_chroma_width),
                                  dst_v, static_cast<int>(dst_chroma_width),
                                  dst_width, dst_height,
                                  kLibYuvFilterBilinear)) {
    return true;
  }

  scale_plane_crop_nn(src.y, src.y_stride, crop_x, crop_y, crop_w, crop_h, dst_y,
                      dst_width, dst_width, dst_height);
  scale_plane_crop_nn(src.u, src.u_stride, crop_x / 2, crop_y / 2, crop_w / 2,
                      crop_h / 2, dst_u, static_cast<int>(dst_chroma_width),
                      static_cast<int>(dst_chroma_width),
                      static_cast<int>(dst_chroma_height));
  scale_plane_crop_nn(src.v, src.v_stride, crop_x / 2, crop_y / 2, crop_w / 2,
                      crop_h / 2, dst_v, static_cast<int>(dst_chroma_width),
                      static_cast<int>(dst_chroma_width),
                      static_cast<int>(dst_chroma_height));
  return true;
}

const ScaledFrameCacheEntry *find_scaled_frame_cache_entry_locked(
    ScaledFrameSourceKind source_kind, uint64_t source_generation, int dst_width,
    int dst_height) {
  if (g_scaled_frame_cache.source_kind != source_kind ||
      g_scaled_frame_cache.source_generation != source_generation) {
    return nullptr;
  }
  for (const auto &entry : g_scaled_frame_cache.entries) {
    if (entry.dst_width == dst_width && entry.dst_height == dst_height) {
      return &entry;
    }
  }
  return nullptr;
}

void reset_scaled_frame_cache_locked(ScaledFrameSourceKind source_kind,
                                     uint64_t source_generation) {
  if (g_scaled_frame_cache.source_kind == source_kind &&
      g_scaled_frame_cache.source_generation == source_generation) {
    return;
  }
  g_scaled_frame_cache.source_kind = source_kind;
  g_scaled_frame_cache.source_generation = source_generation;
  g_scaled_frame_cache.entries.clear();
}

const ScaledFrameCacheEntry *store_scaled_frame_cache_entry_locked(
    ScaledFrameSourceKind source_kind, uint64_t source_generation, int dst_width,
    int dst_height, const SourceDescriptor &source_descriptor,
    std::vector<uint8_t> &&scaled_i420) {
  reset_scaled_frame_cache_locked(source_kind, source_generation);
  for (auto &entry : g_scaled_frame_cache.entries) {
    if (entry.dst_width == dst_width && entry.dst_height == dst_height) {
      entry.source_descriptor = source_descriptor;
      entry.scaled_i420 = std::move(scaled_i420);
      return &entry;
    }
  }
  g_scaled_frame_cache.entries.push_back(
      ScaledFrameCacheEntry{dst_width, dst_height, source_descriptor,
                            std::move(scaled_i420)});
  return &g_scaled_frame_cache.entries.back();
}

bool load_latest_scaled_i420_for_dst(int dst_width, int dst_height,
                                     SourceDescriptor *source_descriptor,
                                     const std::vector<uint8_t> **scaled_i420) {
  if (source_descriptor == nullptr || scaled_i420 == nullptr || dst_width <= 0 ||
      dst_height <= 0) {
    return false;
  }

  *source_descriptor = SourceDescriptor{};
  *scaled_i420 = nullptr;

  awesomecam::BinderFrameState binder_state;
  if (awesomecam::PeekLatestBinderFrameState(&binder_state) &&
      binder_state.format == awesomecam::kFrameFormatI420) {
    if (const auto *cached = find_scaled_frame_cache_entry_locked(
            ScaledFrameSourceKind::kBinder, binder_state.generation, dst_width,
            dst_height)) {
      *source_descriptor = cached->source_descriptor;
      *scaled_i420 = &cached->scaled_i420;
      return true;
    }

    awesomecam::BinderFrameCopy binder_frame;
    if (awesomecam::CopyLatestBinderFrame(&binder_frame) &&
        binder_frame.format == awesomecam::kFrameFormatI420) {
      SourceDescriptor binder_descriptor;
      binder_descriptor.valid = true;
      binder_descriptor.sequence = false;
      binder_descriptor.path = "binder://Video2CameraService";
      binder_descriptor.width = binder_frame.width;
      binder_descriptor.height = binder_frame.height;
      binder_descriptor.fps = kDefaultSourceFps;
      binder_descriptor.format = SourcePixelFormat::kI420;

      std::vector<uint8_t> built_i420;
      if (!build_scaled_i420_locked(binder_descriptor, binder_frame.bytes, dst_width,
                                    dst_height, &built_i420)) {
        LOGE("Failed to scale binder frame gen=%llu %dx%d for %dx%d",
             static_cast<unsigned long long>(binder_frame.generation),
             binder_frame.width, binder_frame.height, dst_width, dst_height);
      } else {
        const auto *cached = store_scaled_frame_cache_entry_locked(
            ScaledFrameSourceKind::kBinder, binder_frame.generation, dst_width,
            dst_height, binder_descriptor, std::move(built_i420));
        *source_descriptor = cached->source_descriptor;
        *scaled_i420 = &cached->scaled_i420;
        return true;
      }
    }
  }

  std::lock_guard<std::mutex> source_lock(g_source_state.mutex);
  if (!ensure_source_frame_locked(dst_width, dst_height, source_descriptor)) {
    return false;
  }

  if (const auto *cached = find_scaled_frame_cache_entry_locked(
          ScaledFrameSourceKind::kLocal, g_source_state.load_generation, dst_width,
          dst_height)) {
    *source_descriptor = cached->source_descriptor;
    *scaled_i420 = &cached->scaled_i420;
    return true;
  }

  std::vector<uint8_t> built_i420;
  if (!build_scaled_i420_locked(*source_descriptor, g_source_state.frame_bytes, dst_width,
                                dst_height, &built_i420)) {
    LOGE("Failed to build scaled I420 for %dx%d from %s", dst_width, dst_height,
         source_pixel_format_name(source_descriptor->format));
    return false;
  }

  const auto *cached = store_scaled_frame_cache_entry_locked(
      ScaledFrameSourceKind::kLocal, g_source_state.load_generation, dst_width,
      dst_height, *source_descriptor, std::move(built_i420));
  *source_descriptor = cached->source_descriptor;
  *scaled_i420 = &cached->scaled_i420;
  return true;
}

void copy_plane_rows(uint8_t *dst, size_t dst_stride, const uint8_t *src, int src_stride,
                     int row_width, int rows) {
  for (int y = 0; y < rows; ++y) {
    memcpy(dst + y * dst_stride, src + y * src_stride, static_cast<size_t>(row_width));
  }
}

bool write_i420_to_ycbcr(const std::vector<uint8_t> &scaled_i420, int width, int height,
                         const android_ycbcr &layout) {
  if (width <= 0 || height <= 0 || layout.y == nullptr || layout.cb == nullptr ||
      layout.cr == nullptr) {
    return false;
  }

  const size_t y_size = static_cast<size_t>(width) * height;
  const size_t chroma_width = chroma_width_for(width);
  const size_t chroma_height = chroma_height_for(height);
  const size_t chroma_size = chroma_width * chroma_height;
  if (scaled_i420.size() < y_size + 2 * chroma_size) return false;

  const uint8_t *src_y = scaled_i420.data();
  const uint8_t *src_u = src_y + y_size;
  const uint8_t *src_v = src_u + chroma_size;

  auto *dst_y = static_cast<uint8_t *>(layout.y);
  auto *dst_cb = static_cast<uint8_t *>(layout.cb);
  auto *dst_cr = static_cast<uint8_t *>(layout.cr);

  copy_plane_rows(dst_y, layout.ystride, src_y, width, width, height);

  if (layout.chroma_step == 1) {
    copy_plane_rows(dst_cb, layout.cstride, src_u, static_cast<int>(chroma_width),
                    static_cast<int>(chroma_width), static_cast<int>(chroma_height));
    copy_plane_rows(dst_cr, layout.cstride, src_v, static_cast<int>(chroma_width),
                    static_cast<int>(chroma_width), static_cast<int>(chroma_height));
    return true;
  }

  if (layout.chroma_step == 2) {
    const bool nv12_layout = dst_cb < dst_cr;
    uint8_t *dst_uv = nv12_layout ? dst_cb : dst_cr;
    for (size_t y = 0; y < chroma_height; ++y) {
      const uint8_t *u_row = src_u + y * chroma_width;
      const uint8_t *v_row = src_v + y * chroma_width;
      uint8_t *dst_row = dst_uv + y * layout.cstride;
      for (size_t x = 0; x < chroma_width; ++x) {
        if (nv12_layout) {
          dst_row[x * 2 + 0] = u_row[x];
          dst_row[x * 2 + 1] = v_row[x];
        } else {
          dst_row[x * 2 + 0] = v_row[x];
          dst_row[x * 2 + 1] = u_row[x];
        }
      }
    }
    return true;
  }

  for (size_t y = 0; y < chroma_height; ++y) {
    uint8_t *cb_row = dst_cb + y * layout.cstride;
    uint8_t *cr_row = dst_cr + y * layout.cstride;
    const uint8_t *u_row = src_u + y * chroma_width;
    const uint8_t *v_row = src_v + y * chroma_width;
    for (size_t x = 0; x < chroma_width; ++x) {
      cb_row[x * layout.chroma_step] = u_row[x];
      cr_row[x * layout.chroma_step] = v_row[x];
    }
  }

  return true;
}

bool try_replace_camera3_frame(
    const android::camera3::camera_stream_buffer &buffer, int32_t surface_id,
    StreamRecord *matched_stream) {
  if (buffer.stream == nullptr || buffer.buffer == nullptr) return false;
  if (buffer.stream->format != 0x23) return false;
  if (!resolve_graphic_buffer_api()) return false;

  SourceDescriptor source_descriptor;
  const std::vector<uint8_t> *scaled_i420 = nullptr;
  if (!load_latest_scaled_i420_for_dst(static_cast<int>(buffer.stream->width),
                                       static_cast<int>(buffer.stream->height),
                                       &source_descriptor, &scaled_i420) ||
      scaled_i420 == nullptr) {
    return false;
  }

  void *anw_buffer = reinterpret_cast<void *>(
      reinterpret_cast<uintptr_t>(buffer.buffer) - kAnwHandleOffset);
  GraphicBufferSpRet graphic_buffer_ref = g_graphic_buffer_api.from(anw_buffer);
  void *graphic_buffer = graphic_buffer_ref.ptr;
  if (graphic_buffer == nullptr) {
    LOGE("GraphicBuffer::from returned null (surfaceId=%d)", surface_id);
    return false;
  }

  const void *dec_strong_cookie = &graphic_buffer_ref;
  bool replaced = false;

  android_ycbcr layout{};
  const int lock_rc = g_graphic_buffer_api.lock_ycbcr(graphic_buffer,
                                                      kGraphicBufferCpuLockUsage,
                                                      &layout);
  if (lock_rc != 0) {
    LOGE("GraphicBuffer::lockYCbCr failed rc=%d for %ux%u surfaceId=%d", lock_rc,
         buffer.stream->width, buffer.stream->height, surface_id);
  } else {
    replaced = write_i420_to_ycbcr(*scaled_i420,
                                   static_cast<int>(buffer.stream->width),
                                   static_cast<int>(buffer.stream->height), layout);
    const int unlock_rc = g_graphic_buffer_api.unlock(graphic_buffer);
    if (unlock_rc != 0) {
      LOGW("GraphicBuffer::unlock failed rc=%d", unlock_rc);
    }
  }

  g_graphic_buffer_api.dec_strong(graphic_buffer, dec_strong_cookie);

  if (replaced) {
    const uint64_t replace_count =
        g_replaced_frame_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (replace_count <= 10 || (replace_count % 120) == 0) {
      LOGI("Replaced frame #%llu streamId=%d dst=%ux%u fmt=%#x source=%s %dx%d %s surfaceId=%d",
           static_cast<unsigned long long>(replace_count),
           matched_stream != nullptr ? matched_stream->stream_id : -1,
           buffer.stream->width, buffer.stream->height, buffer.stream->format,
           basename_copy(source_descriptor.sequence ? g_source_state.loaded_path
                                                    : source_descriptor.path)
               .c_str(),
           source_descriptor.width, source_descriptor.height,
           source_pixel_format_name(source_descriptor.format), surface_id);
    }
  }

  return replaced;
}

using Camera3DeviceCreateStreamFn = int (*)(
    void *thiz, const void *consumers, bool hasDeferredConsumer, uint32_t width,
    uint32_t height, int format, int32_t dataSpace, int rotation, int *id,
    const void *physicalCameraId, const void *sensorPixelModesUsed, void *surfaceIds,
    int streamSetId, bool isShared, bool isMultiResolution, uint64_t consumerUsage,
    int64_t dynamicRangeProfile, int64_t streamUseCase, int timestampBase,
    int32_t colorSpace, bool useReadoutTimestamp);

using ReturnBufferCheckedLockedFn = int (*) (
    void *thiz, const android::camera3::camera_stream_buffer &buffer, long timestamp,
    long readout_timestamp, bool output, int32_t surface_id,
    const std::vector<size_t> &transform, android::sp<android::Fence> *release_fence);

int hook_camera3device_create_stream(
    void *thiz, const void *consumers, bool hasDeferredConsumer, uint32_t width,
    uint32_t height, int format, int32_t dataSpace, int rotation, int *id,
    const void *physicalCameraId, const void *sensorPixelModesUsed, void *surfaceIds,
    int streamSetId, bool isShared, bool isMultiResolution, uint64_t consumerUsage,
    int64_t dynamicRangeProfile, int64_t streamUseCase, int timestampBase,
    int32_t colorSpace, bool useReadoutTimestamp) {
  SHADOWHOOK_STACK_SCOPE();

  auto orig = reinterpret_cast<Camera3DeviceCreateStreamFn>(g_orig_camera3device_create_stream);
  if (orig == nullptr) {
    LOGE("Camera3Device::createStream orig is NULL");
    return -1;
  }

  const int res = orig(thiz, consumers, hasDeferredConsumer, width, height, format,
                       dataSpace, rotation, id, physicalCameraId, sensorPixelModesUsed,
                       surfaceIds, streamSetId, isShared, isMultiResolution,
                       consumerUsage, dynamicRangeProfile, streamUseCase, timestampBase,
                       colorSpace, useReadoutTimestamp);

  const uint64_t count =
      g_create_stream_hit_count.fetch_add(1, std::memory_order_relaxed) + 1;
  const int streamId = (id != nullptr) ? *id : -1;
  if (res == 0) {
    remember_stream_record(streamId, width, height, format, dataSpace, consumerUsage,
                           streamSetId, hasDeferredConsumer, isShared,
                           isMultiResolution, timestampBase, colorSpace,
                           useReadoutTimestamp);
  }

  LOGI("Camera3Device::createStream hit #%llu res=%d streamId=%d width=%u height=%u format=%#x dataspace=%#x deferred=%d shared=%d multiRes=%d usage=%" PRIu64 " streamSetId=%d tsBase=%d colorSpace=%d readoutTs=%d",
       static_cast<unsigned long long>(count), res, streamId, width, height, format,
       dataSpace, hasDeferredConsumer, isShared, isMultiResolution, consumerUsage,
       streamSetId, timestampBase, colorSpace, useReadoutTimestamp);

  return res;
}

int hook_return_buffer_checked_locked(
    void *thiz, const android::camera3::camera_stream_buffer &buffer, long timestamp,
    long readout_timestamp, bool output, int32_t surface_id,
    const std::vector<size_t> &transform, android::sp<android::Fence> *release_fence) {
  SHADOWHOOK_STACK_SCOPE();

  auto orig = reinterpret_cast<ReturnBufferCheckedLockedFn>(g_orig_return_buffer_checked_locked);
  if (orig == nullptr) {
    LOGE("returnBufferCheckedLocked orig is NULL");
    return -1;
  }

  const uint64_t count =
      g_return_buffer_hit_count.fetch_add(1, std::memory_order_relaxed) + 1;

  StreamRecord matched{};
  bool found_match = false;
  uint64_t per_stream_log_count = 0;

  const auto *stream = buffer.stream;
  if (stream != nullptr) {
    found_match = find_stream_record_by_props(stream->width, stream->height,
                                              stream->format, stream->data_space,
                                              &matched, &per_stream_log_count);
    if (found_match) {
      if (per_stream_log_count <= 5) {
        LOGI("returnBufferCheckedLocked hit #%llu streamId=%d width=%u height=%u format=%#x dataspace=%#x usage=%#" PRIx64 " output=%d surfaceId=%d ts=%ld readoutTs=%ld",
             static_cast<unsigned long long>(count), matched.stream_id, stream->width,
             stream->height, stream->format, stream->data_space, stream->usage,
             output, surface_id, timestamp, readout_timestamp);
      }
    } else if (count <= 12) {
      LOGI("returnBufferCheckedLocked hit #%llu unmatched width=%u height=%u format=%#x dataspace=%#x usage=%#" PRIx64 " output=%d surfaceId=%d ts=%ld readoutTs=%ld",
           static_cast<unsigned long long>(count), stream->width, stream->height,
           stream->format, stream->data_space, stream->usage, output, surface_id,
           timestamp, readout_timestamp);
    }
  } else if (count <= 12) {
    LOGI("returnBufferCheckedLocked hit #%llu stream=null output=%d surfaceId=%d ts=%ld readoutTs=%ld",
         static_cast<unsigned long long>(count), output, surface_id, timestamp,
         readout_timestamp);
  }

  if (output && stream != nullptr) {
    std::lock_guard<std::mutex> process_lock(g_frame_process_mutex);
    (void)try_replace_camera3_frame(buffer, surface_id, found_match ? &matched : nullptr);
  }

  return orig(thiz, buffer, timestamp, readout_timestamp, output, surface_id,
              transform, release_fence);
}

void install_hook() {
  LOGI("install_hook: begin");

  const int init_rc = shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false);
  if (init_rc != 0) {
    const int err = shadowhook_get_errno();
    LOGE("shadowhook_init failed rc=%d errno=%d msg=%s", init_rc, err,
         shadowhook_to_errmsg(err));
    return;
  }
  LOGI("install_hook: shadowhook initialized");

  if (!resolve_graphic_buffer_api()) {
    LOGE("install_hook: GraphicBuffer API unresolved");
    return;
  }

  const uintptr_t base = find_cameraserver_base();
  if (base == 0) {
    LOGE("failed to locate cameraserver base");
    return;
  }
  LOGI("install_hook: cameraserver base=%p", reinterpret_cast<void *>(base));

  {
    const uintptr_t target = base + kCreateConfiguredSurfaceOffset;
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(target), &info) != 0) {
      LOGI("createConfiguredSurface target=%p module=%s sym=%s",
           reinterpret_cast<void *>(target),
           info.dli_fname != nullptr ? info.dli_fname : "(null)",
           info.dli_sname != nullptr ? info.dli_sname : "(null)");
    }

    g_create_surface_stub =
        shadowhook_hook_func_addr(reinterpret_cast<void *>(target),
                                  reinterpret_cast<void *>(hook_create_configured_surface),
                                  &g_orig_create_configured_surface);
    if (g_create_surface_stub == nullptr) {
      const int err = shadowhook_get_errno();
      LOGE("hook createConfiguredSurface failed errno=%d msg=%s", err,
           shadowhook_to_errmsg(err));
    } else {
      LOGI("installed hook for %s at %p (base=%p offset=0x%lx) orig=%p",
           kCreateConfiguredSurfaceSymbol, reinterpret_cast<void *>(target),
           reinterpret_cast<void *>(base),
           static_cast<unsigned long>(kCreateConfiguredSurfaceOffset),
           g_orig_create_configured_surface);
    }
  }

  {
    const uintptr_t target = base + kCamera3DeviceCreateStreamOffset;
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(target), &info) != 0) {
      LOGI("Camera3Device::createStream target=%p module=%s sym=%s",
           reinterpret_cast<void *>(target),
           info.dli_fname != nullptr ? info.dli_fname : "(null)",
           info.dli_sname != nullptr ? info.dli_sname : "(null)");
    }

    g_camera3device_create_stream_stub =
        shadowhook_hook_func_addr(reinterpret_cast<void *>(target),
                                  reinterpret_cast<void *>(hook_camera3device_create_stream),
                                  &g_orig_camera3device_create_stream);
    if (g_camera3device_create_stream_stub == nullptr) {
      const int err = shadowhook_get_errno();
      LOGE("hook Camera3Device::createStream failed errno=%d msg=%s", err,
           shadowhook_to_errmsg(err));
    } else {
      LOGI("installed hook for %s at %p (base=%p offset=0x%lx) orig=%p",
           kCamera3DeviceCreateStreamSymbol, reinterpret_cast<void *>(target),
           reinterpret_cast<void *>(base),
           static_cast<unsigned long>(kCamera3DeviceCreateStreamOffset),
           g_orig_camera3device_create_stream);
    }
  }

  {
    const uintptr_t target = base + kReturnBufferCheckedLockedOffset;
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(target), &info) != 0) {
      LOGI("returnBufferCheckedLocked target=%p module=%s sym=%s",
           reinterpret_cast<void *>(target),
           info.dli_fname != nullptr ? info.dli_fname : "(null)",
           info.dli_sname != nullptr ? info.dli_sname : "(null)");
    }

    g_return_buffer_checked_locked_stub =
        shadowhook_hook_func_addr(reinterpret_cast<void *>(target),
                                  reinterpret_cast<void *>(hook_return_buffer_checked_locked),
                                  &g_orig_return_buffer_checked_locked);
    if (g_return_buffer_checked_locked_stub == nullptr) {
      const int err = shadowhook_get_errno();
      LOGE("hook returnBufferCheckedLocked failed errno=%d msg=%s", err,
           shadowhook_to_errmsg(err));
    } else {
      LOGI("installed hook for %s at %p (base=%p offset=0x%lx) orig=%p",
           kReturnBufferCheckedLockedSymbol, reinterpret_cast<void *>(target),
           reinterpret_cast<void *>(base),
           static_cast<unsigned long>(kReturnBufferCheckedLockedOffset),
           g_orig_return_buffer_checked_locked);
    }
  }
}

}  // namespace

extern "C" void log_create_configured_surface_result(void *stream_info_ptr,
                                                      uint64_t is_stream_info_valid) {
  const uint64_t count =
      g_surface_hit_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (stream_info_ptr == nullptr) {
    LOGE("SessionConfigurationUtils::createSurfaceFromGbp hit #%llu valid=%llu streamInfo=null",
         static_cast<unsigned long long>(count),
         static_cast<unsigned long long>(is_stream_info_valid));
    return;
  }

  const auto *info = reinterpret_cast<const OutputStreamInfoPrefix *>(stream_info_ptr);
  LOGI("SessionConfigurationUtils::createSurfaceFromGbp hit #%llu valid=%llu width=%d height=%d format=%#x dataspace=%#x",
       static_cast<unsigned long long>(count),
       static_cast<unsigned long long>(is_stream_info_valid), info->width,
       info->height, info->format, info->data_space);
}

extern "C" __attribute__((naked, noinline)) void hook_create_configured_surface() {
#if defined(__aarch64__)
  __asm__ __volatile__(
      "sub sp, sp, #0x90\n"
      "stp x29, x30, [sp, #0x80]\n"
      "stp x19, x20, [sp, #0x30]\n"
      "stp x21, x22, [sp, #0x40]\n"
      "stp x23, x24, [sp, #0x50]\n"
      "stp x25, x26, [sp, #0x60]\n"
      "stp x27, x28, [sp, #0x70]\n"

      "mov x19, x0\n"
      "mov x20, x1\n"
      "mov x21, x2\n"
      "mov x22, x3\n"
      "mov x23, x4\n"
      "mov x24, x5\n"
      "mov x25, x6\n"
      "mov x26, x7\n"
      "mov x27, x8\n"

      "ldr x9, [sp, #0x90]\n"
      "str x9, [sp, #0x00]\n"
      "ldr x9, [sp, #0x98]\n"
      "str x9, [sp, #0x08]\n"
      "ldr x9, [sp, #0xa0]\n"
      "str x9, [sp, #0x10]\n"
      "ldr x9, [sp, #0xa8]\n"
      "str x9, [sp, #0x18]\n"
      "ldr x9, [sp, #0xb0]\n"
      "str x9, [sp, #0x20]\n"

      "adrp x16, :got:g_orig_create_configured_surface\n"
      "ldr x16, [x16, :got_lo12:g_orig_create_configured_surface]\n"
      "ldr x16, [x16]\n"
      "mov x0, x19\n"
      "mov x1, x20\n"
      "mov x2, x21\n"
      "mov x3, x22\n"
      "mov x4, x23\n"
      "mov x5, x24\n"
      "mov x6, x25\n"
      "mov x7, x26\n"
      "mov x8, x27\n"
      "blr x16\n"

      "str x0, [sp, #0x28]\n"
      "mov x0, x19\n"
      "mov x1, x20\n"
      "bl log_create_configured_surface_result\n"
      "ldr x0, [sp, #0x28]\n"

      "ldp x19, x20, [sp, #0x30]\n"
      "ldp x21, x22, [sp, #0x40]\n"
      "ldp x23, x24, [sp, #0x50]\n"
      "ldp x25, x26, [sp, #0x60]\n"
      "ldp x27, x28, [sp, #0x70]\n"
      "ldp x29, x30, [sp, #0x80]\n"
      "add sp, sp, #0x90\n"
      "ret\n");
#else
#error "hook_create_configured_surface is only implemented for aarch64"
#endif
}

namespace {
void EnsureSelfPinned(const void *symbol_for_dladdr, const char *entry_name) {
  if (g_self_handle != nullptr) return;
  Dl_info self_info{};
  if (dladdr(symbol_for_dladdr, &self_info) != 0 && self_info.dli_fname != nullptr) {
    int flags = RTLD_NOW | RTLD_GLOBAL;
#ifdef RTLD_NODELETE
    flags |= RTLD_NODELETE;
#endif
    g_self_handle = dlopen(self_info.dli_fname, flags);
    if (g_self_handle != nullptr) {
      LOGI("%s: self-pinned %s handle=%p", entry_name, self_info.dli_fname, g_self_handle);
    } else {
      LOGE("%s: self-pin failed for %s: %s", entry_name, self_info.dli_fname,
           dlerror() != nullptr ? dlerror() : "unknown");
    }
  } else {
    LOGE("%s: dladdr failed for self", entry_name);
  }
}
}

extern "C" __attribute__((visibility("default"))) void main_hook_c61() {
  EnsureSelfPinned(reinterpret_cast<void *>(&main_hook_c61), "main_hook_c61");
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    LOGI("main_hook[C61]: already ran");
    return;
  }
  LOGI("main_hook[C61]: classic threadpool worker only");
  const bool ok = awesomecam::ProbeClassicThreadPoolWorkerOnly();
  LOGI("main_hook[C61]: ProbeClassicThreadPoolWorkerOnly => %s", ok ? "ok" : "fail");
}

extern "C" __attribute__((visibility("default"))) void main_hook_c62() {
  EnsureSelfPinned(reinterpret_cast<void *>(&main_hook_c62), "main_hook_c62");
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    LOGI("main_hook[C62]: already ran");
    return;
  }
  LOGI("main_hook[C62]: classic service worker async");
  const bool ok = awesomecam::ProbeClassicServiceWorkerAsync();
  LOGI("main_hook[C62]: ProbeClassicServiceWorkerAsync => %s", ok ? "ok" : "fail");
}

extern "C" __attribute__((visibility("default"))) void main_hook() {
  EnsureSelfPinned(reinterpret_cast<void *>(&main_hook), "main_hook");
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    LOGI("main_hook: already ran");
    return;
  }
  LOGI("main_hook: install hooks + classic service worker async");
  install_hook();
  const bool ok = awesomecam::ProbeClassicServiceWorkerAsync();
  LOGI("main_hook: ProbeClassicServiceWorkerAsync => %s", ok ? "ok" : "fail");
}

extern "C" __attribute__((visibility("default"))) void main_hook_c51() {
  EnsureSelfPinned(reinterpret_cast<void *>(&main_hook_c51), "main_hook_c51");
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    LOGI("main_hook[C51]: already ran");
    return;
  }
  LOGI("main_hook[C51]: threadpool worker only");
  const bool ok = awesomecam::ProbeBinderThreadPoolWorkerOnly();
  LOGI("main_hook[C51]: ProbeBinderThreadPoolWorkerOnly => %s", ok ? "ok" : "fail");
}

extern "C" __attribute__((visibility("default"))) void main_hook_c52() {
  EnsureSelfPinned(reinterpret_cast<void *>(&main_hook_c52), "main_hook_c52");
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    LOGI("main_hook[C52]: already ran");
    return;
  }
  LOGI("main_hook[C52]: full service worker async");
  const bool ok = awesomecam::ProbeBinderFullServiceWorkerAsync();
  LOGI("main_hook[C52]: ProbeBinderFullServiceWorkerAsync => %s", ok ? "ok" : "fail");
}
