#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#ifdef USE_ESP32
#include "esp_heap_caps.h"
#endif

namespace esphome {
namespace p4_rtsp {

class WebTestServer;
class WebSession;

using BackchannelCallback = std::function<void(const int16_t *data, size_t samples)>;

template<class T> class SpiramAllocator {
 public:
  using value_type = T;

  constexpr SpiramAllocator() noexcept = default;
  template<class U> constexpr SpiramAllocator(const SpiramAllocator<U> &) noexcept {}

  T *allocate(std::size_t n) {
    if (n == 0) {
      return nullptr;
    }
    size_t size = n * sizeof(T);
#ifdef USE_ESP32
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == nullptr) {
      ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
#else
    void *ptr = malloc(size);
#endif
    return static_cast<T *>(ptr);
  }

  void deallocate(T *ptr, std::size_t) noexcept {
#ifdef USE_ESP32
    heap_caps_free(ptr);
#else
    free(ptr);
#endif
  }
};

template<class T, class U>
constexpr bool operator==(const SpiramAllocator<T> &, const SpiramAllocator<U> &) noexcept {
  return true;
}
template<class T, class U>
constexpr bool operator!=(const SpiramAllocator<T> &, const SpiramAllocator<U> &) noexcept {
  return false;
}

struct WebVideoFrame {
  std::vector<uint8_t, SpiramAllocator<uint8_t>> data;
  bool keyframe{false};
};

class WebTestServer {
 public:
  WebTestServer(uint16_t port, int audio_sample_rate, int audio_channels);
  ~WebTestServer();

  void start();
  void stop();

  void set_backchannel_callback(BackchannelCallback callback);

  void push_video_frame(const uint8_t *data, size_t len, bool keyframe, uint32_t timestamp_ms);
  void push_audio_data(const uint8_t *data, size_t len);

  bool needs_streaming() const;

  int audio_sample_rate() const { return this->audio_sample_rate_; }
  int audio_channels() const { return this->audio_channels_; }
  const BackchannelCallback &backchannel_callback() const { return this->backchannel_callback_; }

 protected:
  void accept_loop();
  static void accept_task(void *param);
  void on_session_closed(WebSession *session);

  uint16_t port_;
  int audio_sample_rate_;
  int audio_channels_;
  BackchannelCallback backchannel_callback_;

  int listen_fd_{-1};
  std::atomic<bool> running_{false};
  mutable std::mutex sessions_mutex_;
  std::vector<std::unique_ptr<WebSession>> sessions_;

  friend class WebSession;
};

class WebSession {
 public:
  WebSession(int fd, WebTestServer *server);
  ~WebSession();

  void start();
  void request_stop();

  void queue_video_frame(const uint8_t *data, size_t len, bool keyframe);
  void queue_audio_data(const uint8_t *data, size_t len);

  bool wants_video() const { return this->want_video_; }
  bool wants_audio() const { return this->want_audio_; }

 protected:
  void read_loop();
  void sender_loop();
  static void read_task(void *param);
  static void sender_task(void *param);

  void handle_http_();
  bool process_ws_frames_();
  void handle_ws_text_(const std::string &text);
  void handle_ws_binary_(const std::vector<uint8_t> &payload);
  void send_http_response_(int code, const char *reason, const char *content_type, const char *body,
                           size_t body_len);
  bool send_all_(const uint8_t *data, size_t len);
  bool send_ws_frame_(uint8_t opcode, const uint8_t *payload, size_t len);
  bool send_ws_binary_(uint8_t tag, const uint8_t *data, size_t len);
  void close_();

  WebTestServer *server_;
  int fd_{-1};
  std::atomic<bool> running_{true};
  std::atomic<bool> closed_{false};
  std::atomic<bool> sender_done_{false};
  bool ws_upgraded_{false};
  bool want_video_{false};
  bool want_audio_{false};
  bool speaker_active_{false};

  std::mutex video_queue_mutex_;
  std::vector<WebVideoFrame> video_queue_;
  std::mutex audio_queue_mutex_;
  std::vector<uint8_t, SpiramAllocator<uint8_t>> audio_queue_;
  size_t audio_queue_head_{0};

  std::mutex send_mutex_;
  std::vector<uint8_t> read_buffer_;
};

}  // namespace p4_rtsp
}  // namespace esphome
