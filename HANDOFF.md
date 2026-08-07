# HANDOFF — p4_rtsp (ESP32-P4 RTSP Camera + Audio)

Instructions for the next agent continuing this project. Read this FIRST, then the
relevant source files. Last updated: 2026-08-07 (after commit `96a35cc`).

## Project goal
Custom ESPHome component `p4_rtsp` on an ESP32-P4 (Waveshare P4 Dev Kit, ES8311
audio codec) that streams the OV5647 MIPI-CSI camera as H.264 over RTSP to Frigate,
plus microphone audio (L16) and two-way audio (G.711 backchannel) on the same RTSP
stream. A test-tone button in the ESPHome web UI drives the speaker.

## Current status (what works vs. what is broken)

### Works — verified on hardware
- **Camera video over RTSP**: 800x800 H264 @ ~35 fps, verified with ffmpeg RTSP
  pull (285 frames / 8 s) and in Frigate. Solid.
- **RTSP server** on port 554 with SDP advertising `a=sendrecv`, L16 (PT 97),
  PCMU (PT 0), PCMA (PT 8) and interleaved backchannel decode (G.711 -> PCM,
  x2 upsample to 16 kHz).
- **Test-tone button** in the ESPHome web UI (web_server port 80). Pressing it
  logs `played test tone: 960 samples` with NO bus errors.
- **Shared-I2S-bus arbitration mic/speaker is FIXED** (see "I2S bus arbitration"
  below). The log after pressing the tone shows no `Parent bus is busy`, no
  `microphone did not release`, no `speaker did not start`.

### Broken — the two active problems
1. **Speaker produces NO sound.** The tone is "played" (960 samples written into
   the speaker ring buffer, `finish()` drains cleanly) but nothing is audible.
   This is now a **hardware / ES8311 codec / config issue**, not a bus issue.
2. **Microphone audio is silent.** The L16 audio track is all zeros
   (WAV inspection: Peak 0, RMS 0). Never worked. ES8311 ADC path.
3. **OTA upload is flaky** (binary ~906 KB, fails with
   `receiving update prepare result response: timed out`). Fallback: USB flash.
   The agent cannot access the serial port (`/dev/ttyACM0`, no sudo); the USER
   flashes factory.bin manually and pastes serial logs.

## Environment / how to build & flash
- Repo: `github.com/andreaswatch/esphome_rtsp`, branch `main`, commit `96a35cc`
  (working tree = `/home/andreas/esphome_stream`). Last tag `v0.2.0` predates the
  audio work; create a new tag once speaker+mic audio works.
- ESPHome: `/tmp/opencode/esphome-venv/bin/esphome` (2026.7.4).
- Build: `cd /home/andreas/esphome_stream && /tmp/opencode/esphome-venv/bin/esphome compile diag_full.yaml`
  (local component at `components/p4_rtsp`, `refresh: 0s`).
- Factory image: `.esphome/build/p4-rtsp-cam/build/firmware.factory.bin`; copy to
  `/tmp/opencode/p4-rtsp-cam-factory.bin` and give the user the SHA-256.
- OTA (flaky): `esphome upload --device 192.168.178.196 diag_full.yaml`.
- USB flash (USER does this): `esptool.py --chip esp32p4 --port /dev/ttyACM0 --baud 921600 write_flash 0x0 <factory.bin>`.
- Device: 192.168.178.196. RTSP: 554, web: 80, API/OTA: 3232.
- Logs: user pastes serial output; camera spams `cam_capture: frame #N encoded`
  every 300 frames — filter those out when reading logs.

## Hardware wiring (Waveshare P4 Dev Kit + ES8311 + OV5647)
- ES8311 codec via I2C on GPIO7 (SDA) / GPIO8 (SCL), i2s_bus id `i2c_audio`.
- I2S shared bus: MCLK=GPIO13, LRCLK=GPIO10, BCLK=GPIO12. Mic data in=GPIO9
  (DSDIN), speaker data out=GPIO11 (ASDOUT).
