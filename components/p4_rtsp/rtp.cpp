#include "rtp.h"

#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace p4_rtsp {

static const char *const TAG = "p4_rtsp.rtp";

static const uint8_t NAL_TYPE_SPS = 7;
static const uint8_t NAL_TYPE_PPS = 8;
static const uint8_t NAL_TYPE_IDR = 5;
static const uint8_t NAL_TYPE_STAP_A = 24;
static const uint8_t NAL_TYPE_FU_A = 28;
static const uint8_t NAL_FU_BIT = 0x80;
static const uint8_t NAL_EBIT = 0x40;
static const size_t MAX_RTP_PACKET = RTP_HEADER_SIZE + RTP_MTU_PAYLOAD;

void rtp_write_header(uint8_t *out, bool marker, uint8_t pt, uint16_t seq, uint32_t ts, uint32_t ssrc) {
  out[0] = 0x80;
  out[1] = (marker ? 0x80 : 0x00) | (pt & 0x7f);
  out[2] = static_cast<uint8_t>(seq >> 8);
  out[3] = static_cast<uint8_t>(seq & 0xff);
  out[4] = static_cast<uint8_t>(ts >> 24);
  out[5] = static_cast<uint8_t>(ts >> 16);
  out[6] = static_cast<uint8_t>(ts >> 8);
  out[7] = static_cast<uint8_t>(ts & 0xff);
  out[8] = static_cast<uint8_t>(ssrc >> 24);
  out[9] = static_cast<uint8_t>(ssrc >> 16);
  out[10] = static_cast<uint8_t>(ssrc >> 8);
  out[11] = static_cast<uint8_t>(ssrc & 0xff);
}

H264Packetizer::H264Packetizer() { this->ssrc_ = random_uint32(); }

void H264Packetizer::set_send_callback(RtpSendCallback callback) { this->send_callback_ = std::move(callback); }

void H264Packetizer::set_ssrc(uint32_t ssrc) { this->ssrc_ = ssrc; }

void H264Packetizer::set_payload_type(uint8_t pt) { this->payload_type_ = pt; }

void H264Packetizer::push_annexb(const uint8_t *frame, size_t len, uint32_t timestamp_ms) {
  uint32_t ts = static_cast<uint32_t>((static_cast<uint64_t>(timestamp_ms) * RTP_CLOCK_VIDEO) / 1000);
  size_t i = 0;
  while (i < len) {
    size_t start = i;
    while (i < len && frame[i] == 0x00) {
      i++;
    }
    if (i >= len || frame[i] != 0x01) {
      i++;
      continue;
    }
    i++;
    size_t nal_start = i;
    size_t nal_end = nal_start;
    while (nal_end < len) {
      if (nal_end + 3 < len && frame[nal_end] == 0x00 && frame[nal_end + 1] == 0x00 &&
          frame[nal_end + 2] == 0x01) {
        break;
      }
      if (nal_end + 4 < len && frame[nal_end] == 0x00 && frame[nal_end + 1] == 0x00 &&
          frame[nal_end + 2] == 0x00 && frame[nal_end + 3] == 0x01) {
        break;
      }
      nal_end++;
    }
    if (nal_end == nal_start) {
      i = start + 1;
      continue;
    }
    const uint8_t *nal = frame + nal_start;
    size_t nal_len = nal_end - nal_start;
    uint8_t type = nal[0] & 0x1f;
    if (type == NAL_TYPE_SPS && nal_len > 4) {
      this->sps_.assign(nal, nal + nal_len);
    } else if (type == NAL_TYPE_PPS && nal_len >= 1) {
      // PPS for H.264 CBP can be short (2-4 bytes); use >= 1 to avoid missing it.
      this->pps_.assign(nal, nal + nal_len);
    }
    this->send_nal_(nal, nal_len, ts);
    i = nal_end;
  }
}

void H264Packetizer::send_nal_(const uint8_t *nal, size_t len, uint32_t ts) {
  uint8_t type = nal[0] & 0x1f;
  size_t single_overhead = RTP_HEADER_SIZE + 1;
  if (len <= RTP_MTU_PAYLOAD - 1) {
    this->send_single_(nal, len, ts, type == NAL_TYPE_IDR);
  } else {
    this->send_fua_(nal, len, ts);
  }
}

