#pragma once

#include <stdint.h>

namespace awesomecam {

static constexpr const char *kVideo2CameraServiceName = "Video2CameraService";
static constexpr const char *kVideo2CameraDescriptor =
    "com.namnh.awesomecam.Video2CameraService";

enum TransactionCode : uint32_t {
  kTxnSetFrame = 1,
  kTxnClearFrame = 2,
};

enum FrameFormat : int32_t {
  kFrameFormatUnknown = 0,
  kFrameFormatI420 = 1,
};

}  // namespace awesomecam
