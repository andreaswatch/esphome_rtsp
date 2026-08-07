#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "esp_attr.h"
#include "esp_cam_ctlr_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace esphome {
namespace p4_rtsp {

using FrameCallback = std::function<void(const uint8_t *data, size_t len, bool keyframe,
                                         uint32_t timestamp_ms)>;

class CameraPipeline {
 public:
  CameraPipeline();
  ~CameraPipeline();

  void set_config(int width, int height, int fps, int bitrate, int gop, int sccb_sda, int sccb_scl,
                  int xclk_pin, int data_lanes, int power_down_pin);
  void set_frame_callback(FrameCallback callback);

  bool start();
  // Kick off start() in its own task so the caller (loopTask) never blocks on
  // sensor/ISP/CSI setup (which can exceed the task watchdog). Returns true if
  // the start task was launched (or pipeline already running/starting).
  bool start_async();
  void stop();
  bool running() const { return this->running_; }
  bool starting() const { return this->starting_; }
  bool start_succeeded() const { return this->start_succeeded_; }

  protected:
   static void start_task_wrapper(void *param);
   static void capture_task_wrapper(void *param);
   void capture_task();
  bool init_sensor_();
  bool setup_isp_();
  bool setup_csi_();
  bool init_encoder_();
  void deinit_encoder_();
  bool encode_frame_(const uint8_t *raw, size_t len, uint32_t timestamp_ms);

  static bool IRAM_ATTR s_dma_start(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans,
                                    void *user_data);
  static bool IRAM_ATTR s_dma_complete(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans,
                                       void *user_data);
  bool dma_start_cb_(esp_cam_ctlr_trans_t *trans);
  bool dma_complete_cb_(esp_cam_ctlr_trans_t *trans);

  int width_{1280};
  int height_{720};
  int fps_{25};
  int bitrate_{4000000};
  int gop_{25};
  int sccb_sda_{7};
  int sccb_scl_{8};
  int xclk_pin_{40};
  int data_lanes_{2};
  int power_down_pin_{-1};
  void *ldo_{nullptr};
  FrameCallback frame_callback_;

  bool running_{false};
  volatile bool starting_{false};
  volatile bool start_succeeded_{false};
  void *sensor_{nullptr};
  void *cam_handle_{nullptr};
  void *isp_proc_{nullptr};
  void *sccb_bus_{nullptr};
  void *sccb_io_{nullptr};
  void *xclk_{nullptr};
  QueueHandle_t produced_{nullptr};
  QueueHandle_t consumed_{nullptr};
  void *cap_bufs_[2]{nullptr, nullptr};
  size_t fb_size_{0};
  uint32_t target_width_{0};
  uint32_t target_height_{0};
  uint8_t lane_count_{2};
  int lane_bitrate_mbps_{912};
  bool raw8_{true};
  void *encoder_{nullptr};
  void *enc_buf_{nullptr};
  size_t enc_buf_len_{0};
  uint32_t frame_index_{0};
};

}  // namespace p4_rtsp
}  // namespace esphome
