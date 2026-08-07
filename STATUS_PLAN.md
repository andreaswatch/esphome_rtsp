# ESP32-P4 RTSP Camera — Status & Implementation Plan

> **File:** `STATUS_PLAN.md` (Repository Root)  
> **Target Device:** ESP32-P4 (`diag_full.yaml`, `192.168.178.196`)  
> **Last Updated:** 2026-08-07  

---

## 🎯 Primary Goal
Enable stable H.264 Video + Audio streaming to **Frigate / go2rtc** with two-way audio backchannel (Microphone to ESP32 speaker).

---

## 📊 Current Status Overview

| Feature | Status | Details |
|---|---|---|
| H.264 Video Stream | ✅ **Working** | 800x800 @ ~35 FPS, Constrained Baseline |
| H.264 SPS/PPS SDP Caching | ✅ **Fixed** | Gecachte `sprop-parameter-sets` in DESCRIBE |
| PPS Short NAL Parsing | ✅ **Fixed** | `nal_len >= 1` in `rtp.cpp` allows short 4-byte PPS |
| Audio Output Stream | ✅ **Working** | L16 / 16kHz & G.711 PCMA/PCMU advertised |
| 2-Way Audio Backchannel | ✅ **Working** | `a=sendrecv` on `?backchannel=1`, G.711 decoding to I2S |
| Direct `ffprobe` Streaming | ✅ **Working** | Both Video & Audio probe clean |
| `go2rtc` Direct Ingestion | ✅ **VERIFIED WORKING** | Restream via `rtsp://127.0.0.1:8554/test` & `test_twoway` verified |
| **Speaker Mute Fix** | 🆕 **Implemented** | `set_mute_state(false)` + `set_volume(1.0)` in `run_speaker_sequence_()` |
| **Mic PDM Fix** | 🆕 **Implemented** | `use_microphone: false` in `diag_full.yaml` — analoger Mic-Pfad statt PDM |

---

## 🔍 Root Cause Analysis (2026-08-07)

### 1. Speaker produces NO sound → **ES8311 DAC is muted by default**
- The ESPHome `es8311` component **never calls `set_mute_off()`** in `setup()`.
- After reset, REG31 bits 5/6 are set → DAC output is muted.
- `set_volume(0.75)` in `setup()` only writes REG32 (volume level), **not** REG31 (mute).
- The `I2SAudioSpeaker::set_volume()` *would* unmute (via `audio_dac_->set_mute_off()`), but it is only invoked by an explicit `speaker.volume_set` automation — none exists.
- **Fix:** In `P4RtspStream::run_speaker_sequence_()` (p4_rtsp.cpp), explicitly call `speaker_->set_mute_state(false)` + `speaker_->set_volume(1.0f)` before `play()`.

### 2. Microphone audio is silent → **PDM mode wrongly activated**
- `audio_dac` had `use_microphone: true` → sets `use_mic_ = true` in the ES8311 component.
- `configure_mic_()` then writes `reg14 |= BIT(6)` → **switches the codec into PDM digital-microphone mode**.
- The Waveshare board's mic is the **analog DSDIN input** (GPIO9). In PDM mode no analog signal is converted → all-zero samples.
- **Fix:** Set `use_microphone: false` in `diag_full.yaml`. The analog mic path is enabled by default (REG14 = 0x1A), and `reg14` silently stays in analog mode.
- Additional: mic I2S channel changed to `left` (Waveshare ES8311 reference), matching the `EspHomeVoipLib` working config.

---

## 🟢 Verified Test Results with `go2rtc`

- **Single / Main Stream (`test`):**
  - Ingest URL: `rtsp://192.168.178.196:554/live?backchannel=0#tcp`
  - Output Stream: `rtsp://127.0.0.1:8554/test`
  - Result: `ffprobe` successfully reads `h264 (Constrained Baseline), 800x800, yuv420p` without overread or connection reset errors.
- **Two-Way Audio Stream (`test_twoway`):**
  - Ingest URL: `rtsp://192.168.178.196:554/live?backchannel=1#tcp`
  - Output Stream: `rtsp://127.0.0.1:8554/test_twoway`
  - Result: Stream is active and producing frames cleanly through `go2rtc`.

---

## 📝 Key Changes & Fixes Summary

1. **`nal_len >= 1` PPS Check ([rtp.cpp](file:///c:/dev/GitHub/NcStudio.CadServer/ncstudio-app/esphome_rtsp/components/p4_rtsp/rtp.cpp)):**
   Allowed short (≤ 4 Bytes) PPS units from ESP32-P4 hardware encoder to be cached properly.
2. **Server-Level SPS/PPS Cache ([rtsp_server.cpp](file:///c:/dev/GitHub/NcStudio.CadServer/ncstudio-app/esphome_rtsp/components/p4_rtsp/rtsp_server.cpp)):**
   Provides valid `sprop-parameter-sets` in the initial `DESCRIBE` header so H.264 decoders (go2rtc/Frigate) do not fail with `non-existing PPS 0`.
3. **Socket Timeout & Non-blocking Socket Errors:**
   Added `ETIMEDOUT` handling to `recv()` to prevent socket drops on transient delays.
4. **Duplex Backchannel Support:**
   Properly outputs `a=sendrecv` on `?backchannel=1` with G.711 PCMA/PCMU audio decoders.