- Amp/speaker power enable: GPIO53 (`switch` `Speaker Enable`, restore ON).
- OV5647 MIPI-CSI 2 lanes, XCLK=GPIO40 (24 MHz), power-down=GPIO27, MIPI PHY LDO
  ch3 @ 2500 mV. `esp_video` 0.8.0 + `esp_h264` 1.3.0 IDF components.
- WiFi via ESP32-C6 hosted (esp32_hosted), PSRAM (hex, 200 MHz).

## Key implementation notes (READ THESE before touching code)

### I2S bus arbitration (FIXED — do not regress)
`p4_rtsp.cpp`:
- `P4RtspStream::speaker_play_()` (p4_rtsp.cpp:135) only copies the PCM into
  `speaker_audio_` and spawns a one-shot FreeRTOS task `spk_play` on core 1
  (`xTaskCreatePinnedToCore`, 8192 B). Guarded by `speaker_busy_` (drops requests
  while a playback is in flight).
- The actual sequence runs in `run_speaker_sequence_()` (p4_rtsp.cpp:153):
  1. set `speaker_active_ = true` (suppresses the always-on mic auto-restart in
     `loop()`/`start_streaming_()`),
  2. `microphone_->stop()`, set `mic_started_ = false`,
  3. poll `microphone_->is_stopped()` (NOT `is_running()`!) up to 1 s,
  4. `speaker_->start()`, poll `is_running()` up to 1 s,
  5. `speaker_->play(...)`, `speaker_->finish()`,
  6. poll `speaker_->is_stopped()` up to 1 s,
  7. `speaker_active_ = false`, restart mic (`mic_started_ = true` BEFORE
     `microphone_->start()` — otherwise loop() double-starts it in the window),
     poll `is_running()` up to 1 s.
- **CRITICAL**: the arbitration MUST NOT run on the ESPHome loop task. The mic's
  own async stop (`state -> STOPPING -> task teardown -> STATE_STOPPED -> unlock`)
  is driven by the mic component's `loop()` which runs on the loop task. Blocking
  the loop task with `vTaskDelay` deadlocks the stop (this was the bug: the button
  handler ran inside `web_server::loop()` on the loop task; mic stayed RUNNING
  forever). Any future synchronous audio call in this component must go through a
  task.
- **`is_running()` vs `is_stopped()`**: the mic's I2S driver holds the shared bus
  lock until `STATE_STOPPED` (task torn down, `parent_->unlock()`). `is_running()`
  turns false already at `STATE_STOPPING` while the lock is still held. Always wait
  on `is_stopped()`.
