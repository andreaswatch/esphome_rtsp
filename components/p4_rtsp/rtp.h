#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace esphome {
namespace p4_rtsp {

static constexpr size_t RTP_HEADER_SIZE = 12;
static constexpr size_t RTP_MTU_PAYLOAD = 1200;
static constexpr uint8_t RTP_PT_PCMU = 0;
static constexpr uint8_t RTP_PT_PCMA = 8;
static constexpr uint8_t RTP_PT_H264 = 96;
static constexpr uint8_t RTP_PT_L16 = 97;
static constexpr uint32_t RTP_CLOCK_VIDEO = 90000;

using RtpSendCallback = std::function<void(const uint8_t *data, size_t len)>;

void rtp_write_header(uint8_t *out, bool marker, uint8_t pt, uint16_t seq, uint32_t ts, uint32_t ssrc);

class H264Packetizer {
 public:
  H264Packetizer();
  void set_send_callback(RtpSendCallback callback);
  void set_ssrc(uint32_t ssrc);
  void set_payload_type(uint8_t pt);
  void push_annexb(const uint8_t *frame, size_t len, uint32_t timestamp_ms);
  const std::vector<uint8_t> &sps() const { return this->sps_; }
  const std::vector<uint8_t> &pps() const { return this->pps_; }
  uint16_t sequence_number() const { return this->seq_; }
  uint32_t ssrc() const { return this->ssrc_; }

 protected:
  void send_nal_(const uint8_t *nal, size_t len, uint32_t ts);
  void send_stapa_(std::vector<std::pair<const uint8_t *, size_t>> nals, uint32_t ts);
  void send_single_(const uint8_t *nal, size_t len, uint32_t ts, bool marker);
  void send_fua_(const uint8_t *nal, size_t len, uint32_t ts);

  RtpSendCallback send_callback_;
  uint32_t ssrc_{0};
  uint8_t payload_type_{RTP_PT_H264};
  uint16_t seq_{0};
  std::vector<uint8_t> sps_;
  std::vector<uint8_t> pps_;
};

class L16Packetizer {
 public:
  L16Packetizer();
  void set_send_callback(RtpSendCallback callback);
  void set_ssrc(uint32_t ssrc);
  void set_payload_type(uint8_t pt);
  void set_sample_rate(int rate);
  void set_channels(int channels);
  void push_bytes(const uint8_t *data, size_t len);

 protected:
  RtpSendCallback send_callback_;
  uint32_t ssrc_{0};
  uint8_t payload_type_{RTP_PT_L16};
  uint16_t seq_{0};
  int sample_rate_{16000};
  int channels_{1};
  uint32_t timestamp_{0};
};

// G.711 A-law (PCMA) packetizer. Accepts raw 16-bit little-endian PCM (as
// delivered by the I2S microphone) and emits 8-bit A-law RTP packets.
// go2rtc / Frigate / WebRTC handle PCMA natively, unlike L16/s16be.
class PCMAPacketizer {
 public:
  PCMAPacketizer();
  void set_send_callback(RtpSendCallback callback);
  void set_ssrc(uint32_t ssrc);
  void set_payload_type(uint8_t pt);
  // Input sample rate of the raw PCM stream handed to push_pcm16.
  void set_input_sample_rate(int rate);
  void set_channels(int channels);
  void push_pcm16(const uint8_t *data, size_t len);

  uint16_t sequence_number() const { return this->seq_; }

 protected:
  RtpSendCallback send_callback_;
  uint32_t ssrc_{0};
  uint8_t payload_type_{RTP_PT_PCMA};
  uint16_t seq_{0};
  int input_sample_rate_{16000};
  int channels_{1};
  uint32_t timestamp_{0};
};

}  // namespace p4_rtsp
}  // namespace esphome
