#include "stream_switch.h"

#include "p4_rtsp.h"

#include "esphome/core/log.h"

namespace esphome {
namespace p4_rtsp {

static const char *const TAG = "p4_rtsp.stream_switch";

void StreamSwitch::write_state(bool state) {
  if (this->stream_ == nullptr) {
    ESP_LOGE(TAG, "no p4_rtsp stream wired to switch");
    return;
  }
  switch (this->kind_) {
    case StreamKind::VIDEO:
      this->stream_->set_video_stream_enabled(state);
      break;
    case StreamKind::MIC:
      this->stream_->set_mic_stream_enabled(state);
      break;
    case StreamKind::SPEAKER:
      this->stream_->set_speaker_enabled(state);
      break;
  }
  this->publish_state(state);
}

}  // namespace p4_rtsp
}  // namespace esphome