- ESPHome source to reference (do not edit site-packages; it's a venv):
  `/tmp/opencode/esphome-venv/lib64/python3.12/site-packages/esphome/components/i2s_audio/...`
  (see `i2s_audio_microphone.cpp` state machine, `i2s_audio_speaker*.cpp`).

### Backchannel / two-way audio
- `rtsp_server.cpp` `RtspSession::handle_interleaved_()` decodes L16, PCMU, PCMA
  on the audio interleaved channel and calls `server_->backchannel_callback_`
  which is wired to `P4RtspStream::on_backchannel_audio_()` -> `speaker_play_()`.
- SDP (`build_sdp_()`): `a=sendrecv` + `a=rtpmap:97 L16/16000/1` +
  `a=rtpmap:0 PCMU/8000` + `a=rtpmap:8 PCMA/8000`.
- **Design limitation to revisit**: every backchannel RTP packet (~20–60 ms apart)
  currently triggers a full mic-stop/speaker-start sequence; `speaker_busy_` drops
  most of them, so two-way audio is NOT usable yet. For real two-way you must
  coalesce backchannel frames into bursts (e.g. drain + play accumulated G.711 for
  ~1 s at a time), or accept half-duplex. The test-tone path (single click) is the
  reference.

### Other notable fixes (already in)
- Camera: `esp_cam_sensor_set_format` actually programs the OV5647 register
  (`camera_pipeline.cpp`); H264 length read from `out_frame.length` not
  `raw_data.len` (was sending full DMA buffer capacity per frame); async
  `start_async()` to avoid task-watchdog bootloop; `stop()` releases power-down.
- Use-after-free: `RtspSession::sender_task` sets `sender_done_`; `control_task`
  waits for it before `on_session_closed`.
- Heap: `RtspSession::queue_video_frame` guards with
  `heap_caps_get_largest_free_block` before `assign`; frames use `SpiramAllocator`
  (`spiram_allocator.h`, PSRAM). 

## Next steps for the following agent (in priority order)

### 1. Speaker: no audible sound
The bus path is proven (no errors, clean drain). Investigate:
- **ES8311 DAC config**: `audio_dac: platform es8311` (diag_full.yaml:62) with
  sample_rate 16000 / 16 bit. Check ESPHome `es8311` component for mute/volume
  defaults — the codec may be muted or at zero gain by default. Consider setting
  volume/mute via the `Speaker::set_volume()`/`set_mute_state()` (note the
  component routes volume through `audio_dac`), or checking `i2s_audio_speaker`
  software volume path.
- **Amp enable GPIO53** (`Speaker Enable` switch, restore ON) — confirm the amp
  actually powers up; maybe it needs a delay after boot before the first playback.
- **I2S TX slot / channel config**: speaker is `channel: mono`, 16 bit, 16 kHz.
  Compare against the reference Waveshare config in
  `/tmp/opencode/EspHomeVoipLib/example_esp32p4.yaml` (same hardware, worked
  there). VoipLib used `mclk_multiple: 256` on the bus and `channel: left` for
  the mic — ours has no explicit `mclk_multiple` (check default) and `right`.
- **Playback test that is easier to hear**: the current tone is 60 ms/1 kHz.
  Temporarily play a longer / looping tone (e.g. 1 s sine at full scale) to
  confirm the path, then shorten.
- Check whether the tone data actually reaches the codec: instrument the speaker
  task, or use a logic analyzer on ASDOUT (GPIO11) / LRCLK / BCLK during playback.
- Ask the user to confirm whether the ES8311 has an on-board amp, and measure
  ASDOUT with a scope while the tone plays.

### 2. Microphone: all-zero samples
- Compare with VoipLib: mic `channel: left`, and the same DIN pin (GPIO9).
  Our config uses `channel: right` — TRY `channel: left` first (likely the data
  lands on the other I2S slot).
- Check ES8311 ADC path (micbias, PGA gain, ADC unmute, channel routing) in the
  ESPHome `es8311` component. VoipLib used `mic_gain: 2`.
- Confirm the mic element is actually a working MEMS mic / line input, and that
  the OV5647 camera board's audio isn't the ES8311's line-in that needs enabling.
- Verify with a raw recording before RTSP: record to WAV via a temporary service
  and inspect Peak/RMS; if still zeros, it's the codec/ADC config, not RTSP.

### 3. Two-way audio (after 1+2)
Implement backchannel burst coalescing (see "Design limitation" above), then test
Frigate two-way audio. Frigate will send PCMU/PCMA on the audio channel.

### 4. OTA reliability (nice-to-have)
Investigate the `update prepare` timeout with ~906 KB image. Options: raise
OTA/APDU timeouts, check `esp32_hosted` (C6) throughput, or split the flash
into a base image + OTA-compressed delta. Meanwhile the user flashes via USB.

## Repo hygiene
- Component lives in `components/p4_rtsp/` (local `external_components` source in
  `diag_full.yaml`; refresh 0s so a rebuild picks up edits).
- `diag_full.yaml` uses `!secret` for wifi creds — safe to commit. Update
  `example_esp32p4.yaml`/docs in the repo when audio is done (there is currently
  no example config committed for the new `test_tone` option).
- Commit messages: follow the existing style (`git log --oneline -10`).
- Do NOT commit `/tmp/opencode` files or the venv.
