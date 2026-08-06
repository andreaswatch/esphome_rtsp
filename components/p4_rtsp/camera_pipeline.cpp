#include "camera_pipeline.h"

#include <cstring>
#include <sys/fcntl.h>
#include <sys/ioctl.h>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome {
namespace p4_rtsp {

static const char *const TAG = "p4_rtsp.camera";

static constexpr const char *VIDEO_DEVICE_PATH = "/dev/video0";
static constexpr uint32_t IMAGE_PIX_FMT = V4L2_PIX_FMT_UYVY;
static constexpr uint32_t FRAME_TIMEOUT_MS = 100;

CameraPipeline::CameraPipeline() = default;

CameraPipeline::~CameraPipeline() {
  this->stop();
  this->deinit_camera_();
  this->deinit_encoder_();
}

void CameraPipeline::set_config(int width, int height, int fps, int bitrate, int gop, int sccb_sda,
                                int sccb_scl, int xclk_pin, int data_lanes) {
  this->width_ = width;
  this->height_ = height;
  this->fps_ = fps;
  this->bitrate_ = bitrate;
  this->gop_ = gop;
  this->sccb_sda_ = sccb_sda;
  this->sccb_scl_ = sccb_scl;
  this->xclk_pin_ = xclk_pin;
  this->data_lanes_ = data_lanes;
}

void CameraPipeline::set_frame_callback(FrameCallback callback) {
  this->frame_callback_ = std::move(callback);
}

bool CameraPipeline::start() {
  if (this->running_) {
    return true;
  }
  if (!this->init_camera_()) {
    return false;
  }
  if (!this->init_encoder_()) {
    this->deinit_camera_();
    return false;
  }
  this->running_ = true;
  xTaskCreatePinnedToCore(CameraPipeline::capture_task_wrapper, "rtsp_cam", 8192, this, 4, nullptr, 0);
  ESP_LOGI(TAG, "Camera pipeline started at %dx%d", this->width_, this->height_);
  return true;
}

void CameraPipeline::stop() {
  if (!this->running_) {
    return;
  }
  this->running_ = false;
  vTaskDelay(pdMS_TO_TICKS(50));
  this->deinit_encoder_();
  this->deinit_camera_();
  ESP_LOGI(TAG, "Camera pipeline stopped");
}

void CameraPipeline::capture_task_wrapper(void *param) {
  auto *pipeline = static_cast<CameraPipeline *>(param);
  pipeline->capture_task();
  vTaskDelete(nullptr);
}

bool CameraPipeline::init_camera_() {
  if (!this->esp_video_initialized_) {
    esp_video_init_sccb_config_t sccb = {};
    sccb.init_sccb = true;
    sccb.i2c_config.port = 1;
    sccb.i2c_config.scl_pin = this->sccb_scl_;
    sccb.i2c_config.sda_pin = this->sccb_sda_;
    sccb.freq = 100000;

    esp_video_init_csi_config_t csi = {};
    csi.sccb_config = sccb;
    csi.reset_pin = -1;
    csi.pwdn_pin = -1;

    esp_video_init_config_t init_config = {};
    init_config.csi = &csi;
    int err = esp_video_init(&init_config);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_video_init failed: %d", err);
      return false;
    }
    this->esp_video_initialized_ = true;
  }

  this->video_fd_ = open(VIDEO_DEVICE_PATH, O_RDWR);
  if (this->video_fd_ < 0) {
    ESP_LOGE(TAG, "open(%s) failed", VIDEO_DEVICE_PATH);
    return false;
  }

  struct v4l2_format fmt {};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = this->width_;
  fmt.fmt.pix.height = this->height_;
  fmt.fmt.pix.pixelformat = IMAGE_PIX_FMT;
  fmt.fmt.pix.field = V4L2_FIELD_NONE;
  if (ioctl(this->video_fd_, VIDIOC_S_FMT, &fmt) < 0) {
    ESP_LOGE(TAG, "VIDIOC_S_FMT failed");
    close(this->video_fd_);
    this->video_fd_ = -1;
    return false;
  }

  struct v4l2_requestbuffers reqbuf {};
  reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  reqbuf.memory = V4L2_MEMORY_USERPTR;
  reqbuf.count = 2;
  if (ioctl(this->video_fd_, VIDIOC_REQBUFS, &reqbuf) < 0) {
    ESP_LOGE(TAG, "VIDIOC_REQBUFS failed");
    close(this->video_fd_);
    this->video_fd_ = -1;
    return false;
  }

