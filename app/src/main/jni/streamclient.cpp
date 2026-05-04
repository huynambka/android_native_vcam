#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/log.h>
#include <jni.h>

#include <mutex>
#include <cstring>
#include <thread>
#include <vector>
#include <chrono>

#include "video2camera_ipc.h"
#include "video2camera_ndk.h"

#define LOG_TAG "awesomeCAM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace awesomecam {
namespace {

BinderRuntimeApi g_binder_runtime;
std::mutex g_client_mutex;
AIBinder_Class *g_client_class = nullptr;
AIBinder *g_remote = nullptr;

void *ClientOnCreate(void *) { return nullptr; }
void ClientOnDestroy(void *) {}
binder_status_t ClientOnTransact(AIBinder *, transaction_code_t, const AParcel *, AParcel *) {
  return STATUS_UNKNOWN_TRANSACTION;
}

bool EnsureClientClass() {
  if (g_client_class != nullptr) return true;
  g_client_class = g_binder_runtime.binder_class_define(
      kVideo2CameraDescriptor, ClientOnCreate, ClientOnDestroy, ClientOnTransact);
  return g_client_class != nullptr;
}

bool EnsureConnectedLocked() {
  if (g_remote != nullptr && g_binder_runtime.binder_is_alive(g_remote)) return true;
  if (!LoadBinderRuntimeApi(&g_binder_runtime)) {
    LOGE("streamclient: failed to load binder runtime API");
    return false;
  }
  if (!EnsureClientClass()) {
    LOGE("streamclient: failed to define client class");
    return false;
  }

  LOGI("streamclient: probing service %s", kVideo2CameraServiceName);
  AIBinder *binder = nullptr;
  for (int attempt = 0; attempt < 20; ++attempt) {
    binder = g_binder_runtime.check_service(kVideo2CameraServiceName);
    if (binder != nullptr) break;
    if (attempt == 0 || attempt == 4 || attempt == 9 || attempt == 19) {
      LOGW("streamclient: waiting for service %s attempt=%d/20", kVideo2CameraServiceName,
           attempt + 1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  if (binder == nullptr) {
    LOGE("streamclient: service %s not available after timeout", kVideo2CameraServiceName);
    return false;
  }

  g_binder_runtime.binder_associate_class(binder, g_client_class);
  g_remote = binder;
  LOGI("streamclient: connected to %s", kVideo2CameraServiceName);
  return true;
}

bool TransactSetFrameLocked(int width, int height, int format, const jbyte *data,
                            jsize length, int32_t *outGeneration) {
  if (!EnsureConnectedLocked()) return false;

  if (length > 900000) {
    LOGW("streamclient: large binder frame %dx%d fmt=%d bytes=%d", width, height, format,
         static_cast<int>(length));
  }

  AParcel *in = nullptr;
  binder_status_t status = g_binder_runtime.binder_prepare_transaction(g_remote, &in);
  if (status != STATUS_OK || in == nullptr) {
    LOGE("streamclient: prepareTransaction failed status=%d", status);
    return false;
  }

  status = g_binder_runtime.parcel_write_int32(in, width);
  if (status == STATUS_OK) status = g_binder_runtime.parcel_write_int32(in, height);
  if (status == STATUS_OK) status = g_binder_runtime.parcel_write_int32(in, format);
  if (status == STATUS_OK) {
    status = g_binder_runtime.parcel_write_byte_array(in, reinterpret_cast<const int8_t *>(data), length);
  }
  if (status != STATUS_OK) {
    LOGE("streamclient: write frame parcel failed status=%d", status);
    g_binder_runtime.parcel_delete(in);
    return false;
  }

  AParcel *out = nullptr;
  status = g_binder_runtime.binder_transact(g_remote, kTxnSetFrame, &in, &out, 0);
  if (status != STATUS_OK) {
    LOGE("streamclient: transact setFrame failed status=%d width=%d height=%d fmt=%d bytes=%d",
         status, width, height, format, static_cast<int>(length));
    if (out != nullptr) g_binder_runtime.parcel_delete(out);
    return false;
  }

  int32_t generation = 0;
  if (out != nullptr) {
    status = g_binder_runtime.parcel_read_int32(out, &generation);
    g_binder_runtime.parcel_delete(out);
    if (status != STATUS_OK) {
      LOGE("streamclient: read generation failed status=%d", status);
      return false;
    }
  }

  if (outGeneration != nullptr) *outGeneration = generation;
  return true;
}

bool TransactPlayFileLocked(const char *path, size_t path_length) {
  if (path == nullptr || path_length == 0) return false;
  if (!EnsureConnectedLocked()) return false;

  AParcel *in = nullptr;
  binder_status_t status = g_binder_runtime.binder_prepare_transaction(g_remote, &in);
  if (status != STATUS_OK || in == nullptr) {
    LOGE("streamclient: prepareTransaction(play) failed status=%d", status);
    return false;
  }

  status = g_binder_runtime.parcel_write_byte_array(
      in, reinterpret_cast<const int8_t *>(path), static_cast<int32_t>(path_length));
  if (status != STATUS_OK) {
    LOGE("streamclient: write play path failed status=%d", status);
    g_binder_runtime.parcel_delete(in);
    return false;
  }

  AParcel *out = nullptr;
  status = g_binder_runtime.binder_transact(g_remote, kTxnPlayFile, &in, &out, 0);
  if (status != STATUS_OK) {
    LOGE("streamclient: transact play failed status=%d path=%.*s", status,
         static_cast<int>(path_length), path);
    if (out != nullptr) g_binder_runtime.parcel_delete(out);
    return false;
  }
  if (out != nullptr) g_binder_runtime.parcel_delete(out);
  return true;
}

bool TransactStopPlaybackLocked() {
  if (!EnsureConnectedLocked()) return false;

  AParcel *in = nullptr;
  binder_status_t status = g_binder_runtime.binder_prepare_transaction(g_remote, &in);
  if (status != STATUS_OK || in == nullptr) {
    LOGE("streamclient: prepareTransaction(stop) failed status=%d", status);
    return false;
  }

  AParcel *out = nullptr;
  status = g_binder_runtime.binder_transact(g_remote, kTxnStopPlayback, &in, &out, 0);
  if (status != STATUS_OK) {
    LOGE("streamclient: transact stop failed status=%d", status);
    if (out != nullptr) g_binder_runtime.parcel_delete(out);
    return false;
  }
  if (out != nullptr) g_binder_runtime.parcel_delete(out);
  return true;
}

bool TransactClearLocked() {
  if (!EnsureConnectedLocked()) return false;

  AParcel *in = nullptr;
  binder_status_t status = g_binder_runtime.binder_prepare_transaction(g_remote, &in);
  if (status != STATUS_OK || in == nullptr) {
    LOGE("streamclient: prepareTransaction(clear) failed status=%d", status);
    return false;
  }

  AParcel *out = nullptr;
  status = g_binder_runtime.binder_transact(g_remote, kTxnClearFrame, &in, &out, 0);
  if (status != STATUS_OK) {
    LOGE("streamclient: transact clear failed status=%d", status);
    if (out != nullptr) g_binder_runtime.parcel_delete(out);
    return false;
  }
  if (out != nullptr) g_binder_runtime.parcel_delete(out);
  return true;
}

}  // namespace
}  // namespace awesomecam

extern "C" JNIEXPORT jboolean JNICALL
Java_com_namnh_awesomecam_VideoBridge_nativeConnect(JNIEnv *, jclass) {
  std::lock_guard<std::mutex> lock(awesomecam::g_client_mutex);
  return awesomecam::EnsureConnectedLocked() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_namnh_awesomecam_VideoBridge_nativePushFrame(JNIEnv *env, jclass, jint width,
                                                      jint height, jint format,
                                                      jbyteArray frameData) {
  if (frameData == nullptr) return JNI_FALSE;
  const jsize length = env->GetArrayLength(frameData);
  jboolean is_copy = JNI_FALSE;
  jbyte *bytes = env->GetByteArrayElements(frameData, &is_copy);
  if (bytes == nullptr) return JNI_FALSE;

  int32_t generation = 0;
  bool ok = false;
  {
    std::lock_guard<std::mutex> lock(awesomecam::g_client_mutex);
    ok = awesomecam::TransactSetFrameLocked(width, height, format, bytes, length,
                                            &generation);
  }
  env->ReleaseByteArrayElements(frameData, bytes, JNI_ABORT);
  return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_namnh_awesomecam_VideoBridge_nativeClearFrame(JNIEnv *, jclass) {
  std::lock_guard<std::mutex> lock(awesomecam::g_client_mutex);
  return awesomecam::TransactClearLocked() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_namnh_awesomecam_VideoBridge_nativePlayFile(JNIEnv *env, jclass, jstring path) {
  if (path == nullptr) return JNI_FALSE;
  const char *utf = env->GetStringUTFChars(path, nullptr);
  if (utf == nullptr) return JNI_FALSE;
  const size_t len = strlen(utf);
  bool ok = false;
  {
    std::lock_guard<std::mutex> lock(awesomecam::g_client_mutex);
    ok = awesomecam::TransactPlayFileLocked(utf, len);
  }
  env->ReleaseStringUTFChars(path, utf);
  return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_namnh_awesomecam_VideoBridge_nativeStopPlayback(JNIEnv *, jclass) {
  std::lock_guard<std::mutex> lock(awesomecam::g_client_mutex);
  return awesomecam::TransactStopPlaybackLocked() ? JNI_TRUE : JNI_FALSE;
}
