#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "rtp.h"
#include "spiram_allocator.h"

namespace esphome {
namespace p4_rtsp {

class RtspServer;
class RtspSession;

using BackchannelCallback = std::function<void(const int16_t *data, size_t samples)>;

struct SessionVideoFrame {
  // Keep frame copies in PSRAM — the internal heap is too small for full H264
  // frames once the camera's PSRAM buffers are allocated.
  std::vector<uint8_t, SpiramAllocator<uint8_t>> data;
  uint32_t timestamp_ms{0};
  bool keyframe{false};
};

class RtspServer {
 public:
  RtspServer(uint16_t port, int audio_sample_rate, int audio_channels);
  ~RtspServer();

  void start();
  void stop();

  void set_video_info(int width, int height, int fps);
  void set_backchannel_callback(BackchannelCallback callback);

  void push_video_frame(const uint8_t *data, size_t len, bool keyframe, uint32_t timestamp_ms);
  void push_audio_data(const uint8_t *data, size_t len);

  bool has_clients() const;
  int active_video_sessions() const;

  int audio_sample_rate() const { return this->audio_sample_rate_; }
  int audio_channels() const { return this->audio_channels_; }
  int video_width() const { return this->video_width_; }
  int video_height() const { return this->video_height_; }
  int video_fps() const { return this->video_fps_; }
  const BackchannelCallback &backchannel_callback() const { return this->backchannel_callback_; }
  const std::vector<uint8_t> &cached_sps() const { return this->sps_pps_cache_.sps(); }
  const std::vector<uint8_t> &cached_pps() const { return this->sps_pps_cache_.pps(); }

 protected:
  void accept_loop();
  static void accept_task(void *param);
  void on_session_closed(RtspSession *session);

  uint16_t port_;
  int audio_sample_rate_;
  int audio_channels_;
  int video_width_{0};
  int video_height_{0};
  int video_fps_{0};
  BackchannelCallback backchannel_callback_;

  int listen_fd_{-1};
  std::atomic<bool> running_{false};
  mutable std::mutex sessions_mutex_;
  std::vector<std::unique_ptr<RtspSession>> sessions_;
  H264Packetizer sps_pps_cache_;  // no send_callback — used only to extract SPS/PPS
  mutable std::mutex sps_pps_mutex_;
  std::atomic<int> active_video_count_{0};

  friend class RtspSession;
};

enum class TrackId : uint8_t {
  NONE = 0,
  VIDEO = 1,
  AUDIO = 2,
};

enum class TransportKind : uint8_t {
  NONE = 0,
  UDP = 1,
  INTERLEAVED = 2,
};

struct TrackState {
  bool setup{false};
  bool active{false};
  TransportKind transport{TransportKind::NONE};
  uint32_t client_ip{0};
  uint16_t client_rtp_port{0};
  uint16_t client_rtcp_port{0};
  int rtp_socket{-1};
  uint16_t server_rtp_port{0};
  int interleaved_channel{-1};
};

class RtspSession {
 public:
  RtspSession(int fd, RtspServer *server);
  ~RtspSession();

  void start();
  void request_stop();

  void queue_video_frame(const uint8_t *data, size_t len, bool keyframe, uint32_t timestamp_ms);
  void queue_audio_data(const uint8_t *data, size_t len);

  bool playing() const { return this->playing_; }
  bool video_active() const;
  bool closed() const { return this->closed_; }

 protected:
  void control_loop();
  void sender_loop();
  static void control_task(void *param);
  static void sender_task(void *param);

  void handle_client_();
  void handle_interleaved_(const uint8_t *header, size_t header_len);
  int16_t mulaw_decode(uint8_t u) const;
  int16_t alaw_decode(uint8_t a) const;
  void process_request_(const char *request, size_t len);
  void handle_request_(const std::string &method, const std::string &url,
                       const std::vector<std::pair<std::string, std::string>> &headers);
  void send_response_(int code, const char *reason, const char *extra_headers, const char *body,
                      size_t body_len);
  std::string build_sdp_(bool backchannel) const;
  std::string build_transport_header_(const TrackState &track) const;
  void send_track_packet_(const TrackState &track, const uint8_t *rtp, size_t len);

  RtspServer *server_;
  int fd_{-1};
  uint32_t session_id_{0};
  std::atomic<bool> running_{true};
  std::atomic<bool> closed_{false};
  std::atomic<bool> sender_done_{false};
  bool playing_{false};
  bool teardown_requested_{false};

  TrackState video_track_;
  TrackState audio_track_;
  TrackId pending_track_{TrackId::NONE};

  std::mutex video_queue_mutex_;
  std::vector<SessionVideoFrame> video_queue_;
  std::mutex audio_queue_mutex_;
  std::vector<uint8_t> audio_queue_;
  size_t audio_queue_head_{0};

  H264Packetizer h264_;
  OpusPacketizer opus_;
  uint32_t video_timestamp_base_{0};
  uint32_t audio_timestamp_base_{0};
  uint32_t video_frames_sent_{0};

  std::mutex send_mutex_;
  std::vector<uint8_t> request_buffer_;

  bool audio_channel_set_{false};
  int audio_interleaved_channel_{-1};
};

}  // namespace p4_rtsp
}  // namespace esphome
