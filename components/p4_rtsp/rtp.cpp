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

// Exp-Golomb read helpers for SPS parsing.
static uint32_t read_ue(const uint8_t *data, size_t len, size_t *bit_pos) {
  uint32_t zeros = 0;
  size_t pos = *bit_pos;
  while (pos < len * 8 && ((data[pos >> 3] >> (7 - (pos & 7))) & 1) == 0) {
    zeros++;
    pos++;
  }
  pos++;  // consume the leading '1'
  uint32_t value = 1;
  for (uint32_t i = 0; i < zeros; i++) {
    value = (value << 1) | (((data[pos >> 3] >> (7 - (pos & 7))) & 1));
    pos++;
  }
  *bit_pos = pos;
  return value - 1;
}

// The ESP32-P4 HW encoder emits an SPS whose VUI (video usability information)
// is truncated: it sets vui_parameters_present_flag=1 but the VUI payload is
// cut off mid-way. ffmpeg/MSE tolerate this ("Overread VUI by 8 bits"), but
// strict WebRTC decoders (Firefox/Chromium) refuse the whole stream -> black
// video. Parse the SPS, copy the prefix up to vui_parameters_present_flag,
// clear that flag and append rbsp_stop_one_bit. Writes the sanitized SPS into
// `out` (capacity at least `in_len`) and returns its length; returns 0 when
// the SPS has no (truncated) VUI and needs no fix.
static size_t sanitize_sps(const uint8_t *sps, size_t in_len, uint8_t *out) {
  if (in_len < 8) {
    return 0;
  }
  size_t pos = 8;  // skip NAL header
  pos += 24;       // profile_idc, constraint flags, level_idc (fixed 8 bits each)
  read_ue(sps, in_len, &pos);  // seq_parameter_set_id
  read_ue(sps, in_len, &pos);  // log2_max_frame_num_minus4
  uint32_t poc_type = read_ue(sps, in_len, &pos);
  if (poc_type == 0) {
    read_ue(sps, in_len, &pos);  // log2_max_pic_order_cnt_lsb_minus4
  } else if (poc_type == 1) {
    uint32_t delta = read_ue(sps, in_len, &pos);
    uint32_t n = read_ue(sps, in_len, &pos);
    for (uint32_t i = 0; i < n * 2 + delta; i++) {
      read_ue(sps, in_len, &pos);
    }
  }
  read_ue(sps, in_len, &pos);  // max_num_ref_frames
  pos += 1;                     // gaps_in_frame_num_value_allowed_flag
  read_ue(sps, in_len, &pos);  // pic_width_in_mbs_minus1
  read_ue(sps, in_len, &pos);  // pic_height_in_map_units_minus1
  pos += 1;                     // frame_mbs_only_flag
  pos += 1;                     // direct_8x8_inference_flag
  uint8_t cropping = (sps[pos >> 3] >> (7 - (pos & 7))) & 1;
  pos += 1;
  if (cropping) {
    for (int i = 0; i < 4; i++) {
      read_ue(sps, in_len, &pos);
    }
  }
  if (pos >= in_len * 8) {
    return 0;  // SPS too short, cannot locate VUI flag
  }
  // vui_parameters_present_flag must be 1 for the truncation case.
  if (((sps[pos >> 3] >> (7 - (pos & 7))) & 1) != 1) {
    return 0;  // no VUI, nothing to fix
  }
  // Copy bits [0, pos) verbatim, then set vui flag to 0, then a stop bit,
  // then zero padding.
  const size_t vui_bit = pos;
  const size_t out_bits = vui_bit + 2;  // cleared flag + stop bit
  const size_t out_len = (out_bits + 7) / 8;
  if (out_len > in_len) {
    return 0;
  }
  size_t copy_bytes = (vui_bit + 7) / 8;
  std::memcpy(out, sps, copy_bytes);
  out[vui_bit >> 3] &= static_cast<uint8_t>(~(0x80 >> (vui_bit & 7)));  // clear vui flag
  const size_t stop_bit = vui_bit + 1;
  out[stop_bit >> 3] |= (0x80 >> (stop_bit & 7));  // set rbsp_stop_one_bit
  // Zero-fill any trailing padding in the final byte.
  if (out_len * 8 > out_bits) {
    const uint8_t mask = static_cast<uint8_t>(0xFF << (8 - (out_len * 8 - out_bits)));
    out[out_len - 1] &= mask;
  }
  return out_len;
}

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
      // The HW encoder emits a truncated SPS (VUI cut off mid-way). Strict
      // WebRTC decoders reject it -> black video. Sanitize it: drop the VUI so
      // the SPS is standards-compliant (ffmpeg already tolerated the truncation,
      // this keeps the exact encoder dimensions/profile).
      uint8_t sanitized[64];
      size_t fixed = sanitize_sps(nal, nal_len, sanitized);
      if (fixed > 0 && fixed <= sizeof(sanitized)) {
        this->sps_.assign(sanitized, sanitized + fixed);
        nal = this->sps_.data();
        nal_len = this->sps_.size();
      } else {
        this->sps_.assign(nal, nal + nal_len);
      }
    } else if (type == NAL_TYPE_PPS && nal_len >= 1) {
      // PPS for H.264 CBP can be short (2-4 bytes); use >= 1 to avoid missing it.
      this->pps_.assign(nal, nal + nal_len);
    }
    // The RTP marker bit must be set on the LAST packet of each access unit
    // (frame), not only for IDR NALs. go2rtc's H264 depacketizer and other
    // strict clients delimit access units using this bit; without it small
    // single-packet P-frames merge into the next frame -> black WebRTC video.
    bool is_last = (nal_end >= len);
    this->send_nal_(nal, nal_len, ts, is_last);
    i = nal_end;
  }
}

void H264Packetizer::send_nal_(const uint8_t *nal, size_t len, uint32_t ts, bool is_last) {
  if (len <= RTP_MTU_PAYLOAD - 1) {
    // Marker only on the last NAL of the access unit (any NAL type).
    this->send_single_(nal, len, ts, is_last);
  } else {
    this->send_fua_(nal, len, ts, is_last);
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

void H264Packetizer::send_fua_(const uint8_t *nal, size_t len, uint32_t ts, bool is_last) {
  uint8_t fu_indicator = static_cast<uint8_t>((nal[0] & 0xe0) | NAL_TYPE_FU_A);
  uint8_t fu_header_base = static_cast<uint8_t>(nal[0] & 0x1f);
  size_t payload = len - 1;
  size_t offset = 1;
  bool first = true;
  while (payload > 0) {
    size_t chunk = payload > RTP_MTU_PAYLOAD - 2 ? RTP_MTU_PAYLOAD - 2 : payload;
    std::vector<uint8_t> pkt(RTP_HEADER_SIZE + 2 + chunk);
    bool last = (chunk == payload);
    // RTP marker only on the very last fragment of the last NAL of the frame.
    rtp_write_header(pkt.data(), last && is_last, this->payload_type_, this->seq_, ts, this->ssrc_);
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