  this->cap_buf_len_ = fmt.fmt.pix.sizeimage;
  this->cap_buf_ = calloc(1, this->cap_buf_len_);
  if (this->cap_buf_ == nullptr) {
    ESP_LOGE(TAG, "capture buffer alloc failed");
    close(this->video_fd_);
    this->video_fd_ = -1;
    return false;
  }

  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(this->video_fd_, VIDIOC_STREAMON, &type) < 0) {
    ESP_LOGE(TAG, "VIDIOC_STREAMON failed");
    free(this->cap_buf_);
    this->cap_buf_ = nullptr;
    close(this->video_fd_);
    this->video_fd_ = -1;
    return false;
  }
  return true;
}

void CameraPipeline::deinit_camera_() {
  if (this->cap_buf_ != nullptr) {
    free(this->cap_buf_);
    this->cap_buf_ = nullptr;
  }
  if (this->video_fd_ >= 0) {
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(this->video_fd_, VIDIOC_STREAMOFF, &type);
    close(this->video_fd_);
    this->video_fd_ = -1;
  }
}

bool CameraPipeline::init_encoder_() {
  esp_h264_enc_cfg_hw_t cfg = {};
  cfg.gop = this->gop_;
  cfg.fps = this->fps_;
  cfg.res.width = this->width_;
  cfg.res.height = this->height_;
  cfg.rc.bitrate = this->bitrate_;
  cfg.rc.qp_min = 26;
  cfg.rc.qp_max = 30;
  cfg.pic_type = ESP_H264_RAW_FMT_UYVY;

  esp_h264_enc_handle_t enc = nullptr;
  int err = esp_h264_enc_hw_new(&cfg, &enc);
  if (err != ESP_H264_ERR_OK) {
    ESP_LOGE(TAG, "esp_h264_enc_hw_new failed: %d", err);
    return false;
  }
  this->encoder_ = enc;
  err = esp_h264_enc_open(enc);
  if (err != ESP_H264_ERR_OK) {
    ESP_LOGE(TAG, "esp_h264_enc_open failed: %d", err);
    esp_h264_enc_del(enc);
    this->encoder_ = nullptr;
    return false;
  }
  this->enc_buf_len_ = this->width_ * this->height_ * 2;
  this->enc_buf_ = calloc(1, this->enc_buf_len_);
  if (this->enc_buf_ == nullptr) {
    esp_h264_enc_close(enc);
    esp_h264_enc_del(enc);
    this->encoder_ = nullptr;
    return false;
  }
  return true;
}

void CameraPipeline::deinit_encoder_() {
  if (this->enc_buf_ != nullptr) {
    free(this->enc_buf_);
    this->enc_buf_ = nullptr;
  }
  if (this->encoder_ != nullptr) {
    auto *enc = static_cast<esp_h264_enc_handle_t>(this->encoder_);
    esp_h264_enc_close(enc);
    esp_h264_enc_del(enc);
    this->encoder_ = nullptr;
  }
}

void CameraPipeline::capture_task() {
  while (this->running_) {
    struct v4l2_buffer buf {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_USERPTR;
    int err = ioctl(this->video_fd_, VIDIOC_DQBUF, &buf);
    if (err < 0) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    size_t len = buf.bytesused;
    if (len > this->cap_buf_len_) {
      len = this->cap_buf_len_;
    }
    if (buf.m.userptr != 0 && len > 0) {
      memcpy(this->cap_buf_, reinterpret_cast<const void *>(buf.m.userptr), len);
    }
    ioctl(this->video_fd_, VIDIOC_QBUF, &buf);

    uint32_t now = static_cast<uint32_t>(millis());
    this->encode_frame_(static_cast<uint8_t *>(this->cap_buf_), len, now);
  }
}

bool CameraPipeline::encode_frame_(const uint8_t *raw, size_t len, uint32_t timestamp_ms) {
  if (this->encoder_ == nullptr || this->enc_buf_ == nullptr) {
    return false;
  }
  auto *enc = static_cast<esp_h264_enc_handle_t>(this->encoder_);

  esp_h264_enc_in_frame_t in_frame = {};
  in_frame.raw_data.buffer = const_cast<uint8_t *>(raw);
  in_frame.raw_data.len = len;
  in_frame.pts = timestamp_ms;

  esp_h264_enc_out_frame_t out_frame = {};
  out_frame.raw_data.buffer = static_cast<uint8_t *>(this->enc_buf_);
  out_frame.raw_data.len = this->enc_buf_len_;

  int err = esp_h264_enc_process(enc, &in_frame, &out_frame);
  if (err != ESP_H264_ERR_OK) {
    return false;
  }
  this->frame_index_++;
  bool keyframe = (out_frame.frame_type == ESP_H264_FRAME_TYPE_IDR ||
                   out_frame.frame_type == ESP_H264_FRAME_TYPE_I);
  if (this->frame_callback_) {
    this->frame_callback_(out_frame.raw_data.buffer, out_frame.raw_data.len, keyframe, timestamp_ms);
  }
  return true;
}

}  // namespace p4_rtsp
}  // namespace esphome
