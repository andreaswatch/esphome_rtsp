#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "esphome/components/microphone/microphone.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"

namespace esphome {
namespace p4_rtsp {

class RtspServer;
class CameraPipeline;

class P4RtspStream : public Component {
 public:
  void setup() override;
  void loop() override;
  float get_setup_priority() const override;
  void dump_config() override;

  void set_port(uint16_t port) { this->port_ = port; }
  void set_video_enabled(bool enabled) { this->video_enabled_ = enabled; }
  void set_video_resolution(int width, int height) {
    this->video_width_ = width;
    this->video_height_ = height;
  }
  void set_video_fps(int fps) { this->video_fps_ = fps; }
  void set_video_bitrate(int bitrate) { this->video_bitrate_ = bitrate; }
  void set_video_gop(int gop) { this->video_gop_ = gop; }
  void set_camera_pins(int sda, int scl, int xclk, int lanes) {
    this->sccb_sda_ = sda;
    this->sccb_scl_ = scl;
    this->xclk_pin_ = xclk;
    this->data_lanes_ = lanes;
  }
  void set_microphone(microphone::Microphone *microphone) { this->microphone_ = microphone; }
  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }
  void set_audio_sample_rate(int sample_rate) { this->audio_sample_rate_ = sample_rate; }
  void set_audio_channels(int channels) { this->audio_channels_ = channels; }

  bool has_active_stream() const;

 protected:
  void start_streaming_();
  void stop_streaming_();
  void on_audio_bytes_(const std::vector<uint8_t> &data);
  void on_backchannel_audio_(const int16_t *data, size_t samples);

  uint16_t port_{554};
  bool video_enabled_{false};
  int video_width_{1280};
  int video_height_{720};
  int video_fps_{25};
  int video_bitrate_{4000000};
  int video_gop_{25};
  int sccb_sda_{7};
  int sccb_scl_{8};
  int xclk_pin_{40};
  int data_lanes_{2};
  microphone::Microphone *microphone_{nullptr};
  speaker::Speaker *speaker_{nullptr};
  int audio_sample_rate_{16000};
  int audio_channels_{1};

  std::vector<uint8_t> backchannel_samples_;
  std::unique_ptr<RtspServer> server_;
  std::unique_ptr<CameraPipeline> camera_;
  bool server_started_{false};
  bool camera_running_{false};
  bool mic_started_{false};
};

}  // namespace p4_rtsp
}  // namespace esphome