void H264Packetizer::send_stapa_(std::vector<std::pair<const uint8_t *, size_t>> nals, uint32_t ts) {
  size_t total = RTP_HEADER_SIZE + 1;
  for (auto &nal : nals) {
    total += 2 + nal.second;
  }
  std::vector<uint8_t> pkt(total);
  rtp_write_header(pkt.data(), false, this->payload_type_, this->seq_, ts, this->ssrc_);
  size_t pos = RTP_HEADER_SIZE;
  pkt[pos++] = (NAL_FU_BIT) | NAL_TYPE_STAP_A;
  for (auto &nal : nals) {
    pkt[pos++] = static_cast<uint8_t>(nal.second >> 8);
    pkt[pos++] = static_cast<uint8_t>(nal.second & 0xff);
    std::memcpy(pkt.data() + pos, nal.first, nal.second);
    pos += nal.second;
  }
  this->seq_++;
  if (this->send_callback_) {
    this->send_callback_(pkt.data(), pkt.size());
  }
}

void H264Packetizer::send_single_(const uint8_t *nal, size_t len, uint32_t ts, bool marker) {
  std::vector<uint8_t> pkt(RTP_HEADER_SIZE + len);
  rtp_write_header(pkt.data(), marker, this->payload_type_, this->seq_, ts, this->ssrc_);
  std::memcpy(pkt.data() + RTP_HEADER_SIZE, nal, len);
  this->seq_++;
  if (this->send_callback_) {
    this->send_callback_(pkt.data(), pkt.size());
  }
}

void H264Packetizer::send_fua_(const uint8_t *nal, size_t len, uint32_t ts) {
  uint8_t fu_indicator = static_cast<uint8_t>((nal[0] & 0xe0) | NAL_TYPE_FU_A);
  uint8_t fu_header_base = static_cast<uint8_t>(nal[0] & 0x1f);
  size_t payload = len - 1;
  size_t offset = 1;
  bool first = true;
  while (payload > 0) {
    size_t chunk = payload > RTP_MTU_PAYLOAD - 2 ? RTP_MTU_PAYLOAD - 2 : payload;
    std::vector<uint8_t> pkt(RTP_HEADER_SIZE + 2 + chunk);
    bool last = (chunk == payload);
    rtp_write_header(pkt.data(), last, this->payload_type_, this->seq_, ts, this->ssrc_);
    pkt[RTP_HEADER_SIZE] = fu_indicator;
    uint8_t fu_header = fu_header_base;
    if (first) {
      fu_header |= NAL_FU_BIT;
    }
    if (last) {
      fu_header |= NAL_EBIT;
    }
    pkt[RTP_HEADER_SIZE + 1] = fu_header;
    std::memcpy(pkt.data() + RTP_HEADER_SIZE + 2, nal + offset, chunk);
    offset += chunk;
    payload -= chunk;
    first = false;
    this->seq_++;
    if (this->send_callback_) {
      this->send_callback_(pkt.data(), pkt.size());
    }
  }
}

L16Packetizer::L16Packetizer() { this->ssrc_ = random_uint32(); }

void L16Packetizer::set_send_callback(RtpSendCallback callback) { this->send_callback_ = std::move(callback); }

void L16Packetizer::set_ssrc(uint32_t ssrc) { this->ssrc_ = ssrc; }

void L16Packetizer::set_payload_type(uint8_t pt) { this->payload_type_ = pt; }

void L16Packetizer::set_sample_rate(int rate) { this->sample_rate_ = rate; }

void L16Packetizer::set_channels(int channels) { this->channels_ = channels; }

void L16Packetizer::push_bytes(const uint8_t *data, size_t len) {
  static const size_t PACKET_DURATION_MS = 20;
  size_t samples_per_packet = (static_cast<size_t>(this->sample_rate_) * PACKET_DURATION_MS) / 1000;
  if (samples_per_packet == 0) {
    samples_per_packet = 160;
  }
  size_t bytes_per_packet = samples_per_packet * 2;
  size_t offset = 0;
  while (offset + 1 < len) {
    size_t chunk_bytes = len - offset;
    if (chunk_bytes > bytes_per_packet) {
      chunk_bytes = bytes_per_packet;
    }
    if (chunk_bytes & 1) {
      chunk_bytes--;
    }
    bool last = (offset + chunk_bytes >= len);
    size_t samples = chunk_bytes / 2;
    std::vector<uint8_t> pkt(RTP_HEADER_SIZE + chunk_bytes);
    rtp_write_header(pkt.data(), last, this->payload_type_, this->seq_, this->timestamp_, this->ssrc_);
    for (size_t i = 0; i < samples; i++) {
      uint8_t lo = data[offset + i * 2];
      uint8_t hi = data[offset + i * 2 + 1];
      pkt[RTP_HEADER_SIZE + i * 2] = hi;
      pkt[RTP_HEADER_SIZE + i * 2 + 1] = lo;
    }
    offset += chunk_bytes;
    this->seq_++;
    this->timestamp_ += static_cast<uint32_t>(samples);
    if (this->send_callback_) {
      this->send_callback_(pkt.data(), pkt.size());
    }
  }
}

}  // namespace p4_rtsp
}  // namespace esphome
