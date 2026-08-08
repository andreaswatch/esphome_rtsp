#pragma once

#include "driver/ledc.h"
#include "esphome/core/log.h"

inline void start_es8311_mclk() {
  ledc_timer_config_t timer_cfg = {};
  timer_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
  timer_cfg.timer_num = LEDC_TIMER_0;
  timer_cfg.duty_resolution = LEDC_TIMER_1_BIT;
  timer_cfg.freq_hz = 2048000;
  timer_cfg.clk_cfg = LEDC_AUTO_CLK;
  ledc_timer_config(&timer_cfg);

  ledc_channel_config_t chan_cfg = {};
  chan_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
  chan_cfg.channel = LEDC_CHANNEL_0;
  chan_cfg.timer_sel = LEDC_TIMER_0;
  chan_cfg.intr_type = LEDC_INTR_DISABLE;
  chan_cfg.gpio_num = 13;
  chan_cfg.duty = 1;
  chan_cfg.hpoint = 0;
  ledc_channel_config(&chan_cfg);
  ESP_LOGI("main", "Started 2.048 MHz MCLK on GPIO13 via LEDC timer");
}
