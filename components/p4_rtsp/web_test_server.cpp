#include "web_test_server.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <strings.h>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/md.h"

#include "jmuxer_min_js.h"
#include "web_page.h"

namespace esphome {
namespace p4_rtsp {

static const char *const TAG = "p4_rtsp.web";

static constexpr size_t MAX_VIDEO_QUEUE_FRAMES = 2;
static constexpr size_t MAX_AUDIO_QUEUE_BYTES = 64000;
static constexpr uint16_t MAX_HTTP_HEADER_BYTES = 16384;

static constexpr uint8_t WS_OP_CONT = 0x0;
static constexpr uint8_t WS_OP_TEXT = 0x1;
static constexpr uint8_t WS_OP_BINARY = 0x2;
static constexpr uint8_t WS_OP_CLOSE = 0x8;
static constexpr uint8_t WS_OP_PING = 0x9;
static constexpr uint8_t WS_OP_PONG = 0xa;

static constexpr uint8_t WS_MSG_VIDEO = 1;
static constexpr uint8_t WS_MSG_AUDIO = 2;

static const char *const WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

static std::string base64_encode(const uint8_t *data, size_t len) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= len) {
    uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    out.push_back(alphabet[(v >> 18) & 0x3f]);
    out.push_back(alphabet[(v >> 12) & 0x3f]);
    out.push_back(alphabet[(v >> 6) & 0x3f]);
    out.push_back(alphabet[v & 0x3f]);
    i += 3;
  }
  if (i + 1 == len) {
    uint32_t v = data[i] << 16;
    out.push_back(alphabet[(v >> 18) & 0x3f]);
    out.push_back(alphabet[(v >> 12) & 0x3f]);
    out.push_back('=');
    out.push_back('=');
  } else if (i + 2 == len) {
    uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
    out.push_back(alphabet[(v >> 18) & 0x3f]);
    out.push_back(alphabet[(v >> 12) & 0x3f]);
    out.push_back(alphabet[(v >> 6) & 0x3f]);
    out.push_back('=');
  }
  return out;
}

static std::string websocket_accept(const std::string &key) {
  std::string data = key + WS_GUID;
  uint8_t hash[20];
  mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1),
             reinterpret_cast<const unsigned char *>(data.data()), data.size(), hash);
  return base64_encode(hash, sizeof(hash));
}

static std::string header_value(const std::vector<std::pair<std::string, std::string>> &headers,
                                const std::string &name) {
  for (const auto &h : headers) {
    if (h.first.size() == name.size() && strcasecmp(h.first.c_str(), name.c_str()) == 0) {
      return h.second;
    }
  }
  return "";
}

WebTestServer::WebTestServer(uint16_t port, int audio_sample_rate, int audio_channels)
    : port_(port), audio_sample_rate_(audio_sample_rate), audio_channels_(audio_channels) {}

WebTestServer::~WebTestServer() { this->stop(); }

void WebTestServer::set_backchannel_callback(BackchannelCallback callback) {
  this->backchannel_callback_ = std::move(callback);
}

