#include "video2camera_service.h"

#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/log.h>
#include <pthread.h>

#include <atomic>
#include <mutex>
#include <vector>

#include "video2camera_ipc.h"
#include "video2camera_ndk.h"

#define LOG_TAG "awesomeCAM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace awesomecam {
namespace {

struct IncomingByteArray {
  std::vector<int8_t> data;
};

struct SharedFrameState {
  std::mutex mutex;
  int32_t width = 0;
  int32_t height = 0;
  int32_t format = kFrameFormatUnknown;
  std::vector<uint8_t> bytes;
  uint64_t generation = 0;
  uint64_t push_count = 0;
};

SharedFrameState g_shared_frame;
BinderRuntimeApi g_binder_runtime;
std::atomic<bool> g_service_started{false};
std::atomic<bool> g_service_registered{false};
std::atomic<bool> g_service_launching{false};
AIBinder_Class *g_service_class = nullptr;
AIBinder *g_service_binder = nullptr;

bool ByteArrayAllocator(void *arrayData, int32_t length, int8_t **outBuffer) {
  if (arrayData == nullptr || outBuffer == nullptr) return false;
  auto *incoming = static_cast<IncomingByteArray *>(arrayData);
  if (length < 0) {
    incoming->data.clear();
    *outBuffer = nullptr;
    return true;
  }
  incoming->data.resize(static_cast<size_t>(length));
  *outBuffer = incoming->data.empty() ? nullptr : incoming->data.data();
  return true;
}

void *OnCreate(void *) { return nullptr; }
void OnDestroy(void *) {}

binder_status_t OnTransact(AIBinder *, transaction_code_t code, const AParcel *in,
                           AParcel *out) {
  switch (code) {
    case kTxnSetFrame: {
      int32_t width = 0;
      int32_t height = 0;
      int32_t format = 0;
      binder_status_t status = g_binder_runtime.parcel_read_int32(in, &width);
      if (status != STATUS_OK) return status;
      status = g_binder_runtime.parcel_read_int32(in, &height);
      if (status != STATUS_OK) return status;
      status = g_binder_runtime.parcel_read_int32(in, &format);
      if (status != STATUS_OK) return status;

      IncomingByteArray incoming;
      status = g_binder_runtime.parcel_read_byte_array(in, &incoming, ByteArrayAllocator);
      if (status != STATUS_OK) return status;

      if (width <= 0 || height <= 0 || format != kFrameFormatI420) {
        LOGW("Video2CameraService reject frame width=%d height=%d format=%d size=%zu",
             width, height, format, incoming.data.size());
        return STATUS_BAD_VALUE;
      }

      const size_t expected = static_cast<size_t>(width) * height * 3 / 2;
      if (incoming.data.size() < expected) {
        LOGW("Video2CameraService frame too small size=%zu expected=%zu", incoming.data.size(),
             expected);
        return STATUS_BAD_VALUE;
      }

      {
        std::lock_guard<std::mutex> lock(g_shared_frame.mutex);
        g_shared_frame.width = width;
        g_shared_frame.height = height;
        g_shared_frame.format = format;
        g_shared_frame.bytes.assign(reinterpret_cast<const uint8_t *>(incoming.data.data()),
                                    reinterpret_cast<const uint8_t *>(incoming.data.data()) +
                                        expected);
        g_shared_frame.generation += 1;
        g_shared_frame.push_count += 1;
        if (g_shared_frame.push_count <= 5 || (g_shared_frame.push_count % 120) == 0) {
          LOGI("Video2CameraService got frame #%llu %dx%d fmt=%d bytes=%zu gen=%llu",
               static_cast<unsigned long long>(g_shared_frame.push_count), width, height,
               format, g_shared_frame.bytes.size(),
               static_cast<unsigned long long>(g_shared_frame.generation));
        }
        if (out != nullptr) {
          g_binder_runtime.parcel_write_int32(out, static_cast<int32_t>(g_shared_frame.generation));
        }
      }
      return STATUS_OK;
    }
    case kTxnClearFrame: {
      {
        std::lock_guard<std::mutex> lock(g_shared_frame.mutex);
        g_shared_frame.width = 0;
        g_shared_frame.height = 0;
        g_shared_frame.format = kFrameFormatUnknown;
        g_shared_frame.bytes.clear();
        g_shared_frame.generation += 1;
        if (out != nullptr) {
          g_binder_runtime.parcel_write_int32(out, static_cast<int32_t>(g_shared_frame.generation));
        }
      }
      LOGI("Video2CameraService cleared cached frame");
      return STATUS_OK;
    }
    default:
      return STATUS_UNKNOWN_TRANSACTION;
  }
}

bool EnsureServiceClass() {
  if (g_service_class != nullptr) return true;
  g_service_class = g_binder_runtime.binder_class_define(
      kVideo2CameraDescriptor, OnCreate, OnDestroy, OnTransact);
  return g_service_class != nullptr;
}

}  // namespace

