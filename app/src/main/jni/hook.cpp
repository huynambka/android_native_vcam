#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <vector>

#include <atomic>

#include "shadowhook.h"

#define LOG_TAG "awesomeCAM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

namespace android {
template <typename T>
class sp;
class Fence;
namespace camera3 {
struct camera_stream_buffer;
}  // namespace camera3
}  // namespace android

constexpr const char *kTargetModuleBasename = "cameraserver";
constexpr uintptr_t kTargetOffset = 0x22cd70;
constexpr const char *kTargetSymbol =
    "_ZN7android7camera319Camera3OutputStream25returnBufferCheckedLockedERKNS0_20camera_stream_bufferEllbiRKNSt3__16vectorImNS5_9allocatorImEEEEPNS_2spINS_5FenceEEE";

std::atomic<bool> g_started{false};
std::atomic<uint64_t> g_hit_count{0};
void *g_return_buffer_stub = nullptr;
using ReturnBufferFn = int (*)(void *thiz, const android::camera3::camera_stream_buffer &buffer,
                               long timestamp, long readout_timestamp, bool output, int32_t surface_id,
                               const std::vector<size_t> &transform, android::sp<android::Fence> *release_fence);
ReturnBufferFn g_orig_return_buffer = nullptr;

bool ends_with(const char *value, const char *suffix) {
  if (value == nullptr || suffix == nullptr) return false;
  const size_t value_len = strlen(value);
  const size_t suffix_len = strlen(suffix);
  if (suffix_len > value_len) return false;
  return memcmp(value + value_len - suffix_len, suffix, suffix_len) == 0;
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
      const ssize_t len = readlink("/proc/self/exe", search->exe_path, sizeof(search->exe_path) - 1);
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

int hook_return_buffer(void *thiz, const android::camera3::camera_stream_buffer &buffer, long timestamp,
                       long readout_timestamp, bool output, int32_t surface_id,
                       const std::vector<size_t> &transform, android::sp<android::Fence> *release_fence) {
  SHADOWHOOK_STACK_SCOPE();
  const uint64_t count = g_hit_count.fetch_add(1, std::memory_order_relaxed) + 1;
  LOGI("Camera3OutputStream::returnBufferCheckedLocked hit #%llu this=%p", static_cast<unsigned long long>(count), thiz);

  if (g_orig_return_buffer == nullptr) {
    LOGE("g_orig_return_buffer is NULL");
    return -1;
  }

  return g_orig_return_buffer(thiz, buffer, timestamp, readout_timestamp, output, surface_id, transform,
                              release_fence);
}

void install_hook() {
  LOGI("install_hook: begin");

  const int init_rc = shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false);
  if (init_rc != 0) {
    const int err = shadowhook_get_errno();
    LOGE("shadowhook_init failed rc=%d errno=%d msg=%s", init_rc, err, shadowhook_to_errmsg(err));
    return;
  }
  LOGI("install_hook: shadowhook initialized");

  const uintptr_t base = find_cameraserver_base();
  if (base == 0) {
    LOGE("failed to locate cameraserver base");
    return;
  }
  LOGI("install_hook: cameraserver base=%p", reinterpret_cast<void *>(base));

  const uintptr_t target = base + kTargetOffset;
  Dl_info info {};
  if (dladdr(reinterpret_cast<void *>(target), &info) != 0) {
    LOGI("target addr=%p module=%s sym=%s", reinterpret_cast<void *>(target),
         info.dli_fname != nullptr ? info.dli_fname : "(null)", info.dli_sname != nullptr ? info.dli_sname : "(null)");
  } else {
    LOGI("target addr=%p module=(unknown)", reinterpret_cast<void *>(target));
  }

  g_return_buffer_stub = shadowhook_hook_func_addr(reinterpret_cast<void *>(target),
                                                   reinterpret_cast<void *>(hook_return_buffer),
                                                   reinterpret_cast<void **>(&g_orig_return_buffer));
  if (g_return_buffer_stub == nullptr) {
    const int err = shadowhook_get_errno();
    LOGE("shadowhook_hook_func_addr failed errno=%d msg=%s", err, shadowhook_to_errmsg(err));
    return;
  }

  LOGI("installed hook for %s at %p (base=%p offset=0x%lx) orig=%p", kTargetSymbol, reinterpret_cast<void *>(target),
       reinterpret_cast<void *>(base), static_cast<unsigned long>(kTargetOffset),
       reinterpret_cast<void *>(g_orig_return_buffer));
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void main_hook() {
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    LOGI("main_hook: already installed");
    return;
  }

  LOGI("main_hook: installing hook");
  install_hook();
}
