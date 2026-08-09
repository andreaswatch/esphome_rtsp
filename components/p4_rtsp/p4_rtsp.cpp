#include "p4_rtsp.h"

#include <cmath>
#include <vector>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef USE_MICROPHONE
#include "esphome/components/microphone/microphone.h"
#endif
#ifdef USE_SPEAKER
#include "esphome/components/speaker/speaker.h"
#endif
#include "camera_pipeline.h"
#include "rtsp_server.h"

namespace esphome {
namespace p4_rtsp {

static const char *const TAG = "p4_rtsp";

static const char *const COMPONENT_VERSION = "0.3.0-audio-stack";

void P4RtspStream::setup() {
  ESP_LOGCONFIG(TAG, "Setting up P4RTSP stream");

  this->server_ = std::make_unique<RtspServer>(
      this->port_, this->audio_sample_rate_, this->audio_channels_);
  this->server_->set_video_info(this->video_enabled_ ? this->video_width_ : 0,
                                this->video_enabled_ ? this->video_height_ : 0,
                                this->video_fps_);
  this->server_->set_backchannel_callback(
      [this](const int16_t *data, size_t samples) {
        this->on_backchannel_audio_(data, samples);
      });

  if (this->video_enabled_) {
    this->camera_ = std::make_unique<CameraPipeline>();
    this->camera_->set_config(this->video_width_, this->video_height_,
                              this->video_fps_, this->video_bitrate_,
                              this->video_gop_, this->sccb_sda_,
                              this->sccb_scl_, this->xclk_pin_,
                              this->data_lanes_, this->camera_power_down_pin_);
    this->camera_->set_frame_callback([this](const uint8_t *data, size_t len,
                                             bool keyframe,
                                             uint32_t timestamp_ms) {
      this->server_->push_video_frame(data, len, keyframe, timestamp_ms);
    });
  }

  if (this->microphone_ != nullptr) {
    this->microphone_->add_data_callback(
        [this](const std::vector<uint8_t> &data) {
          this->on_audio_bytes_(data);
        });
  }

  this->server_->start();
  this->server_started_ = true;

  if (this->video_always_on_) {
    this->start_streaming_();
  }
}

void P4RtspStream::loop() {
  uint32_t now = millis();
  if (now - this->last_version_log_ms_ > 30000) {
    this->last_version_log_ms_ = now;
    ESP_LOGI(TAG, "p4_rtsp component %s running (built %s %s)",
             COMPONENT_VERSION, __DATE__, __TIME__);
  }
  this->update_streams_();
}

void P4RtspStream::start_streaming_() {
  // Legacy entry point kept as a full-on switch; stream gating happens in
  // update_streams_().
  this->update_streams_();
}

void P4RtspStream::stop_streaming_() {
  if (this->camera_running_) {
    this->camera_->stop();
    this->camera_running_ = false;
    ESP_LOGI(TAG, "Camera pipeline stopped");
  }
  this->camera_failed_ = false;
  if (this->mic_started_) {
    this->microphone_->stop();
    this->mic_started_ = false;
    ESP_LOGI(TAG, "Microphone stopped");
  }
}

void P4RtspStream::update_streams_() {
  const bool active =
      this->video_always_on_ ||
      (this->server_started_ && this->server_->has_clients());

  // Camera / H.264 video track.
  const bool want_camera =
      active && this->video_enabled_ && this->video_stream_enabled_ &&
      this->camera_ != nullptr;
  if (want_camera) {
    if (!this->camera_running_ && !this->camera_failed_) {
      // Launch the (potentially slow) sensor/ISP/CSI setup in its own task so
      // loopTask never trips the watchdog.
      if (!this->camera_->starting() && !this->camera_->running()) {
        this->camera_->start_async();
      }
      if (this->camera_->running()) {
        this->camera_running_ = true;
        ESP_LOGI(TAG, "Camera pipeline started");
      } else if (!this->camera_->starting()) {
        // Start task ran and did not end in a running pipeline → permanent
        // failure.
        this->camera_failed_ = true;
        ESP_LOGE(TAG, "Camera pipeline failed to start");
      }
    }
  } else if (this->camera_running_) {
    this->camera_->stop();
    this->camera_running_ = false;
    this->camera_failed_ = false;
    ESP_LOGI(TAG, "Camera pipeline stopped");
  }

  // Microphone (16 kHz post-AEC → RTSP L16 track).
  const bool want_mic =
      active && this->mic_stream_enabled_ && this->microphone_ != nullptr;
  if (want_mic) {
    if (!this->mic_started_) {
      this->microphone_->start();
      this->mic_started_ = true;
      ESP_LOGI(TAG, "Microphone started");
    }
  } else if (this->mic_started_) {
    this->microphone_->stop();
    this->mic_started_ = false;
    ESP_LOGI(TAG, "Microphone stopped");
  }
}

void P4RtspStream::set_video_stream_enabled(bool enabled) {
  this->video_stream_enabled_ = enabled;
  this->update_streams_();
  ESP_LOGI(TAG, "Video stream %s", enabled ? "enabled" : "disabled");
}

void P4RtspStream::set_mic_stream_enabled(bool enabled) {
  this->mic_stream_enabled_ = enabled;
  this->update_streams_();
  ESP_LOGI(TAG, "Mic stream %s", enabled ? "enabled" : "disabled");
}

void P4RtspStream::set_speaker_enabled(bool enabled) {
  this->speaker_enabled_ = enabled;
  ESP_LOGI(TAG, "Speaker %s", enabled ? "enabled" : "disabled");
}

void P4RtspStream::on_audio_bytes_(const std::vector<uint8_t> &data) {
  if (this->server_ != nullptr && !data.empty()) {
    this->server_->push_audio_data(data.data(), data.size());
  }
}

void P4RtspStream::on_backchannel_audio_(const int16_t *data, size_t samples) {
  if (this->speaker_ == nullptr || samples == 0 || !this->speaker_enabled_) {
    return;
  }
  // Full-duplex audio stack: no bus arbitration needed, just feed the
  // 16 kHz mono PCM into the speaker surface and let the stack play it.
  this->backchannel_samples_.resize(samples * 2);
  for (size_t i = 0; i < samples; i++) {
    this->backchannel_samples_[i * 2] = static_cast<uint8_t>(data[i] & 0xff);
    this->backchannel_samples_[i * 2 + 1] =
        static_cast<uint8_t>((data[i] >> 8) & 0xff);
  }
  this->speaker_->play(this->backchannel_samples_.data(),
                       this->backchannel_samples_.size());
}

void P4RtspStream::play_test_tone() {
  if (this->speaker_ == nullptr || !this->speaker_enabled_) {
    ESP_LOGW(TAG, "no speaker configured or speaker disabled");
    return;
  }
  // "Ding-Dong" chime, ~3 s total. Each note is a bell-like decaying tone with
  // a few inharmonic partials instead of a plain sine.
  const float sample_rate = static_cast<float>(this->audio_sample_rate_);
  std::vector<int16_t> samples;
  samples.reserve(static_cast<size_t>(sample_rate * 3.1f));

  const auto add_bell = [&](float freq_hz, float duration_s, float tau_s,
                            float amp) {
    size_t n = static_cast<size_t>(sample_rate * duration_s);
    for (size_t i = 0; i < n; i++) {
      float t = static_cast<float>(i) / sample_rate;
      float env = std::exp(-t / tau_s);
      float s = std::sin(2.0f * M_PI * freq_hz * t);
      s += 0.28f * std::sin(2.0f * M_PI * freq_hz * 2.0f * t);
      s += 0.12f * std::sin(2.0f * M_PI * freq_hz * 2.74f * t);
      s += 0.06f * std::sin(2.0f * M_PI * freq_hz * 4.0f * t);
      samples.push_back(static_cast<int16_t>(env * s * amp));
    }
  };

  add_bell(880.0f, 0.9f, 0.20f, 11000.0f);  // "Ding" (A5)
  size_t silence = static_cast<size_t>(sample_rate * 0.2f);
  for (size_t i = 0; i < silence; i++) {
    samples.push_back(0);
  }
  add_bell(587.33f, 1.9f, 0.45f, 10000.0f);  // "Dong" (D5)

  this->speaker_->play(reinterpret_cast<const uint8_t *>(samples.data()),
                       samples.size() * 2);
  ESP_LOGI(TAG, "played ding-dong: %u samples (%.2f s)",
           static_cast<unsigned>(samples.size()),
           static_cast<float>(samples.size()) / sample_rate);
}

bool P4RtspStream::has_active_stream() const {
  return this->server_ != nullptr && this->server_->has_clients();
}

float P4RtspStream::get_setup_priority() const {
  return setup_priority::AFTER_WIFI;
}

void P4RtspStream::dump_config() {
  ESP_LOGCONFIG(TAG, "P4RTSP Stream:");
  ESP_LOGCONFIG(TAG, "  Port: %u", this->port_);
  ESP_LOGCONFIG(TAG, "  Video: %s",
                this->video_enabled_ ? "enabled" : "disabled");
  if (this->video_enabled_) {
    ESP_LOGCONFIG(TAG, "    Resolution: %dx%d", this->video_width_,
                  this->video_height_);
    ESP_LOGCONFIG(TAG, "    FPS: %d", this->video_fps_);
    ESP_LOGCONFIG(TAG, "    Bitrate: %d", this->video_bitrate_);
    ESP_LOGCONFIG(TAG, "    GOP: %d", this->video_gop_);
  }
  ESP_LOGCONFIG(TAG, "  Audio: %d Hz, %d channel(s)", this->audio_sample_rate_,
                this->audio_channels_);
  ESP_LOGCONFIG(TAG, "    Microphone: %s",
                this->microphone_ != nullptr ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "    Speaker: %s",
                this->speaker_ != nullptr ? "yes" : "no");
}

} // namespace p4_rtsp
} // namespace esphome
