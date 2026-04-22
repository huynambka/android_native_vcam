#include <pthread.h>
#include <dlfcn.h>
#include <android/log.h>
#include <stdatomic.h>

#define LOG_TAG "awesomeCAM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static std::atomic<bool> g_started{false};

using main_hook_t = void (*)();

static void *loader_thread(void *) {
    const char *test = "/data/camera/libtest.so";

    dlerror();
    void *h1 = dlopen(test, RTLD_NOW | RTLD_GLOBAL);
    if (!h1) {
        LOGE("dlopen(%s) failed: %s", test, dlerror());
        return nullptr;
    }
    dlerror();
    auto fn = reinterpret_cast<main_hook_t>(dlsym(h1, "test_hook"));
    const char *err = dlerror();
    if (err != nullptr || fn == nullptr) {
        LOGE("dlsym(test_hook) failed: %s", err ? err : "NULL");
        return nullptr;
    }

    LOGI("calling test_hook: %p", reinterpret_cast<void *>(fn));
    fn();
    LOGI("test_hook returned");
    return nullptr;
}

__attribute__((constructor))
static void init_proxy() {
    bool expected = false;
    if (!atomic_compare_exchange_strong(&g_started, &expected, true)) {
        return;
    }

    pthread_t t;
    if (pthread_create(&t, nullptr, loader_thread, nullptr) == 0) {
        pthread_detach(t);
        LOGI("loader thread started");
    } else {
        LOGE("pthread_create failed");
        atomic_store(&g_started, false);
    }
}
