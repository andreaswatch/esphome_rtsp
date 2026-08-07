#include "p4_rtsp.h"

#include <cmath>
#include <vector>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "camera_pipeline.h"
#include "rtsp_server.h"

namespace esphome {
namespace p4_rtsp {

static const char *const TAG = "p4_rtsp";

static const char *const COMPONENT_VERSION = "0.2.0-csi-direct";

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
  if (this->video_always_on_) {
    this->start_streaming_();
    return;
  }
  bool rtsp_active = this->server_started_ && this->server_->has_clients();
  if (rtsp_active) {
    this->start_streaming_();
  } else if (this->camera_running_ || this->mic_started_) {
    this->stop_streaming_();
  }
}

void P4RtspStream::start_streaming_() {
  static int call_count = 0;
  if (++call_count <= 5 || call_count % 100 == 0) {
    ESP_LOGI(TAG, "start_streaming call #%d: mic_started=%d speaker_active=%d mic_ptr=%p",
             call_count, this->mic_started_, this->speaker_active_, this->microphone_);
  }
  if (!this->camera_running_ && this->camera_ != nullptr &&
      !this->camera_failed_) {
    if (!this->camera_->starting() && !this->camera_->running()) {
      // Launch the (potentially slow) sensor/ISP/CSI setup in its own task so
      // loopTask never trips the watchdog.
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
  if (!this->mic_started_ && !this->speaker_active_ &&
      this->microphone_ != nullptr) {
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
  this->camera_failed_ = false;
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
    this->backchannel_samples_[i * 2 + 1] =
        static_cast<uint8_t>((data[i] >> 8) & 0xff);
  }
  this->speaker_play_(this->backchannel_samples_.data(),
                      this->backchannel_samples_.size());
}

void P4RtspStream::speaker_task_wrapper(void *param) {
  auto *self = static_cast<P4RtspStream *>(param);
  self->run_speaker_sequence_();
  self->speaker_busy_ = false;
  vTaskDelete(nullptr);
}

void P4RtspStream::speaker_play_(const uint8_t *pcm, size_t len) {
  if (this->speaker_ == nullptr || len == 0 || this->speaker_busy_) {
    return;
  }
  // The bus arbitration below must not run on the ESPHome loop task: it blocks
  // in vTaskDelay while the microphone's own loop() (which runs on the loop
  // task) performs the async stop, so we would deadlock waiting for the mic to
  // release the bus. Spawn a one-shot task instead (camera uses the same
  // pattern for its slow startup).
  this->speaker_audio_.assign(pcm, pcm + len);
  this->speaker_busy_ = true;
  if (xTaskCreatePinnedToCore(P4RtspStream::speaker_task_wrapper, "spk_play",
                              8192, this, 5, nullptr, 1) != pdPASS) {
    ESP_LOGE(TAG, "failed to create speaker task");
    this->speaker_busy_ = false;
  }
}

void P4RtspStream::run_speaker_sequence_() {
  // The microphone holds the shared I2S bus. Stop it so the speaker can start,
  // wait for the speaker to actually run, play, then release the bus back to
  // the microphone.
  bool mic_was_running = this->mic_started_ && this->microphone_ != nullptr;
  if (mic_was_running) {
    // Suppress the always-on mic auto-restart while we own the bus, otherwise
    // loop() restarts the microphone the moment mic_started_ turns false and it
    // re-grabs the bus before the speaker can start.
    this->speaker_active_ = true;
    this->microphone_->stop();
    this->mic_started_ = false;
    // The mic stop is asynchronous: the I2S driver keeps holding the shared
    // bus lock until the mic task is torn down (state == STATE_STOPPED).
    // is_running() already turns false during STATE_STOPPING while the lock is
    // still held, so we must poll is_stopped() (same approach as the VoIP lib).
    for (int i = 0; i < 100 && !this->microphone_->is_stopped(); i++) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!this->microphone_->is_stopped()) {
      ESP_LOGE(TAG, "microphone did not release the I2S bus");
      this->speaker_active_ = false;
      this->mic_started_ = true;
      this->microphone_->start();
      return;
    }
  }

  if (this->speaker_->is_running()) {
    this->speaker_->stop();
  }
  this->speaker_->start();
  // The speaker start is async (state machine task). Give it time to grab the
  // now-free bus before feeding samples.
  for (int i = 0; i < 100 && !this->speaker_->is_running(); i++) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (!this->speaker_->is_running()) {
    // The speaker's internal retry loop keeps trying while state is STARTING
    // and no task exists yet, so stop() cannot abort it. Leave the bus alone:
    // the retry will succeed on its own once the lock is free.
    ESP_LOGE(TAG, "speaker did not start");
    this->speaker_active_ = false;
    if (mic_was_running && this->microphone_ != nullptr) {
      this->mic_started_ = true;
      this->microphone_->start();
    }
    return;
  }

  // The ES8311 DAC is muted by default after reset (REG31 bits 5/6). The
  // ESPHome es8311 component never calls set_mute_off() on its own, and
  // I2SAudioSpeaker::set_volume() only unmutes when explicitly invoked via
  // speaker.volume_set. Explicitly unmute + set full volume here so the
  // test tone / backchannel audio is actually audible.
  this->speaker_->set_mute_state(false);
  this->speaker_->set_volume(1.0f);

  this->speaker_->play(this->speaker_audio_.data(),
                       this->speaker_audio_.size());
  // Let the speaker drain its buffer, then wait until it fully stopped (state
  // == STATE_STOPPED, i.e. the bus lock was released) before handing the bus
  // back to the microphone.
  this->speaker_->finish();
  for (int i = 0; i < 100 && !this->speaker_->is_stopped(); i++) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (!this->speaker_->is_stopped()) {
    ESP_LOGE(TAG, "speaker did not release the I2S bus");
  }

  this->speaker_active_ = false;
  if (mic_was_running && this->microphone_ != nullptr) {
    this->mic_started_ = true;
    this->microphone_->start();
    for (int i = 0; i < 100 && !this->microphone_->is_running(); i++) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

void P4RtspStream::play_test_tone() {
  if (this->speaker_ == nullptr) {
    ESP_LOGE(TAG, "no speaker configured");
    return;
  }
  // ~60 ms 1 kHz sine with a fast decay → short audible click.
  const float freq_hz = 1000.0f;
  const float duration_s = 0.25f;
  size_t n = static_cast<size_t>(static_cast<float>(this->audio_sample_rate_) *
                                 duration_s);
  if (n == 0) {
    return;
  }
  std::vector<int16_t> samples(n);
  for (size_t i = 0; i < n; i++) {
    float t =
        static_cast<float>(i) / static_cast<float>(this->audio_sample_rate_);
    float env = 1.0f - (t / duration_s);
    samples[i] = static_cast<int16_t>(env * 12000.0f *
                                      std::sin(2.0f * M_PI * freq_hz * t));
  }
  this->speaker_play_(reinterpret_cast<const uint8_t *>(samples.data()),
                      samples.size() * 2);
  ESP_LOGI(TAG, "played test tone: %u samples", static_cast<unsigned>(n));
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