bool EnsureVideo2CameraServiceStarted() {
  bool expected = false;
  if (!g_service_started.compare_exchange_strong(expected, true)) {
    return g_service_registered.load();
  }

  auto fail = [&](const char *msg) {
    LOGE("%s", msg);
    g_service_started.store(false);
    return false;
  };

  LOGI("Video2CameraService: loading binder runtime");
  if (!LoadBinderRuntimeApi(&g_binder_runtime)) {
    return fail("Video2CameraService: failed to load binder runtime API");
  }
  LOGI("Video2CameraService: defining binder class");
  if (!EnsureServiceClass()) {
    return fail("Video2CameraService: failed to define service class");
  }

  LOGI("Video2CameraService: creating binder instance");
  g_service_binder = g_binder_runtime.binder_new(g_service_class, nullptr);
  if (g_service_binder == nullptr) {
    return fail("Video2CameraService: AIBinder_new failed");
  }

  LOGI("Video2CameraService: starting binder threadpool");
  g_binder_runtime.set_thread_pool_max(1);
  g_binder_runtime.start_thread_pool();

  LOGI("Video2CameraService: adding service %s", kVideo2CameraServiceName);
  const binder_status_t add_status =
      g_binder_runtime.add_service(g_service_binder, kVideo2CameraServiceName);
  if (add_status != STATUS_OK) {
    char buf[128];
    snprintf(buf, sizeof(buf), "Video2CameraService: add_service failed status=%d", add_status);
    return fail(buf);
  }

  g_service_registered.store(true);
  LOGI("Video2CameraService registered as %s", kVideo2CameraServiceName);
  return true;
}

namespace {
void *ServiceThreadMain(void *) {
  EnsureVideo2CameraServiceStarted();
  g_service_launching.store(false);
  return nullptr;
}
}  // namespace

void StartVideo2CameraServiceAsync() {
  if (g_service_registered.load()) return;

  bool expected = false;
  if (!g_service_launching.compare_exchange_strong(expected, true)) {
    return;
  }

  pthread_t thread;
  const int rc = pthread_create(&thread, nullptr, ServiceThreadMain, nullptr);
  if (rc != 0) {
    LOGE("Video2CameraService: pthread_create failed rc=%d", rc);
    g_service_launching.store(false);
    return;
  }
  pthread_detach(thread);
  LOGI("Video2CameraService: async startup requested");
}

bool CopyLatestBinderFrame(BinderFrameCopy *out) {
  if (out == nullptr) return false;
  std::lock_guard<std::mutex> lock(g_shared_frame.mutex);
  if (g_shared_frame.width <= 0 || g_shared_frame.height <= 0 ||
      g_shared_frame.format != kFrameFormatI420 || g_shared_frame.bytes.empty()) {
    return false;
  }
  out->width = g_shared_frame.width;
  out->height = g_shared_frame.height;
  out->format = g_shared_frame.format;
  out->generation = g_shared_frame.generation;
  out->bytes = g_shared_frame.bytes;
  return true;
}

void ClearLatestBinderFrame() {
  std::lock_guard<std::mutex> lock(g_shared_frame.mutex);
  g_shared_frame.width = 0;
  g_shared_frame.height = 0;
  g_shared_frame.format = kFrameFormatUnknown;
  g_shared_frame.bytes.clear();
  g_shared_frame.generation += 1;
}

}  // namespace awesomecam
