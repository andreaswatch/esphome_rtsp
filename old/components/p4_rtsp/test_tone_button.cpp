#include "test_tone_button.h"

#include "p4_rtsp.h"

#include "esphome/core/log.h"

namespace esphome {
namespace p4_rtsp {

static const char *const TAG = "p4_rtsp.test_tone";

void TestToneButton::press_action() {
  if (this->stream_ == nullptr) {
    ESP_LOGE(TAG, "no p4_rtsp stream wired to test-tone button");
    return;
  }
  this->stream_->play_test_tone();
}

}  // namespace p4_rtsp
}  // namespace esphome
