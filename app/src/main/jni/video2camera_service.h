#pragma once

#include <stdint.h>

#include <vector>

namespace awesomecam {

struct BinderFrameCopy {
  int32_t width = 0;
  int32_t height = 0;
  int32_t format = 0;
  uint64_t generation = 0;
  std::vector<uint8_t> bytes;
};

bool EnsureVideo2CameraServiceStarted();
bool ProbeBinderRuntimeOnly();
bool ProbeBinderClassDefineOnly();
bool ProbeBinderNewOnly();
bool ProbeBinderThreadPoolOnly();
bool ProbeBinderFullServiceWorkerAsync();
bool ProbeBinderThreadPoolWorkerOnly();
bool ProbeClassicServiceWorkerAsync();
bool ProbeClassicThreadPoolWorkerOnly();
void StartVideo2CameraServiceAsync();
bool CopyLatestBinderFrame(BinderFrameCopy *out);
void ClearLatestBinderFrame();

}  // namespace awesomecam
