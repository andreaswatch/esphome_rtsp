#pragma once

#include "esphome/components/button/button.h"
#include "esphome/core/component.h"

namespace esphome {
namespace p4_rtsp {

class P4RtspStream;

class TestToneButton : public button::Button, public Component {
 public:
  void set_stream(P4RtspStream *stream) { this->stream_ = stream; }

  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void press_action() override;

 protected:
  P4RtspStream *stream_{nullptr};
};

}  // namespace p4_rtsp
}  // namespace esphome
