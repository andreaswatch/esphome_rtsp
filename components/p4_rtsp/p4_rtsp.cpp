#include "p4_rtsp.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include "camera_pipeline.h"
#include "rtsp_server.h"

namespace esphome {
namespace p4_rtsp {

static const char *const TAG = "p4_rtsp";

void P4RtspStream::setup() {
  ESP_LOGCONFIG(TAG, "Setting up P4RTSP stream");

  this->server_ = std::make_unique<RtspServer>(this->port_, this->audio_sample_rate_, this->audio_channels_);
  this->server_->set_video_info(this->video_enabled_ ? this->video_width_ : 0,
                                this->video_enabled_ ? this->video_height_ : 0, this->video_fps_);
  this->server_->set_backchannel_callback([this](const int16_t *data, size_t samples) {
    this->on_backchannel_audio_(data, samples);
  });

  if (this->video_enabled_) {
    this->camera_ = std::make_unique<CameraPipeline>();
    this->camera_->set_config(this->video_width_, this->video_height_, this->video_fps_,
                              this->video_bitrate_, this->video_gop_, this->sccb_sda_, this->sccb_scl_,
                              this->xclk_pin_, this->data_lanes_);
    this->camera_->set_frame_callback([this](const uint8_t *data, size_t len, bool keyframe,
                                             uint32_t timestamp_ms) {
      this->server_->push_video_frame(data, len, keyframe, timestamp_ms);
    });
  }

  if (this->microphone_ != nullptr) {
    this->microphone_->add_data_callback(
        [this](const std::vector<uint8_t> &data) { this->on_audio_bytes_(data); });
  }

  this->server_->start();
  this->server_started_ = true;
}

void P4RtspStream::loop() {
  if (this->server_started_ && this->server_->has_clients()) {
    this->start_streaming_();
  } else if (this->camera_running_ || this->mic_started_) {
    this->stop_streaming_();
  }
}

void P4RtspStream::start_streaming_() {
  if (!this->camera_running_ && this->camera_ != nullptr) {
    if (this->camera_->start()) {
      this->camera_running_ = true;
      ESP_LOGI(TAG, "Camera pipeline started");
    }
  }
  if (!this->mic_started_ && this->microphone_ != nullptr) {
    this->microphone_->start();
    this->mic_started_ = true;
    ESP_LOGI(TAG, "Microphone started");
  }
}

void P4RtspStream::stop_streaming_() {
  if (this->camera_running_) {
    this->camera_->stop();
    this->camera_running_ = false;
    ESP_LOGI(TAG, "Camera pipeline stopped");
  }
  if (this->mic_started_) {
    this->microphone_->stop();
    this->mic_started_ = false;
    ESP_LOGI(TAG, "Microphone stopped");
  }
}

void P4RtspStream::on_audio_bytes_(const std::vector<uint8_t> &data) {
  if (this->server_ != nullptr && !data.empty()) {
    this->server_->push_audio_data(data.data(), data.size());
  }
}

void P4RtspStream::on_backchannel_audio_(const int16_t *data, size_t samples) {
  if (this->speaker_ == nullptr) {
    return;
  }
  this->backchannel_samples_.resize(samples * 2);
  for (size_t i = 0; i < samples; i++) {
    this->backchannel_samples_[i * 2] = static_cast<uint8_t>(data[i] & 0xff);
    this->backchannel_samples_[i * 2 + 1] = static_cast<uint8_t>((data[i] >> 8) & 0xff);
  }
  if (!this->speaker_->is_running()) {
    this->speaker_->start();
  }
  this->speaker_->play(this->backchannel_samples_.data(), this->backchannel_samples_.size());
}

bool P4RtspStream::has_active_stream() const {
  return this->server_ != nullptr && this->server_->has_clients();
}

float P4RtspStream::get_setup_priority() const { return setup_priority::AFTER_WIFI; }

void P4RtspStream::dump_config() {
  ESP_LOGCONFIG(TAG, "P4RTSP Stream:");
  ESP_LOGCONFIG(TAG, "  Port: %u", this->port_);
  ESP_LOGCONFIG(TAG, "  Video: %s", this->video_enabled_ ? "enabled" : "disabled");
  if (this->video_enabled_) {
    ESP_LOGCONFIG(TAG, "    Resolution: %dx%d", this->video_width_, this->video_height_);
    ESP_LOGCONFIG(TAG, "    FPS: %d", this->video_fps_);
    ESP_LOGCONFIG(TAG, "    Bitrate: %d", this->video_bitrate_);
    ESP_LOGCONFIG(TAG, "    GOP: %d", this->video_gop_);
  }
  ESP_LOGCONFIG(TAG, "  Audio: %d Hz, %d channel(s)", this->audio_sample_rate_, this->audio_channels_);
  ESP_LOGCONFIG(TAG, "    Microphone: %s", this->microphone_ != nullptr ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "    Speaker: %s", this->speaker_ != nullptr ? "yes" : "no");
}

}  // namespace p4_rtsp
}  // namespace esphome
