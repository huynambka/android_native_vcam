#include "libyuv_runtime.h"

#include <android/log.h>
#include <dlfcn.h>
#include <mutex>

#define LOG_TAG "awesomeCAM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace awesomecam {
namespace {

using Android420ToI420Fn = int (*)(const uint8_t *src_y, int src_stride_y,
                                   const uint8_t *src_u, int src_stride_u,
                                   const uint8_t *src_v, int src_stride_v,
                                   int src_pixel_stride_uv,
                                   uint8_t *dst_y, int dst_stride_y,
                                   uint8_t *dst_u, int dst_stride_u,
                                   uint8_t *dst_v, int dst_stride_v,
                                   int width, int height);
using I420ScaleFn = int (*)(const uint8_t *src_y, int src_stride_y,
                            const uint8_t *src_u, int src_stride_u,
                            const uint8_t *src_v, int src_stride_v,
                            int src_width, int src_height,
                            uint8_t *dst_y, int dst_stride_y,
                            uint8_t *dst_u, int dst_stride_u,
                            uint8_t *dst_v, int dst_stride_v,
                            int dst_width, int dst_height,
                            int filtering);

struct LibYuvRuntime {
  void *handle = nullptr;
  Android420ToI420Fn android420_to_i420 = nullptr;
  I420ScaleFn i420_scale = nullptr;
  bool ok = false;
};

std::once_flag g_init_once;
LibYuvRuntime g_runtime;

void InitLibYuvRuntime() {
  constexpr const char *kCandidates[] = {
      "libyuv.so",
      "/system/lib64/libyuv.so",
      "/apex/com.google.pixel.camera.hal/lib64/libyuv.so",
      "/apex/com.android.media.swcodec/lib64/libyuv.so",
  };

  for (const char *path : kCandidates) {
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) continue;

    auto android420_to_i420 = reinterpret_cast<Android420ToI420Fn>(
        dlsym(handle, "Android420ToI420"));
    auto i420_scale = reinterpret_cast<I420ScaleFn>(dlsym(handle, "I420Scale"));
    if (android420_to_i420 != nullptr && i420_scale != nullptr) {
      g_runtime.handle = handle;
      g_runtime.android420_to_i420 = android420_to_i420;
      g_runtime.i420_scale = i420_scale;
      g_runtime.ok = true;
      LOGI("libyuv runtime loaded from %s", path);
      return;
    }

    dlclose(handle);
  }

  LOGW("libyuv runtime unavailable; using manual YUV copy/scale fallback");
}

const LibYuvRuntime &Runtime() {
  std::call_once(g_init_once, InitLibYuvRuntime);
  return g_runtime;
}

}  // namespace

bool LibYuvAvailable() { return Runtime().ok; }

bool LibYuvAndroid420ToI420(const uint8_t *src_y, int src_stride_y,
                            const uint8_t *src_u, int src_stride_u,
                            const uint8_t *src_v, int src_stride_v,
                            int src_pixel_stride_uv,
                            uint8_t *dst_y, int dst_stride_y,
                            uint8_t *dst_u, int dst_stride_u,
                            uint8_t *dst_v, int dst_stride_v,
                            int width, int height) {
  const auto &rt = Runtime();
  if (!rt.ok || rt.android420_to_i420 == nullptr) return false;
  return rt.android420_to_i420(src_y, src_stride_y, src_u, src_stride_u,
                               src_v, src_stride_v, src_pixel_stride_uv,
                               dst_y, dst_stride_y, dst_u, dst_stride_u,
                               dst_v, dst_stride_v, width, height) == 0;
}

bool LibYuvI420Scale(const uint8_t *src_y, int src_stride_y,
                     const uint8_t *src_u, int src_stride_u,
                     const uint8_t *src_v, int src_stride_v,
                     int src_width, int src_height,
                     uint8_t *dst_y, int dst_stride_y,
                     uint8_t *dst_u, int dst_stride_u,
                     uint8_t *dst_v, int dst_stride_v,
                     int dst_width, int dst_height,
                     int filtering) {
  const auto &rt = Runtime();
  if (!rt.ok || rt.i420_scale == nullptr) return false;
  return rt.i420_scale(src_y, src_stride_y, src_u, src_stride_u, src_v,
                       src_stride_v, src_width, src_height, dst_y,
                       dst_stride_y, dst_u, dst_stride_u, dst_v,
                       dst_stride_v, dst_width, dst_height, filtering) == 0;
}

}  // namespace awesomecam
