#pragma once

#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"

namespace esphome {
namespace p4_rtsp {

class P4RtspStream;

enum class StreamKind : uint8_t { VIDEO = 0, MIC = 1, SPEAKER = 2 };

class StreamSwitch : public switch_::Switch, public Component {
 public:
  void set_stream(P4RtspStream *stream) { this->stream_ = stream; }
  void set_kind(uint8_t kind) { this->kind_ = static_cast<StreamKind>(kind); }

  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void write_state(bool state) override;

 protected:
  P4RtspStream *stream_{nullptr};
  StreamKind kind_{StreamKind::VIDEO};
};

}  // namespace p4_rtsp
}  // namespace esphome