void WebTestServer::start() {
  if (this->running_) {
    return;
  }
  this->listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (this->listen_fd_ < 0) {
    ESP_LOGE(TAG, "socket() failed");
    return;
  }
  int opt = 1;
  setsockopt(this->listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(this->port_);
  addr.sin_addr.s_addr = INADDR_ANY;
  if (bind(this->listen_fd_, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "bind() failed on port %u", this->port_);
    close(this->listen_fd_);
    this->listen_fd_ = -1;
    return;
  }
  if (listen(this->listen_fd_, 4) < 0) {
    ESP_LOGE(TAG, "listen() failed");
    close(this->listen_fd_);
    this->listen_fd_ = -1;
    return;
  }
  this->running_ = true;
  xTaskCreatePinnedToCore(WebTestServer::accept_task, "web_accept", 4096, this, 3, nullptr, 1);
  ESP_LOGI(TAG, "Web test server listening on port %u", this->port_);
}

void WebTestServer::stop() {
  this->running_ = false;
  if (this->listen_fd_ >= 0) {
    close(this->listen_fd_);
    this->listen_fd_ = -1;
  }
  std::lock_guard<std::mutex> lock(this->sessions_mutex_);
  for (auto &session : this->sessions_) {
    session->request_stop();
  }
}

void WebTestServer::accept_task(void *param) {
  auto *server = static_cast<WebTestServer *>(param);
  server->accept_loop();
  vTaskDelete(nullptr);
}

void WebTestServer::accept_loop() {
  while (this->running_) {
    struct sockaddr_in client {};
    socklen_t len = sizeof(client);
    int fd = accept(this->listen_fd_, (struct sockaddr *) &client, &len);
    if (fd < 0) {
      continue;
    }
    std::lock_guard<std::mutex> lock(this->sessions_mutex_);
    auto session = std::make_unique<WebSession>(fd, this);
    WebSession *raw = session.get();
    this->sessions_.push_back(std::move(session));
    raw->start();
  }
}

void WebTestServer::on_session_closed(WebSession *session) {
  std::lock_guard<std::mutex> lock(this->sessions_mutex_);
  for (auto it = this->sessions_.begin(); it != this->sessions_.end(); ++it) {
    if (it->get() == session) {
      it->reset();
      this->sessions_.erase(it);
      return;
    }
  }
}

void WebTestServer::push_video_frame(const uint8_t *data, size_t len, bool keyframe, uint32_t timestamp_ms) {
  std::lock_guard<std::mutex> lock(this->sessions_mutex_);
  for (auto &session : this->sessions_) {
    session->queue_video_frame(data, len, keyframe);
  }
}

void WebTestServer::push_audio_data(const uint8_t *data, size_t len) {
  std::lock_guard<std::mutex> lock(this->sessions_mutex_);
  for (auto &session : this->sessions_) {
    session->queue_audio_data(data, len);
  }
}

bool WebTestServer::needs_streaming() const {
  std::lock_guard<std::mutex> lock(this->sessions_mutex_);
  for (const auto &session : this->sessions_) {
    if (session->wants_video() || session->wants_audio()) {
      return true;
    }
  }
  return false;
}

WebSession::WebSession(int fd, WebTestServer *server) : server_(server), fd_(fd) {}

WebSession::~WebSession() {
  if (this->fd_ >= 0) {
    close(this->fd_);
  }
}

void WebSession::start() {
  xTaskCreatePinnedToCore(WebSession::read_task, "web_read", 6144, this, 3, nullptr, 1);
  xTaskCreatePinnedToCore(WebSession::sender_task, "web_send", 8192, this, 4, nullptr, 1);
}

void WebSession::request_stop() {
  this->running_ = false;
  this->close_();
}

void WebSession::read_task(void *param) {
  auto *session = static_cast<WebSession *>(param);
  session->read_loop();
  while (!session->sender_done_) {
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  session->closed_ = true;
  session->server_->on_session_closed(session);
  vTaskDelete(nullptr);
}

void WebSession::sender_task(void *param) {
  auto *session = static_cast<WebSession *>(param);
  session->sender_loop();
  session->sender_done_ = true;
  vTaskDelete(nullptr);
}

void WebSession::queue_video_frame(const uint8_t *data, size_t len, bool keyframe) {
  if (!this->running_ || !this->want_video_) {
    return;
  }
  std::lock_guard<std::mutex> lock(this->video_queue_mutex_);
  if (this->video_queue_.size() >= MAX_VIDEO_QUEUE_FRAMES) {
    this->video_queue_.erase(this->video_queue_.begin());
  }
  WebVideoFrame frame;
  frame.data.assign(data, data + len);
  frame.keyframe = keyframe;
  this->video_queue_.push_back(std::move(frame));
}

void WebSession::queue_audio_data(const uint8_t *data, size_t len) {
  if (!this->running_ || !this->want_audio_) {
    return;
  }
  std::lock_guard<std::mutex> lock(this->audio_queue_mutex_);
  size_t used = this->audio_queue_.size() - this->audio_queue_head_;
  if (len > MAX_AUDIO_QUEUE_BYTES - used) {
    size_t drop = len - (MAX_AUDIO_QUEUE_BYTES - used);
    this->audio_queue_head_ += drop;
  }
  this->audio_queue_.insert(this->audio_queue_.end(), data, data + len);
  if (this->audio_queue_.size() > MAX_AUDIO_QUEUE_BYTES) {
    this->audio_queue_.erase(this->audio_queue_.begin(),
                             this->audio_queue_.begin() + (this->audio_queue_.size() - MAX_AUDIO_QUEUE_BYTES));
    this->audio_queue_head_ = 0;
  }
}

void WebSession::close_() {
  if (!this->running_.exchange(false)) {
    return;
  }
  this->want_video_ = false;
  this->want_audio_ = false;
  this->speaker_active_ = false;
  if (this->fd_ >= 0) {
    close(this->fd_);
    this->fd_ = -1;
  }
}

bool WebSession::send_all_(const uint8_t *data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t r = send(this->fd_, data + sent, len - sent, 0);
    if (r < 0) {
      return false;
    }
    if (r == 0) {
      return false;
    }
    sent += static_cast<size_t>(r);
  }
  return true;
}

bool WebSession::send_ws_frame_(uint8_t opcode, const uint8_t *payload, size_t len) {
  uint8_t header[10];
  size_t hlen;
  header[0] = 0x80 | opcode;
  if (len < 126) {
    header[1] = static_cast<uint8_t>(len);
    hlen = 2;
  } else if (len < 65536) {
    header[1] = 126;
    header[2] = static_cast<uint8_t>((len >> 8) & 0xff);
    header[3] = static_cast<uint8_t>(len & 0xff);
    hlen = 4;
  } else {
    header[1] = 127;
    for (int i = 0; i < 8; i++) {
      header[2 + i] = static_cast<uint8_t>((len >> (8 * (7 - i))) & 0xff);
    }
    hlen = 10;
  }
  std::lock_guard<std::mutex> lock(this->send_mutex_);
  if (!this->send_all_(header, hlen)) {
    return false;
  }
  if (len > 0 && !this->send_all_(payload, len)) {
    return false;
  }
  return true;
}

bool WebSession::send_ws_binary_(uint8_t tag, const uint8_t *data, size_t len) {
  size_t total = len + 1;
  std::vector<uint8_t> frame;
  frame.reserve(total + 10);
  frame.push_back(tag);
  frame.insert(frame.end(), data, data + len);
  uint8_t header[10];
  size_t hlen;
  header[0] = 0x80 | WS_OP_BINARY;
  if (total < 126) {
    header[1] = static_cast<uint8_t>(total);
    hlen = 2;
  } else if (total < 65536) {
    header[1] = 126;
    header[2] = static_cast<uint8_t>((total >> 8) & 0xff);
    header[3] = static_cast<uint8_t>(total & 0xff);
    hlen = 4;
  } else {
    header[1] = 127;
    for (int i = 0; i < 8; i++) {
      header[2 + i] = static_cast<uint8_t>((total >> (8 * (7 - i))) & 0xff);
    }
    hlen = 10;
  }
  std::lock_guard<std::mutex> lock(this->send_mutex_);
  if (!this->send_all_(header, hlen)) {
    return false;
  }
  if (!this->send_all_(frame.data(), frame.size())) {
    return false;
  }
  return true;
}

void WebSession::handle_ws_text_(const std::string &text) {
  if (text == "video-on") {
    this->want_video_ = true;
  } else if (text == "video-off") {
    this->want_video_ = false;
  } else if (text == "audio-on") {
    this->want_audio_ = true;
  } else if (text == "audio-off") {
    this->want_audio_ = false;
  } else if (text == "speaker-on") {
    this->speaker_active_ = true;
  } else if (text == "speaker-off") {
    this->speaker_active_ = false;
  }
  if (text == "video-on" || text == "video-off") {
    std::string reply = this->want_video_ ? "video-on" : "video-off";
    this->send_ws_frame_(WS_OP_TEXT, reinterpret_cast<const uint8_t *>(reply.data()), reply.size());
  } else if (text == "audio-on" || text == "audio-off") {
    std::string reply = this->want_audio_ ? "audio-on" : "audio-off";
    this->send_ws_frame_(WS_OP_TEXT, reinterpret_cast<const uint8_t *>(reply.data()), reply.size());
  } else if (text == "speaker-on" || text == "speaker-off") {
    std::string reply = this->speaker_active_ ? "speaker-on" : "speaker-off";
    this->send_ws_frame_(WS_OP_TEXT, reinterpret_cast<const uint8_t *>(reply.data()), reply.size());
  }
}

void WebSession::handle_ws_binary_(const std::vector<uint8_t> &payload) {
  if (!this->speaker_active_ || this->server_->backchannel_callback_ == nullptr) {
    return;
  }
  size_t samples = payload.size() / 2;
  if (samples == 0) {
    return;
  }
  std::vector<int16_t> decoded(samples);
  for (size_t i = 0; i < samples; i++) {
    uint16_t v = static_cast<uint16_t>(payload[i * 2]) | static_cast<uint16_t>(payload[i * 2 + 1] << 8);
    decoded[i] = static_cast<int16_t>(v);
  }
  this->server_->backchannel_callback_(decoded.data(), samples);
}

bool WebSession::process_ws_frames_() {
  while (true) {
    if (this->read_buffer_.size() < 2) {
      return true;
    }
    uint8_t b0 = this->read_buffer_[0];
    uint8_t b1 = this->read_buffer_[1];
    uint8_t opcode = b0 & 0x0f;
    bool masked = (b1 & 0x80) != 0;
    uint64_t len = b1 & 0x7f;
    size_t off = 2;
    if (len == 126) {
      if (this->read_buffer_.size() < 4) {
        return true;
      }
      len = (static_cast<uint64_t>(this->read_buffer_[2]) << 8) | this->read_buffer_[3];
      off = 4;
    } else if (len == 127) {
      if (this->read_buffer_.size() < 10) {
        return true;
      }
      len = 0;
      for (int i = 0; i < 8; i++) {
        len = (len << 8) | this->read_buffer_[2 + i];
      }
      off = 10;
    }
    if (!masked) {
      return false;
    }
    if (this->read_buffer_.size() < off + 4) {
      return true;
    }
    if (len > (1u << 20)) {
      return false;
    }
    if (this->read_buffer_.size() < off + 4 + len) {
      return true;
    }
    uint8_t mask[4];
    memcpy(mask, this->read_buffer_.data() + off, 4);
    off += 4;

    std::vector<uint8_t> payload(static_cast<size_t>(len));
    for (size_t i = 0; i < len; i++) {
      payload[i] = this->read_buffer_[off + i] ^ mask[i % 4];
    }
    this->read_buffer_.erase(this->read_buffer_.begin(), this->read_buffer_.begin() + off + len);

    if (opcode == WS_OP_TEXT) {
      this->handle_ws_text_(std::string(payload.begin(), payload.end()));
    } else if (opcode == WS_OP_BINARY) {
      this->handle_ws_binary_(payload);
    } else if (opcode == WS_OP_PING) {
      this->send_ws_frame_(WS_OP_PONG, payload.data(), payload.size());
    } else if (opcode == WS_OP_CLOSE) {
      this->send_ws_frame_(WS_OP_CLOSE, payload.data(), payload.size());
      return false;
    } else if (opcode == WS_OP_CONT || opcode == WS_OP_PONG) {
      // ignore
    }
  }
}

void WebSession::handle_http_() {
  auto header_end = std::search(this->read_buffer_.begin(), this->read_buffer_.end(),
                                (const uint8_t *) "\r\n\r\n", (const uint8_t *) "\r\n\r\n" + 4);
  if (header_end == this->read_buffer_.end()) {
    return;
  }
  std::string head(this->read_buffer_.begin(), header_end);

  size_t line_end = head.find("\r\n");
  std::string request_line = head.substr(0, line_end);
  size_t sp1 = request_line.find(' ');
  size_t sp2 = request_line.find(' ', sp1 + 1);
  if (sp1 == std::string::npos || sp2 == std::string::npos) {
    return;
  }
  std::string method = request_line.substr(0, sp1);
  std::string url = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
  std::string path = url.substr(0, url.find('?'));

  std::vector<std::pair<std::string, std::string>> headers;
  size_t pos = line_end + 2;
  while (pos + 2 <= head.size()) {
    size_t eol = head.find("\r\n", pos);
    if (eol == std::string::npos) {
      break;
    }
    std::string line = head.substr(pos, eol - pos);
    pos = eol + 2;
    if (line.empty()) {
      break;
    }
    size_t colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    std::string key = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    while (!value.empty() && value[0] == ' ') {
      value.erase(0, 1);
    }
    headers.emplace_back(key, value);
  }

  if (method != "GET") {
    this->send_http_response_(405, "Method Not Allowed", "text/html; charset=utf-8", nullptr, 0);
    this->running_ = false;
    return;
  }

  std::string upgrade = header_value(headers, "Upgrade");
  if (path == "/ws" && strcasecmp(upgrade.c_str(), "websocket") == 0) {
    std::string key = header_value(headers, "Sec-WebSocket-Key");
    if (key.empty()) {
      this->running_ = false;
      return;
    }
    std::string accept = websocket_accept(key);
    std::string resp = std::string("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n") +
                       "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    std::lock_guard<std::mutex> lock(this->send_mutex_);
    if (!this->send_all_(reinterpret_cast<const uint8_t *>(resp.data()), resp.size())) {
      this->running_ = false;
      return;
    }
    this->ws_upgraded_ = true;
    ESP_LOGD(TAG, "WebSocket client connected");
    return;
  }

  if (path == "/" || path == "/index.html") {
    this->send_http_response_(200, "OK", "text/html; charset=utf-8", WEB_PAGE_HTML, sizeof(WEB_PAGE_HTML) - 1);
  } else if (path == "/jmuxer.min.js") {
    this->send_http_response_(200, "OK", "text/javascript; charset=utf-8", JMUXER_MIN_JS, sizeof(JMUXER_MIN_JS) - 1);
  } else {
    this->send_http_response_(404, "Not Found", "text/html; charset=utf-8", nullptr, 0);
  }
  this->running_ = false;
}

void WebSession::send_http_response_(int code, const char *reason, const char *content_type, const char *body,
                                     size_t body_len) {
  std::string resp = "HTTP/1.1 " + to_string(code) + " " + reason + "\r\n";
  resp += "Server: ESPHome-P4-RTSP/0.1\r\n";
  if (content_type != nullptr) {
    resp += "Content-Type: " + std::string(content_type) + "\r\n";
  }
  resp += "Content-Length: " + to_string(body_len) + "\r\n";
  resp += "Cache-Control: no-cache\r\n";
  resp += "Connection: close\r\n\r\n";
  std::lock_guard<std::mutex> lock(this->send_mutex_);
  this->send_all_(reinterpret_cast<const uint8_t *>(resp.data()), resp.size());
  if (body != nullptr && body_len > 0) {
    this->send_all_(reinterpret_cast<const uint8_t *>(body), body_len);
  }
}

void WebSession::read_loop() {
  struct timeval tv;
  tv.tv_sec = 1;
  tv.tv_usec = 0;
  setsockopt(this->fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(this->fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  uint8_t recvbuf[2048];
  while (this->running_) {
    ssize_t r = recv(this->fd_, recvbuf, sizeof(recvbuf), 0);
    if (r < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      break;
    }
    if (r == 0) {
      break;
    }
    this->read_buffer_.insert(this->read_buffer_.end(), recvbuf, recvbuf + r);

    if (!this->ws_upgraded_) {
      auto header_end = std::search(this->read_buffer_.begin(), this->read_buffer_.end(),
                                    (const uint8_t *) "\r\n\r\n", (const uint8_t *) "\r\n\r\n" + 4);
      if (header_end == this->read_buffer_.end()) {
        if (this->read_buffer_.size() > MAX_HTTP_HEADER_BYTES) {
          break;
        }
        continue;
      }
      this->handle_http_();
      if (!this->ws_upgraded_) {
        break;
      }
      this->read_buffer_.erase(this->read_buffer_.begin(),
                               this->read_buffer_.begin() + (header_end - this->read_buffer_.begin() + 4));
      continue;
    }

    if (!this->process_ws_frames_()) {
      break;
    }
  }
  this->close_();
}

void WebSession::sender_loop() {
  std::vector<uint8_t> audio_chunk;
  while (this->running_) {
    bool did_something = false;
    WebVideoFrame frame;
    bool have_video = false;
    {
      std::lock_guard<std::mutex> lock(this->video_queue_mutex_);
      if (!this->video_queue_.empty()) {
        frame = std::move(this->video_queue_.front());
        this->video_queue_.erase(this->video_queue_.begin());
        have_video = true;
      }
    }
    if (have_video) {
      if (!this->send_ws_binary_(WS_MSG_VIDEO, frame.data.data(), frame.data.size())) {
        this->close_();
        break;
      }
      did_something = true;
    }
    {
      std::lock_guard<std::mutex> lock(this->audio_queue_mutex_);
      size_t avail = this->audio_queue_.size() - this->audio_queue_head_;
      if (avail > 0) {
        audio_chunk.assign(this->audio_queue_.begin() + this->audio_queue_head_,
                           this->audio_queue_.begin() + this->audio_queue_head_ + avail);
        this->audio_queue_head_ += avail;
        if (this->audio_queue_head_ == this->audio_queue_.size()) {
          this->audio_queue_.clear();
          this->audio_queue_head_ = 0;
        }
        did_something = true;
      }
    }
    if (!audio_chunk.empty()) {
      if (!this->send_ws_binary_(WS_MSG_AUDIO, audio_chunk.data(), audio_chunk.size())) {
        this->close_();
        break;
      }
      audio_chunk.clear();
    }
    if (!did_something) {
      vTaskDelay(5);
    }
  }
}

}  // namespace p4_rtsp
}  // namespace esphome
