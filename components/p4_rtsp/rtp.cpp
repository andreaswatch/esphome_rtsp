#include "rtp.h"

#include <cstring>

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "esp_audio_enc.h"
#include "esp_opus_enc.h"

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

// ---------------------------------------------------------------------------
// G.711 A-law (PCMA) packetizer
// ---------------------------------------------------------------------------

// Lookup table derived from ffmpeg's linear2alaw (bit-exact). Indexed by the
// lower boundary (16-bit PCM) of each of the 128 positive A-law intervals.
// Binary search returns the A-law code word for a given magnitude.
static const uint16_t ALAW_LO[128] = {
    0, 16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176,
    192, 208, 224, 240, 256, 272, 288, 304, 320, 336, 352, 368,
    384, 400, 416, 432, 448, 464, 480, 496, 516, 544, 576, 608,
    640, 672, 704, 736, 768, 800, 832, 864, 896, 928, 960, 992,
    1032, 1088, 1152, 1216, 1280, 1344, 1408, 1472, 1536, 1600, 1664, 1728,
    1792, 1856, 1920, 1984, 2064, 2176, 2304, 2432, 2560, 2688, 2816, 2944,
    3072, 3200, 3328, 3456, 3584, 3712, 3840, 3968, 4128, 4352, 4608, 4864,
    5120, 5376, 5632, 5888, 6144, 6400, 6656, 6912, 7168, 7424, 7680, 7936,
    8256, 8704, 9216, 9728, 10240, 10752, 11264, 11776, 12288, 12800, 13312, 13824,
    14336, 14848, 15360, 15872, 16512, 17408, 18432, 19456, 20480, 21504, 22528, 23552,
    24576, 25600, 26624, 27648, 28672, 29696, 30720, 31744,
};

static const uint8_t ALAW_CODE[128] = {
    0xd5, 0xd4, 0xd7, 0xd6, 0xd1, 0xd0, 0xd3, 0xd2, 0xdd, 0xdc, 0xdf, 0xde,
    0xd9, 0xd8, 0xdb, 0xda, 0xc5, 0xc4, 0xc7, 0xc6, 0xc1, 0xc0, 0xc3, 0xc2,
    0xcd, 0xcc, 0xcf, 0xce, 0xc9, 0xc8, 0xcb, 0xca, 0xf5, 0xf4, 0xf7, 0xf6,
    0xf1, 0xf0, 0xf3, 0xf2, 0xfd, 0xfc, 0xff, 0xfe, 0xf9, 0xf8, 0xfb, 0xfa,
    0xe5, 0xe4, 0xe7, 0xe6, 0xe1, 0xe0, 0xe3, 0xe2, 0xed, 0xec, 0xef, 0xee,
    0xe9, 0xe8, 0xeb, 0xea, 0x95, 0x94, 0x97, 0x96, 0x91, 0x90, 0x93, 0x92,
    0x9d, 0x9c, 0x9f, 0x9e, 0x99, 0x98, 0x9b, 0x9a, 0x85, 0x84, 0x87, 0x86,
    0x81, 0x80, 0x83, 0x82, 0x8d, 0x8c, 0x8f, 0x8e, 0x89, 0x88, 0x8b, 0x8a,
    0xb5, 0xb4, 0xb7, 0xb6, 0xb1, 0xb0, 0xb3, 0xb2, 0xbd, 0xbc, 0xbf, 0xbe,
    0xb9, 0xb8, 0xbb, 0xba, 0xa5, 0xa4, 0xa7, 0xa6, 0xa1, 0xa0, 0xa3, 0xa2,
    0xad, 0xac, 0xaf, 0xae, 0xa9, 0xa8, 0xab, 0xaa,
};

