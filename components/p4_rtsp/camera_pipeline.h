#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace esphome {
namespace p4_rtsp {

using FrameCallback = std::function<void(const uint8_t *data, size_t len, bool keyframe,
                                         uint32_t timestamp_ms)>;

class CameraPipeline {
 public:
  CameraPipeline();
  ~CameraPipeline();

  void set_config(int width, int height, int fps, int bitrate, int gop, int sccb_sda, int sccb_scl,
                  int xclk_pin, int data_lanes);
  void set_frame_callback(FrameCallback callback);

  bool start();
  void stop();
  bool running() const { return this->running_; }

 protected:
  static void capture_task_wrapper(void *param);
  void capture_task();
  bool init_camera_();
  bool init_encoder_();
  void deinit_camera_();
  void deinit_encoder_();
  bool encode_frame_(const uint8_t *raw, size_t len, uint32_t timestamp_ms);

  int width_{1280};
  int height_{720};
  int fps_{25};
  int bitrate_{4000000};
  int gop_{25};
  int sccb_sda_{7};
  int sccb_scl_{8};
  int xclk_pin_{40};
  int data_lanes_{2};
  FrameCallback frame_callback_;

  bool running_{false};
  int video_fd_{-1};
  void *encoder_{nullptr};
  void *enc_buf_{nullptr};
  size_t enc_buf_len_{0};
  void *cap_buf_{nullptr};
  size_t cap_buf_len_{0};
  uint32_t frame_index_{0};
};

}  // namespace p4_rtsp
}  // namespace esphome
