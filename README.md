# ESP32-P4 RTSP Stream (audio stack + AEC)

Frischer Neuaufbau auf Basis von `esp_audio_stack` + `esp_aec`
(n-IA-hane/esphome-audio-stack) und einem schlanken eigenen `p4_rtsp`-Component.
Der frühere Zustand liegt vollständig in `old/`.

## Ziel

| Feature | Weg |
|---|---|
| Video | OV5647 MIPI-CSI -> H.264 -> RTSP (bewährt, aus altem Stand übernommen) |
| Mic -> RTSP | `esp_audio_stack` liefert Post-AEC 16 kHz s16 mono -> L16-RTP (PT 97) |
| RTSP -> Speaker | Backchannel PCMU/PCMA (PT 0/8) -> G.711-Dekodierung -> `speaker.play()` direkt (voll Duplex) |
| AEC | `esp_aec` (`sr_low_cost`), Software-Reference `previous_frame` |

**RTSP-URL:** `rtsp://<ip>:554/live` (+ `?backchannel=1` für Two-Way-Audio)

## Audio-Stack

- **Bus:** 16 kHz / 16-bit / mono auf dem geteilten ES8311-Bus
  (MCLK=GPIO13, BCLK=GPIO12, LRCLK=GPIO10, DIN=GPIO11 Mic, DOUT=GPIO9 Speaker).
  Bewusst 16 kHz statt 48 kHz: die AEC arbeitet ohnehin bei 16 kHz und der
  Stack-Speaker spielt auf Bus-Rate, damit gibt es **keine** Ratenkonvertierung.
- **Mic zu leise (altes Problem):** `codec.input.gain_db: 37.5` (max analog).
  Optional digitaler Zugewinn via `input_gain` (Default 1.0, max 32.0).
- **Voller Duplex:** kein Mic-stop/Speaker-start-Arbitrieren mehr nötig;
  `play()` kann während laufender Mikrofonaufnahme aufgerufen werden.

## Komponenten-Quellen

```yaml
external_components:
  - source: github://n-IA-hane/esphome-audio-stack@v2026.7.0
    components: [esp_audio_stack, esp_aec]
    refresh: 0s
  - source: components
    components: [p4_rtsp]
    refresh: 0s
```

## Build & Flash

```bash
/tmp/opencode/esphome-venv/bin/esphome compile p4_stream.yaml
# Factory-Image:
#   .esphome/build/p4-rtsp-cam/build/firmware.factory.bin
```

USB-Flash (User): `esptool.py --chip esp32p4 --port /dev/ttyACM0 --baud 921600 write_flash 0x0 firmware.factory.bin`

Gerät: `192.168.178.196` (RTSP 554, Web 80, API/OTA 3232).

## Hardware-Testplan (nach Flash)

1. Boot-Log prüfen: `p4_rtsp component 0.3.0-audio-stack running`, Audio-Stack-Zustände `idle/mic/duplex`.
2. **Speaker:** "Audio Test-Ton"-Button im Web-UI (Port 80) -> Ton hörbar, Mic darf weiter laufen (Duplex).
3. **Mic/AEC:** `ffmpeg -rtsp_transport tcp -i rtsp://192.168.178.196:554/live -map 0:a:0 -t 5 -af volumedetect -f null -` -> nicht mehr -0 dB, hörbarer Pegel.
4. **Echo-Test:** Speaker-Ton abspielen, parallel Mic streamen -> Echo im Stream gedämpft (AEC).
5. **Two-Way:** Frigate/go2rtc Backchannel (PCMA) -> Ton auf Speaker.

## Tuning-Knobs

- Mic-Pegel: `codec.input.gain_db` (0–37.5), `input_gain` (1–32)
- AEC: `mode: sr_low_cost | sr_high_perf | voip_low_cost | voip_high_perf | fd_low_cost | fd_high_perf`
- AEC-Referenz: `aec_reference: previous_frame` (aktuell) -> optional `ring_buffer` (tunable Verzögerung) oder ES8311-Stereo-DAC-Feedback (`use_stereo_aec_reference`, erfordert Board-Unterstützung)
- Kamera: Auflösung/FPS/Bitrate in `p4_rtsp.video`

## Bekannte offene Punkte

- Speaker verzerrt ab ~75% Lautstärke (altes Problem): im neuen Stack über `master_volume`/Codec-Gain steuern; nicht getestet.
- OTA bei ~1 MB Image weiterhin fragil -> USB-Flash.
- go2rtc konsumiert L16 als `s16be` nativ; für WebRTC-Browser-Audio ggf. FFmpeg-Transcode zu opus nötig (siehe Kommentar in `go2rtc.yaml`).
