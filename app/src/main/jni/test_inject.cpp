#include <unistd.h>
#include <android/log.h>

#define LOG_TAG "awesomeCAM"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

__attribute__((constructor))
static void log_constructor_loaded(void) {
    ALOGI("test_inject constructor ran in pid=%d", getpid());
}
