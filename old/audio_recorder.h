#pragma once
#include <vector>
#include <cstdint>

inline std::vector<int16_t>& get_recorded_audio() {
  static std::vector<int16_t> recorded_audio;
  return recorded_audio;
}

inline bool& get_is_recording() {
  static bool is_recording = false;
  return is_recording;
}
