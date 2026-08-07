#include "camera_pipeline.h"

#include <cstdio>
#include <cstring>

#include "driver/i2c_master.h"
#include "driver/isp.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "esp_cam_sensor_xclk.h"
#include "esp_cache.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_sccb_intf.h"
extern "C" {
#include "esp_sccb_i2c.h"
}
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/color_types.h"

namespace esphome {
namespace p4_rtsp {

static const char *const TAG = "p4_rtsp.camera";

static constexpr uint8_t K_FRAME_DIMENSION_ALIGNMENT = 16;
static constexpr uint8_t K_YUV420_BYTES_NUMERATOR = 3;
static constexpr uint8_t K_YUV420_BYTES_DENOMINATOR = 2;
static constexpr size_t K_FRAME_BUFFER_ALIGNMENT_BYTES = 64;
static constexpr uint8_t K_CSI_QUEUE_ITEMS = 1;
static constexpr uint8_t K_SENSOR_STREAM_ENABLE = 1;
static constexpr uint8_t K_FRAME_BUFFER_COUNT = 2;
static constexpr uint32_t K_CAPTURE_QUEUE_WAIT_MS = 1000;
static constexpr uint32_t K_ISP_PROCESSOR_CLOCK_HZ = 80 * 1000000;
static constexpr uint8_t K_DEMOSAIC_GRAD_RATIO_INTEGER = 2;
static constexpr uint8_t K_DEMOSAIC_GRAD_RATIO_DECIMAL = 5;

static constexpr uint32_t align16(uint32_t v) { return (v + 15u) & ~15u; }

CameraPipeline::CameraPipeline() = default;

CameraPipeline::~CameraPipeline() { this->stop(); }

void CameraPipeline::set_config(int width, int height, int fps, int bitrate, int gop, int sccb_sda,
                                int sccb_scl, int xclk_pin, int data_lanes, int power_down_pin) {
  this->width_ = width;
  this->height_ = height;
  this->fps_ = fps;
  this->bitrate_ = bitrate;
  this->gop_ = gop;
  this->sccb_sda_ = sccb_sda;
  this->sccb_scl_ = sccb_scl;
  this->xclk_pin_ = xclk_pin;
  this->data_lanes_ = data_lanes;
  this->power_down_pin_ = power_down_pin;
}

void CameraPipeline::set_frame_callback(FrameCallback callback) {
  this->frame_callback_ = std::move(callback);
}

void CameraPipeline::start_task_wrapper(void *param) {
  auto *self = static_cast<CameraPipeline *>(param);
  bool ok = self->start();
  self->start_succeeded_ = ok;
  self->starting_ = false;
  ESP_LOGI(TAG, "camera start task finished: %s", ok ? "OK" : "FAILED");
  vTaskDelete(nullptr);
}

bool CameraPipeline::start_async() {
  if (this->running_ || this->starting_) {
    return true;
  }
  this->starting_ = true;
  this->start_succeeded_ = false;
  if (xTaskCreatePinnedToCore(CameraPipeline::start_task_wrapper, "cam_start", 8192, this, 5, nullptr, 0) !=
      pdPASS) {
    ESP_LOGE(TAG, "failed to create camera start task");
    this->starting_ = false;
    return false;
  }
  return true;
}

bool CameraPipeline::start() {
  if (this->running_) {
    return true;
  }
  // Power the MIPI-CSI PHY (Function-EV-Board: LDO ch3 @ 2.5V). Best effort —
  // some boards power the PHY externally.
  esp_ldo_channel_handle_t ldo = nullptr;
  esp_ldo_channel_config_t ldo_cfg = {};
  ldo_cfg.chan_id = 3;
  ldo_cfg.voltage_mv = 2500;
  if (esp_ldo_acquire_channel(&ldo_cfg, &ldo) == ESP_OK) {
    this->ldo_ = ldo;
    ESP_LOGI(TAG, "MIPI PHY LDO ch3 @2500mV OK");
  } else {
    ESP_LOGW(TAG, "MIPI PHY LDO ch3 unavailable (externally powered?)");
  }
  if (!this->init_sensor_()) {
    return false;
  }

  // Frame buffers (YUV420, sized on 16-aligned dims as the H264 encoder expects).
  this->fb_size_ = static_cast<size_t>(align16(this->target_width_)) * align16(this->target_height_) *
                   K_YUV420_BYTES_NUMERATOR / K_YUV420_BYTES_DENOMINATOR;
  this->produced_ = xQueueCreate(K_FRAME_BUFFER_COUNT, sizeof(void *));
  this->consumed_ = xQueueCreate(K_FRAME_BUFFER_COUNT, sizeof(void *));
  if (this->produced_ == nullptr || this->consumed_ == nullptr) {
    ESP_LOGE(TAG, "frame queue alloc failed");
    return false;
  }
  for (int i = 0; i < K_FRAME_BUFFER_COUNT; i++) {
    void *buf = heap_caps_aligned_alloc(K_FRAME_BUFFER_ALIGNMENT_BYTES, this->fb_size_,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    bool dma_capable = buf != nullptr;
    if (buf == nullptr) {
      buf = heap_caps_aligned_alloc(K_FRAME_BUFFER_ALIGNMENT_BYTES, this->fb_size_,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (buf == nullptr) {
      ESP_LOGE(TAG, "frame buffer %d alloc failed (%u B)", i, static_cast<unsigned>(this->fb_size_));
      return false;
    }
    ESP_LOGI(TAG, "frame buffer %d: %u B PSRAM, dma=%d", i, static_cast<unsigned>(this->fb_size_), dma_capable);
    this->cap_bufs_[i] = buf;
    xQueueSendToBack(this->consumed_, &buf, 0);
  }
  ESP_LOGI(TAG, "frame buffers: %d x %u B PSRAM (%ux%u)", K_FRAME_BUFFER_COUNT,
           static_cast<unsigned>(this->fb_size_), this->target_width_, this->target_height_);

  if (!this->init_encoder_()) {
    return false;
  }
  if (!this->setup_isp_()) {
    return false;
  }
  if (!this->setup_csi_()) {
    return false;
  }

  int stream_enable = K_SENSOR_STREAM_ENABLE;
  if (esp_cam_sensor_ioctl(static_cast<esp_cam_sensor_device_t *>(this->sensor_),
                           ESP_CAM_SENSOR_IOC_S_STREAM, &stream_enable) != ESP_OK) {
    ESP_LOGE(TAG, "sensor stream start failed");
    return false;
  }
  ESP_LOGI(TAG, "sensor streaming started");

  this->running_ = true;
  xTaskCreatePinnedToCore(CameraPipeline::capture_task_wrapper, "cam_capture", 8192, this, 5, nullptr, 1);
  ESP_LOGI(TAG, "camera pipeline started %ux%u", this->target_width_, this->target_height_);
  return true;
}

void CameraPipeline::stop() {
  if (!this->running_) {
    return;
  }
  this->running_ = false;
  vTaskDelay(pdMS_TO_TICKS(50));

  if (this->cam_handle_ != nullptr) {
    esp_cam_ctlr_stop(static_cast<esp_cam_ctlr_handle_t>(this->cam_handle_));
    esp_cam_ctlr_disable(static_cast<esp_cam_ctlr_handle_t>(this->cam_handle_));
    this->cam_handle_ = nullptr;
  }
  if (this->isp_proc_ != nullptr) {
    esp_isp_disable(static_cast<isp_proc_handle_t>(this->isp_proc_));
    esp_isp_del_processor(static_cast<isp_proc_handle_t>(this->isp_proc_));
    this->isp_proc_ = nullptr;
  }
  if (this->sensor_ != nullptr) {
    int stream_disable = 0;
    esp_cam_sensor_ioctl(static_cast<esp_cam_sensor_device_t *>(this->sensor_),
                         ESP_CAM_SENSOR_IOC_S_STREAM, &stream_disable);
    this->sensor_ = nullptr;
  }
  if (this->sccb_io_ != nullptr) {
    esp_sccb_del_i2c_io(static_cast<esp_sccb_io_handle_t>(this->sccb_io_));
    this->sccb_io_ = nullptr;
  }
  if (this->sccb_bus_ != nullptr) {
    i2c_master_bus_handle_t bus = static_cast<i2c_master_bus_handle_t>(this->sccb_bus_);
    i2c_del_master_bus(bus);
    this->sccb_bus_ = nullptr;
  }
  if (this->xclk_ != nullptr) {
    esp_cam_sensor_xclk_stop(static_cast<esp_cam_sensor_xclk_handle_t>(this->xclk_));
    esp_cam_sensor_xclk_free(static_cast<esp_cam_sensor_xclk_handle_t>(this->xclk_));
    this->xclk_ = nullptr;
  }
  if (this->power_down_pin_ >= 0) {
    gpio_set_level(static_cast<gpio_num_t>(this->power_down_pin_), 1);
  }
  if (this->ldo_ != nullptr) {
    esp_ldo_release_channel(static_cast<esp_ldo_channel_handle_t>(this->ldo_));
    this->ldo_ = nullptr;
  }
  this->deinit_encoder_();
  for (int i = 0; i < K_FRAME_BUFFER_COUNT; i++) {
    if (this->cap_bufs_[i] != nullptr) {
      heap_caps_free(this->cap_bufs_[i]);
      this->cap_bufs_[i] = nullptr;
    }
  }
  if (this->produced_ != nullptr) {
    vQueueDelete(this->produced_);
    this->produced_ = nullptr;
  }
  if (this->consumed_ != nullptr) {
    vQueueDelete(this->consumed_);
    this->consumed_ = nullptr;
  }
  ESP_LOGI(TAG, "camera pipeline stopped");
}

bool CameraPipeline::init_sensor_() {
  // Desired resolution from YAML config — used to pick the matching sensor format.
  this->target_width_ = static_cast<uint32_t>(this->width_);
  this->target_height_ = static_cast<uint32_t>(this->height_);

  i2c_master_bus_config_t bus_cfg = {};
  bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_cfg.i2c_port = I2C_NUM_1;
  bus_cfg.scl_io_num = static_cast<gpio_num_t>(this->sccb_scl_);
  bus_cfg.sda_io_num = static_cast<gpio_num_t>(this->sccb_sda_);
  bus_cfg.glitch_ignore_cnt = 7;
  bus_cfg.flags.enable_internal_pullup = true;
  i2c_master_bus_handle_t bus = nullptr;
  if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
    ESP_LOGE(TAG, "SCCB i2c bus init failed (sda=%d scl=%d)", this->sccb_sda_, this->sccb_scl_);
    return false;
  }
  this->sccb_bus_ = bus;

  // Release the sensor power-down so it can stream MIPI data.
  if (this->power_down_pin_ >= 0) {
    gpio_set_direction(static_cast<gpio_num_t>(this->power_down_pin_), GPIO_MODE_OUTPUT);
    gpio_set_level(static_cast<gpio_num_t>(this->power_down_pin_), 0);
    ESP_LOGI(TAG, "sensor power_down GPIO%d released", this->power_down_pin_);
  }

  // The sensor needs its MCLK (XCLK) running before it can be detected/stream.
  esp_cam_sensor_xclk_handle_t xclk = nullptr;
  if (esp_cam_sensor_xclk_allocate(ESP_CAM_SENSOR_XCLK_ESP_CLOCK_ROUTER, &xclk) != ESP_OK) {
    ESP_LOGE(TAG, "xclk allocate failed");
    return false;
  }
  esp_cam_sensor_xclk_config_t xclk_cfg = {};
  xclk_cfg.esp_clock_router_cfg.xclk_pin = static_cast<gpio_num_t>(this->xclk_pin_);
  xclk_cfg.esp_clock_router_cfg.xclk_freq_hz = 24000000;
  if (esp_cam_sensor_xclk_start(xclk, &xclk_cfg) != ESP_OK) {
    ESP_LOGE(TAG, "xclk start failed on GPIO%d (24MHz)", this->xclk_pin_);
    esp_cam_sensor_xclk_free(xclk);
    return false;
  }
  this->xclk_ = xclk;
  ESP_LOGI(TAG, "XCLK 24MHz on GPIO%d started", this->xclk_pin_);

  // Let the sensor power up / PLL settle before probing it.
  vTaskDelay(pdMS_TO_TICKS(200));

  for (esp_cam_sensor_detect_fn_t *fn = &__esp_cam_sensor_detect_fn_array_start;
       fn < &__esp_cam_sensor_detect_fn_array_end; ++fn) {
    if (fn->port != ESP_CAM_SENSOR_MIPI_CSI) {
      continue;
    }
    sccb_i2c_config_t sccb_cfg = {};
    sccb_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    sccb_cfg.device_address = fn->sccb_addr;
    sccb_cfg.scl_speed_hz = 100000;
    esp_sccb_io_handle_t io = nullptr;
    if (sccb_new_i2c_io(bus, &sccb_cfg, &io) != ESP_OK) {
      continue;
    }
    esp_cam_sensor_config_t sensor_cfg = {};
    sensor_cfg.sccb_handle = io;
    sensor_cfg.reset_pin = -1;
    sensor_cfg.pwdn_pin = static_cast<int8_t>(this->power_down_pin_);
    sensor_cfg.sensor_port = ESP_CAM_SENSOR_MIPI_CSI;
    esp_cam_sensor_device_t *dev = (*(fn->detect))(&sensor_cfg);
    if (dev != nullptr) {
      this->sensor_ = dev;
      this->sccb_io_ = io;
      ESP_LOGI(TAG, "Detected sensor: %s", esp_cam_sensor_get_name(dev));
      break;
    }
    esp_sccb_del_i2c_io(io);
  }

  if (this->sensor_ == nullptr) {
    ESP_LOGE(TAG, "No MIPI-CSI camera sensor detected");
    return false;
  }

  auto *sensor = static_cast<esp_cam_sensor_device_t *>(this->sensor_);

  // Query all compiled-in formats, then pick the one matching the configured
  // resolution and ACTIVELY program it into the sensor (set_format writes the
  // OV5647 PLL/MIPI/resolution registers). get_format() alone only reads the
  // current (uninitialised) state and leaves the sensor outputting nothing.
  esp_cam_sensor_format_array_t available {};
  esp_cam_sensor_query_format(sensor, &available);
  const esp_cam_sensor_format_t *selected = nullptr;
  const esp_cam_sensor_format_t *fallback = nullptr;
  for (size_t i = 0; i < available.count; i++) {
    const esp_cam_sensor_format_t *f = &available.format_array[i];
    ESP_LOGI(TAG, "  available format [%zu]: %s (%ux%u @%ufps)", i, f->name, f->width, f->height, f->fps);
    if (fallback == nullptr) {
      fallback = f;
    }
    if (f->width == this->target_width_ && f->height == this->target_height_) {
      selected = f;
    }
  }
  if (selected == nullptr) {
    selected = fallback;
  }
  if (selected == nullptr) {
    ESP_LOGE(TAG, "no sensor format available");
    return false;
  }
  if (esp_cam_sensor_set_format(sensor, selected) != ESP_OK) {
    ESP_LOGE(TAG, "esp_cam_sensor_set_format failed for %s", selected->name);
    return false;
  }
  ESP_LOGI(TAG, "sensor format programmed: %s (%ux%u @%ufps)", selected->name, selected->width,
           selected->height, selected->fps);
  this->target_width_ = selected->width;
  this->target_height_ = selected->height;
  this->lane_count_ = selected->mipi_info.lane_num;
  this->lane_bitrate_mbps_ = selected->mipi_info.mipi_clk / 1000000;
  this->raw8_ = selected->format == ESP_CAM_SENSOR_PIXFORMAT_RAW8;
  return true;
}

bool CameraPipeline::setup_isp_() {
  auto *sensor = static_cast<esp_cam_sensor_device_t *>(this->sensor_);
  esp_cam_sensor_format_t sensor_format {};
  esp_cam_sensor_get_format(sensor, &sensor_format);

  color_raw_element_order_t bayer = COLOR_RAW_ELEMENT_ORDER_GBRG;
  if (sensor_format.isp_info != nullptr) {
    switch (sensor_format.isp_info->isp_v1_info.bayer_type) {
      case ESP_CAM_SENSOR_BAYER_RGGB:
        bayer = COLOR_RAW_ELEMENT_ORDER_RGGB;
        break;
      case ESP_CAM_SENSOR_BAYER_GRBG:
        bayer = COLOR_RAW_ELEMENT_ORDER_GRBG;
        break;
      case ESP_CAM_SENSOR_BAYER_BGGR:
        bayer = COLOR_RAW_ELEMENT_ORDER_BGGR;
        break;
      default:
        bayer = COLOR_RAW_ELEMENT_ORDER_GBRG;
        break;
    }
  }

  esp_isp_processor_cfg_t isp_cfg = {};
  isp_cfg.clk_hz = K_ISP_PROCESSOR_CLOCK_HZ;
  isp_cfg.input_data_source = ISP_INPUT_DATA_SOURCE_CSI;
  isp_cfg.input_data_color_type = this->raw8_ ? ISP_COLOR_RAW8 : ISP_COLOR_RAW10;
  isp_cfg.output_data_color_type = ISP_COLOR_YUV420;
  isp_cfg.bayer_order = bayer;
  isp_cfg.has_line_start_packet = sensor_format.mipi_info.line_sync_en;
  isp_cfg.has_line_end_packet = sensor_format.mipi_info.line_sync_en;
  isp_cfg.h_res = this->target_width_;
  isp_cfg.v_res = this->target_height_;
  isp_proc_handle_t proc = nullptr;
  if (esp_isp_new_processor(&isp_cfg, &proc) != ESP_OK) {
    ESP_LOGE(TAG, "ISP init failed");
    return false;
  }
  this->isp_proc_ = proc;
  esp_isp_enable(proc);

  esp_isp_demosaic_config_t demosaic_cfg = {};
  demosaic_cfg.grad_ratio.integer = K_DEMOSAIC_GRAD_RATIO_INTEGER;
  demosaic_cfg.grad_ratio.decimal = K_DEMOSAIC_GRAD_RATIO_DECIMAL;
  demosaic_cfg.padding_mode = ISP_DEMOSAIC_EDGE_PADDING_MODE_SRND_DATA;
  if (esp_isp_demosaic_configure(proc, &demosaic_cfg) == ESP_OK) {
    esp_isp_demosaic_enable(proc);
  }
  ESP_LOGI(TAG, "ISP: RAW -> YUV420, bayer=%d, line_sync=%d", static_cast<int>(bayer),
           sensor_format.mipi_info.line_sync_en);
  return true;
}

bool CameraPipeline::setup_csi_() {
  esp_cam_ctlr_csi_config_t csi_cfg = {};
  csi_cfg.ctlr_id = 0;
  csi_cfg.clk_src = MIPI_CSI_PHY_CLK_SRC_DEFAULT;
  csi_cfg.h_res = this->target_width_;
  csi_cfg.v_res = this->target_height_;
  csi_cfg.data_lane_num = this->lane_count_;
  csi_cfg.lane_bit_rate_mbps = this->lane_bitrate_mbps_;
  csi_cfg.input_data_color_type = this->raw8_ ? CAM_CTLR_COLOR_RAW8 : CAM_CTLR_COLOR_RAW10;
  csi_cfg.output_data_color_type = CAM_CTLR_COLOR_YUV420;
  csi_cfg.queue_items = K_CSI_QUEUE_ITEMS;
  csi_cfg.byte_swap_en = false;
  csi_cfg.bk_buffer_dis = false;

  esp_cam_ctlr_handle_t handle = nullptr;
  if (esp_cam_new_csi_ctlr(&csi_cfg, &handle) != ESP_OK) {
    ESP_LOGE(TAG, "CSI controller init failed");
    return false;
  }
  this->cam_handle_ = handle;

  esp_cam_ctlr_evt_cbs_t cbs = {};
  cbs.on_get_new_trans = CameraPipeline::s_dma_start;
  cbs.on_trans_finished = CameraPipeline::s_dma_complete;
  if (esp_cam_ctlr_register_event_callbacks(handle, &cbs, this) != ESP_OK) {
    ESP_LOGE(TAG, "CSI event callbacks failed");
    return false;
  }
  if (esp_cam_ctlr_enable(handle) != ESP_OK || esp_cam_ctlr_start(handle) != ESP_OK) {
    ESP_LOGE(TAG, "CSI controller start failed");
    return false;
  }
  ESP_LOGI(TAG, "CSI controller started");
  return true;
}

bool IRAM_ATTR CameraPipeline::s_dma_start(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans,
                                           void *user_data) {
  (void) handle;
  return static_cast<CameraPipeline *>(user_data)->dma_start_cb_(trans);
}

bool IRAM_ATTR CameraPipeline::s_dma_complete(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans,
                                              void *user_data) {
  (void) handle;
  return static_cast<CameraPipeline *>(user_data)->dma_complete_cb_(trans);
}

bool CameraPipeline::dma_start_cb_(esp_cam_ctlr_trans_t *trans) {
  void *next_frame_buffer = nullptr;
  if (xQueueReceiveFromISR(this->consumed_, &next_frame_buffer, nullptr) != pdPASS) {
    if (xQueueReceiveFromISR(this->produced_, &next_frame_buffer, nullptr) != pdPASS) {
      return false;
    }
  }
  trans->buffer = next_frame_buffer;
  trans->buflen = this->fb_size_;
  return true;
}

bool CameraPipeline::dma_complete_cb_(esp_cam_ctlr_trans_t *trans) {
  if (xQueueSendFromISR(this->produced_, &trans->buffer, nullptr) != pdPASS) {
    xQueueSendFromISR(this->consumed_, &trans->buffer, nullptr);
  }
  return true;
}

bool CameraPipeline::init_encoder_() {
  esp_h264_enc_cfg_hw_t cfg = {};
  cfg.gop = this->gop_;
  cfg.fps = this->fps_;
  cfg.res.width = this->target_width_;
  cfg.res.height = this->target_height_;
  cfg.rc.bitrate = this->bitrate_;
  cfg.rc.qp_min = 26;
  cfg.rc.qp_max = 30;
  cfg.pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY;

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
  this->enc_buf_len_ = this->target_width_ * this->target_height_ * 2;
  this->enc_buf_ = heap_caps_aligned_alloc(64, this->enc_buf_len_, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
  if (this->enc_buf_ == nullptr) {
    ESP_LOGE(TAG, "encoder buffer alloc failed (%u bytes)", static_cast<unsigned>(this->enc_buf_len_));
    esp_h264_enc_close(enc);
    esp_h264_enc_del(enc);
    this->encoder_ = nullptr;
    return false;
  }
  return true;
}

void CameraPipeline::deinit_encoder_() {
  if (this->enc_buf_ != nullptr) {
    heap_caps_free(this->enc_buf_);
    this->enc_buf_ = nullptr;
  }
  if (this->encoder_ != nullptr) {
    auto *enc = static_cast<esp_h264_enc_handle_t>(this->encoder_);
    esp_h264_enc_close(enc);
    esp_h264_enc_del(enc);
    this->encoder_ = nullptr;
  }
}

void CameraPipeline::capture_task_wrapper(void *param) {
  auto *pipeline = static_cast<CameraPipeline *>(param);
  pipeline->capture_task();
  vTaskDelete(nullptr);
}

void CameraPipeline::capture_task() {
  uint32_t frame_count = 0;
  uint32_t rate_start_ms = 0;
  while (this->running_) {
    void *frame_buffer = nullptr;
    if (xQueueReceive(this->produced_, &frame_buffer, pdMS_TO_TICKS(K_CAPTURE_QUEUE_WAIT_MS)) != pdTRUE) {
      ESP_LOGW(TAG, "no frame in %u ms", K_CAPTURE_QUEUE_WAIT_MS);
      continue;
    }
    esp_cache_msync(frame_buffer, this->fb_size_, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    uint32_t now = static_cast<uint32_t>(millis());
    if (this->encode_frame_(static_cast<uint8_t *>(frame_buffer), this->fb_size_, now)) {
      frame_count++;
    }
    if (frame_count <= 10) {
      ESP_LOGI(TAG, "frame #%u encoded", frame_count);
    } else if (frame_count % 300 == 0) {
      uint32_t fps = 0;
      if (rate_start_ms != 0 && now > rate_start_ms) {
        fps = frame_count * 1000u / (now - rate_start_ms);
      }
      ESP_LOGI(TAG, "frame #%u (~%u fps)", frame_count, fps);
      rate_start_ms = now;
      frame_count = 0;
    }
    xQueueSendToBack(this->consumed_, &frame_buffer, 0);
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
    ESP_LOGE(TAG, "encode failed: %d", err);
    return false;
  }
  this->frame_index_++;
  // NOTE: out_frame.raw_data.len is the CAPACITY we passed in, NOT the encoded
  // size. The driver reports the real bitstream length in out_frame.length.
  // (esp_h264_types.h:167)
  uint32_t encoded_len = out_frame.length;
  if (encoded_len == 0 || encoded_len > this->enc_buf_len_) {
    ESP_LOGW(TAG, "encoder returned length %u (buf %u) — skipping frame", static_cast<unsigned>(encoded_len),
             static_cast<unsigned>(this->enc_buf_len_));
    return false;
  }
  bool keyframe = (out_frame.frame_type == ESP_H264_FRAME_TYPE_IDR ||
                   out_frame.frame_type == ESP_H264_FRAME_TYPE_I);
  if (this->frame_callback_) {
    this->frame_callback_(out_frame.raw_data.buffer, encoded_len, keyframe, timestamp_ms);
  }
  return true;
}

}  // namespace p4_rtsp
}  // namespace esphome
