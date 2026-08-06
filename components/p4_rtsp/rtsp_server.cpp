#include "rtsp_server.h"

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

namespace esphome {
namespace p4_rtsp {

static const char *const TAG = "p4_rtsp.server";

static constexpr uint16_t MAX_AUDIO_QUEUE_BYTES = 64000;
static constexpr size_t MAX_VIDEO_QUEUE_FRAMES = 2;
static constexpr int RTSP_TIMEOUT_MS = 60000;

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

RtspServer::RtspServer(uint16_t port, int audio_sample_rate, int audio_channels)
    : port_(port), audio_sample_rate_(audio_sample_rate), audio_channels_(audio_channels) {}

RtspServer::~RtspServer() { this->stop(); }

void RtspServer::set_video_info(int width, int height, int fps) {
  this->video_width_ = width;
  this->video_height_ = height;
  this->video_fps_ = fps;
}

void RtspServer::set_backchannel_callback(BackchannelCallback callback) {
  this->backchannel_callback_ = std::move(callback);
}

void RtspServer::start() {
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
  struct timeval tv;
  tv.tv_sec = 1;
  tv.tv_usec = 0;
  setsockopt(this->listen_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

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
  xTaskCreatePinnedToCore(RtspServer::accept_task, "rtsp_accept", 4096, this, 3, nullptr, 1);
  ESP_LOGI(TAG, "RTSP listening on port %u", this->port_);
}

void RtspServer::stop() {
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

void RtspServer::accept_task(void *param) {
  auto *server = static_cast<RtspServer *>(param);
  server->accept_loop();
  vTaskDelete(nullptr);
}

void RtspServer::accept_loop() {
  while (this->running_) {
    struct sockaddr_in client {};
    socklen_t len = sizeof(client);
    int fd = accept(this->listen_fd_, (struct sockaddr *) &client, &len);
    if (fd < 0) {
      continue;
    }
    std::lock_guard<std::mutex> lock(this->sessions_mutex_);
    auto session = std::make_unique<RtspSession>(fd, this);
    RtspSession *raw = session.get();
    this->sessions_.push_back(std::move(session));
    raw->start();
  }
}

void RtspServer::on_session_closed(RtspSession *session) {
  std::lock_guard<std::mutex> lock(this->sessions_mutex_);
  for (auto it = this->sessions_.begin(); it != this->sessions_.end(); ++it) {
    if (it->get() == session) {
      it->reset();
      this->sessions_.erase(it);
      return;
    }
  }
}

void RtspServer::push_video_frame(const uint8_t *data, size_t len, bool keyframe, uint32_t timestamp_ms) {
  std::lock_guard<std::mutex> lock(this->sessions_mutex_);
  for (auto &session : this->sessions_) {
    session->queue_video_frame(data, len, keyframe, timestamp_ms);
  }
}

void RtspServer::push_audio_data(const uint8_t *data, size_t len) {
  std::lock_guard<std::mutex> lock(this->sessions_mutex_);
  for (auto &session : this->sessions_) {
    session->queue_audio_data(data, len);
  }
}

bool RtspServer::has_clients() const {
  std::lock_guard<std::mutex> lock(this->sessions_mutex_);
  for (const auto &session : this->sessions_) {
    if (session->playing()) {
      return true;
    }
  }
  return false;
}

int RtspServer::active_video_sessions() const {
  std::lock_guard<std::mutex> lock(this->sessions_mutex_);
  int count = 0;
  for (const auto &session : this->sessions_) {
    if (session->video_active()) {
      count++;
    }
  }
  return count;
}

RtspSession::RtspSession(int fd, RtspServer *server) : server_(server), fd_(fd) {
  this->session_id_ = random_uint32();
  this->h264_.set_ssrc(random_uint32());
  this->h264_.set_payload_type(RTP_PT_H264);
  this->h264_.set_send_callback([this](const uint8_t *data, size_t len) {
    this->send_track_packet_(this->video_track_, data, len);
  });
  this->l16_.set_ssrc(random_uint32());
  this->l16_.set_payload_type(RTP_PT_L16);
  this->l16_.set_sample_rate(this->server_->audio_sample_rate());
  this->l16_.set_channels(this->server_->audio_channels());
  this->l16_.set_send_callback([this](const uint8_t *data, size_t len) {
    this->send_track_packet_(this->audio_track_, data, len);
  });
}

RtspSession::~RtspSession() {
  if (this->video_track_.rtp_socket >= 0) {
    close(this->video_track_.rtp_socket);
  }
  if (this->audio_track_.rtp_socket >= 0) {
    close(this->audio_track_.rtp_socket);
  }
  if (this->fd_ >= 0) {
    close(this->fd_);
  }
}

void RtspSession::start() {
  xTaskCreatePinnedToCore(RtspSession::control_task, "rtsp_ctrl", 6144, this, 3, nullptr, 1);
  xTaskCreatePinnedToCore(RtspSession::sender_task, "rtsp_send", 8192, this, 4, nullptr, 1);
}

void RtspSession::request_stop() {
  this->running_ = false;
  this->teardown_requested_ = true;
}

void RtspSession::control_task(void *param) {
  auto *session = static_cast<RtspSession *>(param);
  session->control_loop();
  session->closed_ = true;
  session->server_->on_session_closed(session);
  vTaskDelete(nullptr);
}

void RtspSession::sender_task(void *param) {
  auto *session = static_cast<RtspSession *>(param);
  session->sender_loop();
  vTaskDelete(nullptr);
}

bool RtspSession::video_active() const {
  return this->playing_ && this->video_track_.setup && this->video_track_.transport != TransportKind::NONE;
}

void RtspSession::queue_video_frame(const uint8_t *data, size_t len, bool keyframe, uint32_t timestamp_ms) {
  if (!this->playing_ || !this->video_active()) {
    return;
  }
  std::lock_guard<std::mutex> lock(this->video_queue_mutex_);
  if (this->video_queue_.size() >= MAX_VIDEO_QUEUE_FRAMES) {
    this->video_queue_.erase(this->video_queue_.begin());
  }
  SessionVideoFrame frame;
  frame.data.assign(data, data + len);
  frame.keyframe = keyframe;
  frame.timestamp_ms = timestamp_ms;
  this->video_queue_.push_back(std::move(frame));
}

void RtspSession::queue_audio_data(const uint8_t *data, size_t len) {
  if (!this->playing_ || !this->audio_track_.setup) {
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

void RtspSession::sender_loop() {
  std::vector<uint8_t> audio_chunk;
  while (this->running_) {
    if (!this->playing_) {
      vTaskDelay(10);
      continue;
    }
    bool did_something = false;
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
      if (this->audio_track_.transport != TransportKind::NONE) {
        this->l16_.push_bytes(audio_chunk.data(), audio_chunk.size());
      }
      audio_chunk.clear();
    }
    SessionVideoFrame frame;
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
      this->h264_.push_annexb(frame.data.data(), frame.data.size(), frame.timestamp_ms);
      this->video_frames_sent_++;
      did_something = true;
    }
    if (!did_something) {
      vTaskDelay(5);
    }
  }
}

void RtspSession::control_loop() {
  struct timeval tv;
  tv.tv_sec = 1;
  tv.tv_usec = 0;
  setsockopt(this->fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

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
    this->request_buffer_.insert(this->request_buffer_.end(), recvbuf, recvbuf + r);
    this->handle_client_();
  }
  this->running_ = false;
  this->closed_ = true;
}

void RtspSession::handle_client_() {
  while (!this->request_buffer_.empty()) {
    if (this->request_buffer_[0] == '$') {
      if (this->request_buffer_.size() < 4) {
        return;
      }
      uint16_t plen = static_cast<uint16_t>((this->request_buffer_[2] << 8) | this->request_buffer_[3]);
      if (this->request_buffer_.size() < 4 + plen) {
        return;
      }
      this->handle_interleaved_(this->request_buffer_.data(), this->request_buffer_.size());
      this->request_buffer_.erase(this->request_buffer_.begin(), this->request_buffer_.begin() + 4 + plen);
      continue;
    }
    auto it = std::search(this->request_buffer_.begin(), this->request_buffer_.end(),
                          (const uint8_t *) "\r\n\r\n", (const uint8_t *) "\r\n\r\n" + 4);
    if (it == this->request_buffer_.end()) {
      return;
    }
    size_t req_len = it - this->request_buffer_.begin() + 4;
    std::string request(this->request_buffer_.begin(), this->request_buffer_.begin() + req_len);
    this->request_buffer_.erase(this->request_buffer_.begin(), this->request_buffer_.begin() + req_len);
    this->process_request_(request.c_str(), request.size());
  }
}

void RtspSession::handle_interleaved_(const uint8_t *header, size_t header_len) {
  if (header_len < 4) {
    return;
  }
  int channel = header[1];
  uint16_t plen = static_cast<uint16_t>((header[2] << 8) | header[3]);
  if (plen < RTP_HEADER_SIZE) {
    return;
  }
  const uint8_t *rtp = header + 4;
  uint8_t pt = rtp[1] & 0x7f;
  if (channel == this->audio_interleaved_channel_ && pt == RTP_PT_L16) {
    size_t payload_len = plen - RTP_HEADER_SIZE;
    size_t samples = payload_len / 2;
    std::vector<int16_t> decoded(samples);
    for (size_t i = 0; i < samples; i++) {
      uint16_t v = static_cast<uint16_t>((rtp[RTP_HEADER_SIZE + i * 2] << 8) |
                                         rtp[RTP_HEADER_SIZE + i * 2 + 1]);
      decoded[i] = static_cast<int16_t>(v);
    }
    if (this->server_->backchannel_callback_) {
      this->server_->backchannel_callback_(decoded.data(), samples);
    }
  }
}

void RtspSession::process_request_(const char *request, size_t len) {
  std::string req(request, len);
  size_t line_end = req.find("\r\n");
  if (line_end == std::string::npos) {
    return;
  }
  std::string request_line = req.substr(0, line_end);
  size_t sp1 = request_line.find(' ');
  size_t sp2 = request_line.find(' ', sp1 + 1);
  if (sp1 == std::string::npos || sp2 == std::string::npos) {
    return;
  }
  std::string method = request_line.substr(0, sp1);
  std::string url = request_line.substr(sp1 + 1, sp2 - sp1 - 1);

  std::vector<std::pair<std::string, std::string>> headers;
  size_t pos = line_end + 2;
  while (pos + 2 <= req.size()) {
    size_t eol = req.find("\r\n", pos);
    if (eol == std::string::npos) {
      break;
    }
    std::string line = req.substr(pos, eol - pos);
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
  this->handle_request_(method, url, headers);
}

static std::string header_value(const std::vector<std::pair<std::string, std::string>> &headers,
                                const std::string &name) {
  for (const auto &h : headers) {
    if (h.first.size() == name.size() &&
        strcasecmp(h.first.c_str(), name.c_str()) == 0) {
      return h.second;
    }
  }
  return "";
}

void RtspSession::handle_request_(const std::string &method, const std::string &url,
                                  const std::vector<std::pair<std::string, std::string>> &headers) {
  std::string cseq = header_value(headers, "CSeq");
  if (cseq.empty()) {
    return;
  }
  std::string extra_headers = "CSeq: " + cseq + "\r\n";

  if (method == "OPTIONS") {
    const char *publics = "OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER, SET_PARAMETER";
    this->send_response_(200, "OK", ("CSeq: " + cseq + "\r\nPublic: " + publics + "\r\n").c_str(), nullptr, 0);
    return;
  }
  if (method == "DESCRIBE") {
    std::string body = this->build_sdp_();
    std::string content = "CSeq: " + cseq + "\r\n" + "Content-Type: application/sdp\r\n" +
                          "Content-Base: rtsp://" + url.substr(0, url.find_last_of('/') + 1) + "\r\n" +
                          "Content-Length: " + to_string(body.size()) + "\r\n";
    this->send_response_(200, "OK", content.c_str(), body.c_str(), body.size());
    return;
  }
  if (method == "SETUP") {
    std::string transport = header_value(headers, "Transport");
    TrackId track = TrackId::NONE;
    if (url.find("trackID=1") != std::string::npos || url.find("trackID1") != std::string::npos) {
      track = TrackId::AUDIO;
    } else if (url.find("trackID=0") != std::string::npos || url.find("trackID0") != std::string::npos ||
               url.find("/0") != std::string::npos) {
      track = TrackId::VIDEO;
    } else if (this->pending_track_ == TrackId::NONE) {
      track = TrackId::VIDEO;
    } else if (this->pending_track_ == TrackId::VIDEO) {
      track = TrackId::AUDIO;
    } else {
      track = this->pending_track_;
    }

    TrackState *ts = nullptr;
    if (track == TrackId::VIDEO && this->server_->video_width_ > 0) {
      ts = &this->video_track_;
    } else if (track == TrackId::AUDIO) {
      ts = &this->audio_track_;
    }
    if (ts == nullptr) {
      this->send_response_(404, "Not Found", extra_headers.c_str(), nullptr, 0);
      return;
    }

    ts->transport = TransportKind::NONE;
    if (transport.find("interleaved") != std::string::npos) {
      ts->transport = TransportKind::INTERLEAVED;
      size_t eq = transport.find("interleaved=");
      if (eq != std::string::npos) {
        int ch = atoi(transport.c_str() + eq + 12);
        ts->interleaved_channel = ch;
        if (track == TrackId::AUDIO) {
          this->audio_interleaved_channel_ = ch;
          this->audio_channel_set_ = true;
        }
      }
    } else {
      ts->transport = TransportKind::UDP;
      size_t cp = transport.find("client_port=");
      if (cp != std::string::npos) {
        ts->client_rtp_port = static_cast<uint16_t>(atoi(transport.c_str() + cp + 12));
        size_t dash = transport.find('-', cp + 12);
        ts->client_rtcp_port =
            dash != std::string::npos ? static_cast<uint16_t>(atoi(transport.c_str() + dash + 1)) : 0;
      }
      struct sockaddr_in peer {};
      socklen_t plen = sizeof(peer);
      getpeername(this->fd_, (struct sockaddr *) &peer, &plen);
      ts->client_ip = peer.sin_addr.s_addr;

      ts->rtp_socket = socket(AF_INET, SOCK_DGRAM, 0);
      if (ts->rtp_socket >= 0) {
        struct sockaddr_in local {};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = INADDR_ANY;
        local.sin_port = 0;
        bind(ts->rtp_socket, (struct sockaddr *) &local, sizeof(local));
        socklen_t slen = sizeof(local);
        getsockname(ts->rtp_socket, (struct sockaddr *) &local, &slen);
        ts->server_rtp_port = ntohs(local.sin_port);
      }
    }
    ts->setup = true;
    this->pending_track_ = track;

    std::string transport_reply = this->build_transport_header_(*ts);
    std::string content = extra_headers + "Session: " + to_string(this->session_id_) + "\r\n" + transport_reply;
    this->send_response_(200, "OK", content.c_str(), nullptr, 0);
    return;
  }
  if (method == "PLAY") {
    this->playing_ = true;
    this->video_timestamp_base_ = static_cast<uint32_t>(millis() * 90);
    std::string content = extra_headers + "Session: " + to_string(this->session_id_) + "\r\n";
    if (this->video_track_.setup) {
      content += "RTP-Info: url=trackID=0;seq=" + to_string(this->h264_.sequence_number()) + "\r\n";
    }
    this->send_response_(200, "OK", content.c_str(), nullptr, 0);
    return;
  }
  if (method == "PAUSE") {
    this->playing_ = false;
    std::string content = extra_headers + "Session: " + to_string(this->session_id_) + "\r\n";
    this->send_response_(200, "OK", content.c_str(), nullptr, 0);
    return;
  }
  if (method == "TEARDOWN") {
    this->teardown_requested_ = true;
    this->playing_ = false;
    std::string content = extra_headers + "Session: " + to_string(this->session_id_) + "\r\n";
    this->send_response_(200, "OK", content.c_str(), nullptr, 0);
    this->running_ = false;
    return;
  }
  if (method == "GET_PARAMETER") {
    this->send_response_(200, "OK", extra_headers.c_str(), nullptr, 0);
    return;
  }
  if (method == "SET_PARAMETER") {
    this->send_response_(200, "OK", extra_headers.c_str(), nullptr, 0);
    return;
  }
  this->send_response_(405, "Method Not Allowed", extra_headers.c_str(), nullptr, 0);
}

void RtspSession::send_response_(int code, const char *reason, const char *extra_headers, const char *body,
                                 size_t body_len) {
  std::string msg = "RTSP/1.0 " + to_string(code) + " " + reason + "\r\n";
  msg += "Server: ESPHome-P4-RTSP/0.1\r\n";
  if (extra_headers != nullptr) {
    msg += extra_headers;
  }
  msg += "\r\n";
  std::lock_guard<std::mutex> lock(this->send_mutex_);
  send(this->fd_, msg.c_str(), msg.size(), 0);
  if (body != nullptr && body_len > 0) {
    send(this->fd_, body, body_len, 0);
  }
}

std::string RtspSession::build_sdp_() const {
  struct sockaddr_in local {};
  socklen_t len = sizeof(local);
  getsockname(this->fd_, (struct sockaddr *) &local, &len);
  std::string ip = inet_ntoa(local.sin_addr);

  std::string sdp;
  sdp += "v=0\r\n";
  sdp += "o=- " + to_string(this->session_id_) + " " + to_string(this->session_id_) + " IN IP4 " + ip + "\r\n";
  sdp += "s=ESP32-P4 RTSP Stream\r\n";
  sdp += "t=0 0\r\n";
  if (this->server_->video_width_ > 0) {
    sdp += "m=video 0 RTP/AVP 96\r\n";
    sdp += "a=control:trackID=0\r\n";
    sdp += "a=rtpmap:96 H264/90000\r\n";
    sdp += "a=fmtp:96 packetization-mode=1;profile-level-id=42001f";
    if (!this->h264_.sps().empty() && !this->h264_.pps().empty()) {
      sdp += ";sprop-parameter-sets=" + base64_encode(this->h264_.sps().data(), this->h264_.sps().size()) +
             "," + base64_encode(this->h264_.pps().data(), this->h264_.pps().size());
    }
    sdp += "\r\n";
  }
  sdp += "m=audio 0 RTP/AVP 97\r\n";
  sdp += "a=control:trackID=1\r\n";
  sdp += "a=rtpmap:97 L16/" + to_string(this->server_->audio_sample_rate()) + "/" +
         to_string(this->server_->audio_channels()) + "\r\n";
  return sdp;
}

std::string RtspSession::build_transport_header_(const TrackState &track) const {
  std::string out = "Transport: ";
  if (track.transport == TransportKind::INTERLEAVED) {
    out += "RTP/AVP/TCP;unicast;interleaved=" + to_string(track.interleaved_channel) + "-" +
           to_string(track.interleaved_channel + 1);
  } else {
    out += "RTP/AVP;unicast;client_port=" + to_string(track.client_rtp_port) + "-" +
           to_string(track.client_rtcp_port);
    if (track.server_rtp_port > 0) {
      out += ";server_port=" + to_string(track.server_rtp_port);
    }
    out += ";ssrc=" + to_string(this->h264_.ssrc());
  }
  return out + "\r\n";
}

void RtspSession::send_track_packet_(const TrackState &track, const uint8_t *rtp, size_t len) {
  if (track.transport == TransportKind::UDP && track.rtp_socket >= 0) {
    struct sockaddr_in dest {};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = track.client_ip;
    dest.sin_port = htons(track.client_rtp_port);
    sendto(track.rtp_socket, rtp, len, 0, (struct sockaddr *) &dest, sizeof(dest));
    return;
  }
  if (track.transport == TransportKind::INTERLEAVED && track.interleaved_channel >= 0) {
    uint8_t header[4];
    header[0] = '$';
    header[1] = static_cast<uint8_t>(track.interleaved_channel);
    header[2] = static_cast<uint8_t>((len >> 8) & 0xff);
    header[3] = static_cast<uint8_t>(len & 0xff);
    std::lock_guard<std::mutex> lock(this->send_mutex_);
    send(this->fd_, header, 4, 0);
    send(this->fd_, rtp, len, 0);
  }
}

}  // namespace p4_rtsp
}  // namespace esphome
