#pragma once

#include <string>

namespace awesomecam {

bool StartNativePlayback(const std::string &path, std::string *error = nullptr);
void StopNativePlayback();
bool IsNativePlaybackRunning();

}  // namespace awesomecam