static inline uint8_t alaw_encode(int16_t pcm) {
  int sign = (pcm >> 8) & 0x80;
  int v = sign ? -pcm : pcm;
  if (v > 32767) {
    v = 32767;
  }
  // Binary search over ALAW_LO (monotonically increasing).
  int lo = 0, hi = 127;
  while (lo < hi) {
    int mid = (lo + hi + 1) >> 1;
    if (ALAW_LO[mid] <= v) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  uint8_t code = ALAW_CODE[lo];
  return sign ? (code & 0x7f) : code;
}

PCMAPacketizer::PCMAPacketizer() { this->ssrc_ = random_uint32(); }

void PCMAPacketizer::set_send_callback(RtpSendCallback callback) {
  this->send_callback_ = std::move(callback);
}

void PCMAPacketizer::set_ssrc(uint32_t ssrc) { this->ssrc_ = ssrc; }

void PCMAPacketizer::set_payload_type(uint8_t pt) { this->payload_type_ = pt; }

void PCMAPacketizer::set_input_sample_rate(int rate) {
  this->input_sample_rate_ = rate;
}

void PCMAPacketizer::set_channels(int channels) { this->channels_ = channels; }

void PCMAPacketizer::push_pcm16(const uint8_t *data, size_t len) {
  // Downsample by taking every (input_rate/8000)th sample (assumes input rate
  // is an even multiple of 8000, e.g. 16000 -> every 2nd sample).
  const int decim = this->input_sample_rate_ / 8000;
  const size_t frame_bytes = static_cast<size_t>(decim * 2);  // one 8k frame in bytes
  const size_t packets = 20;  // 20 ms of audio per RTP packet
  const size_t samples_per_packet = 8000 * packets / 1000;  // 160 samples @8kHz

  std::vector<uint8_t> pkt;
  size_t in_off = 0;
  size_t out_samples = 0;
  pkt.reserve(RTP_HEADER_SIZE + samples_per_packet);

  while (in_off + frame_bytes <= len) {
    if (out_samples == 0) {
      pkt.assign(RTP_HEADER_SIZE, 0);
      rtp_write_header(pkt.data(), false, this->payload_type_, this->seq_,
                       this->timestamp_, this->ssrc_);
    }
    int16_t sample;
    sample = (int16_t)(data[in_off] | (data[in_off + 1] << 8));
    pkt.push_back(alaw_encode(sample));
    in_off += frame_bytes;
    out_samples++;
    if (out_samples >= samples_per_packet) {
      pkt[RTP_HEADER_SIZE - 1] |= 0x80;  // marker
      this->seq_++;
      this->timestamp_ += static_cast<uint32_t>(samples_per_packet);
      if (this->send_callback_) {
        this->send_callback_(pkt.data(), pkt.size());
      }
      out_samples = 0;
    }
  }
  // Flush any remaining partial packet.
  if (out_samples > 0) {
    pkt[RTP_HEADER_SIZE - 1] |= 0x80;
    this->seq_++;
    this->timestamp_ += static_cast<uint32_t>(out_samples);
    if (this->send_callback_) {
      this->send_callback_(pkt.data(), pkt.size());
    }
  }
}

// ---------------------------------------------------------------------------
// Opus (RFC 7587) packetizer
// ---------------------------------------------------------------------------

OpusPacketizer::OpusPacketizer() { this->ssrc_ = random_uint32(); }

OpusPacketizer::~OpusPacketizer() {
  if (this->enc_ != nullptr) {
    esp_opus_enc_close(this->enc_);
    this->enc_ = nullptr;
  }
}

void OpusPacketizer::set_send_callback(RtpSendCallback callback) {
  this->send_callback_ = std::move(callback);
}

void OpusPacketizer::set_ssrc(uint32_t ssrc) { this->ssrc_ = ssrc; }

void OpusPacketizer::set_payload_type(uint8_t pt) { this->payload_type_ = pt; }

void OpusPacketizer::set_input_sample_rate(int rate) {
  this->input_sample_rate_ = rate;
}

void OpusPacketizer::set_channels(int channels) { this->channels_ = channels; }

bool OpusPacketizer::init_encoder_() {
  if (this->enc_ != nullptr) {
    return true;
  }
  esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
  cfg.sample_rate = this->input_sample_rate_;
  cfg.channel = this->channels_;
  cfg.bits_per_sample = ESP_AUDIO_BIT16;
  cfg.bitrate = 32000;
  cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
  cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
  cfg.complexity = 2;
  cfg.enable_fec = true;
  cfg.enable_dtx = false;
  cfg.enable_vbr = false;

  esp_audio_err_t err = esp_opus_enc_open(&cfg, sizeof(cfg), &this->enc_);
  if (err != ESP_AUDIO_ERR_OK || this->enc_ == nullptr) {
    ESP_LOGE(TAG, "Opus encoder open failed: %d", err);
    this->enc_ = nullptr;
    return false;
  }
  int in_size = 0;
  int out_size = 0;
  if (esp_opus_enc_get_frame_size(this->enc_, &in_size, &out_size) !=
      ESP_AUDIO_ERR_OK) {
    esp_opus_enc_close(this->enc_);
    this->enc_ = nullptr;
    return false;
  }
  this->frame_bytes_ = static_cast<size_t>(in_size);
  this->enc_out_buf_.resize(static_cast<size_t>(out_size));
  ESP_LOGI(TAG, "Opus encoder ready (frame_in=%d frame_out=%d)", in_size,
           out_size);
  return true;
}

void OpusPacketizer::push_pcm16(const uint8_t *data, size_t len) {
  if (!this->init_encoder_()) {
    return;
  }
  this->pcm_buf_.insert(this->pcm_buf_.end(), data, data + len);
  while (this->pcm_buf_.size() >= this->frame_bytes_) {
    esp_audio_enc_in_frame_t in_frame;
    in_frame.buffer = this->pcm_buf_.data();
    in_frame.len = static_cast<uint32_t>(this->frame_bytes_);
    esp_audio_enc_out_frame_t out_frame;
    out_frame.buffer = this->enc_out_buf_.data();
    out_frame.len = static_cast<uint32_t>(this->enc_out_buf_.size());
    esp_audio_err_t err =
        esp_opus_enc_process(this->enc_, &in_frame, &out_frame);
    this->pcm_buf_.erase(this->pcm_buf_.begin(),
                         this->pcm_buf_.begin() + this->frame_bytes_);
    if (err != ESP_AUDIO_ERR_OK || out_frame.encoded_bytes == 0) {
      ESP_LOGW(TAG, "Opus encode failed: %d", err);
      continue;
    }
    std::vector<uint8_t> pkt(RTP_HEADER_SIZE + out_frame.encoded_bytes);
    rtp_write_header(pkt.data(), true, this->payload_type_, this->seq_,
                     this->timestamp_, this->ssrc_);
    memcpy(pkt.data() + RTP_HEADER_SIZE, this->enc_out_buf_.data(),
           out_frame.encoded_bytes);
    this->seq_++;
    this->timestamp_ += timestamp_increment_;
    if (this->send_callback_) {
      this->send_callback_(pkt.data(), pkt.size());
    }
  }
}

}  // namespace p4_rtsp
}  // namespace esphome
