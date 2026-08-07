# Projekt-Log: ESP32-P4 RTSP Kamera (p4_rtsp)

> **Letzte Aktualisierung:** 2026-08-07
> **Repo:** `github.com/andreaswatch/esphome_rtsp` (Branch `main`)
> **Device-IP:** 192.168.178.196 · **RTSP:** :554 · **Web:** :80 · **API/OTA:** :3232

---

## 1. Beschreibung der Hardware

- **Board:** Waveshare ESP32-P4 (ETH/NANO-Klasse), aufgebaut mit ES8311-Codec + NS4150B-Verstärker, OV5647 MIPI-CSI Kamera, ESP32-C6 WiFi-Co-Prozessor.
- **Chip:** ESP32-P4 **Rev v1.0 (ECO1)** — via `esptool chip-id` ausgelesen. 2 High-Performance-Cores + LP-Core, 400 MHz.
- **Flash:** 16 MB (in `diag_full.yaml` konfiguriert); **PSRAM:** `hex`, 200 MHz.
- **WiFi:** ESP32-C6 via `esp32_hosted` (SDIO, active_high, slot 1).

### Pinbelegung (verifiziert gegen Waveshare-Datenblatt)
| Funktion | GPIO |
|---|---|
| I2C SDA (ES8311) | GPIO7 |
| I2C SCL (ES8311) | GPIO8 |
| I2S MCLK | GPIO13 |
| I2S BCLK/SCLK | GPIO12 |
| I2S LRCK/WS | GPIO10 |
| I2S ASDOUT → **Mic-In** | GPIO11 |
| I2S DSDIN → **Speaker-Out** | GPIO9 |
| PA_Ctrl (Amplifier Enable, active high) | GPIO53 |
| Kamera XCLK | GPIO40 |
| Kamera Power-Down | GPIO27 |
| Kamera SCCB | SDA=7, SCL=8 |
| MIPI-CSI | 2 lanes, PHY LDO ch3 @ 2500 mV |

### ES8311 Audio-Codec
- **Mic:** analoger DSDIN-Eingang (GPIO9, vom ES8311-ADC). **Wichtig:** `use_microphone: false` — sonst schaltet das Component REG14 BIT6 → PDM-Digital-Mic-Modus → analoge Mic-Samples werden 0.
- **Mic-Gain:** 24 dB (REG16=0x04, Datasheet-Default; vorher 42 dB = 0x07), ADC-Gain REG17=0xC8, PGA 30 dB (REG14=0x1A).
- **Speaker:** DSDIN (GPIO9) → ES8311-DAC → NS4150B Amp → GPIO53 enable.
- **MCLK:** 8 kHz × 256 = 2.048 MHz — exakt in der ES8311-Koeffiziententabelle vorhanden. **Alle vier Raten müssen synchron sein** (`es8311_dac`/`mic`/`speaker`/RTSP-`audio.sample_rate`); die ES8311-Clocking kommt aus `es8311_dac.sample_rate` (`set_sample_frequency`), die I2S-Rate aus `microphone.sample_rate`.

---

## 2. Software-Stack

| Komponente | Version | Hinweis |
|---|---|---|
| ESPHome | 2026.7.4 | Via `py -m pip install --upgrade esphome` |
| PlatformIO Core | 6.1.19 | |
| Platform `espressif32` | 55.3.39 (pioarduino-Fork) | ESPHome empfiehlt für IDF 5.5.5 |
| ESP-IDF | 5.5.5 | Framework `esp-idf`, IDF-Component-Manager |
| `espressif/esp_video` | ^0.8.0 | Kamera/CSI/ISP (OV5647-Treiber) |
| `espressif/esp_h264` | ^1.3.0 | H.264 HW-Encoder |
| `espressif/esp_audio_codec` | **2.5.0 (exakt gepinnt!)** | Opus-Enc/Dec. v2.6+ verlangt P4-Rev ≥3.0 → Build-Fail auf Rev 1.0 |
| Custom Component | `components/p4_rtsp` | Lokale `external_components`-Quelle, `refresh: 0s` |
| go2rtc | 1.9.2 | `go2rtc.yaml`, Windows-Binary in `%TEMP%\opencode\go2rtc` |

### Build-/Flash-Befehle
```powershell
esphome compile diag_full.yaml
esphome upload diag_full.yaml --device COM6      # nur Flash (Build schon da)
esphome run diag_full.yaml --device COM6          # Build + Flash
esphome logs diag_full.yaml --device COM6          # Serielle Logs
```
- Firmware: `.esphome\build\p4-rtsp-cam\build\firmware.factory.bin` (~1,1 MB).
- **Achtung:** Der Build-Ordner kann bei `esp_audio_codec`-Wechsel/Umgebungs-Updates inkonsistent werden (`esp_video_ipa_config.c` wird per Custom-Command generiert). Bei Build-Fehlern: `.esphome\build` löschen oder ESPHome/Platform aktualisieren.

---

## 3. Getroffene Annahmen

1. **Chip-Rev v1.0 (ECO1)** → `esp_audio_codec` auf `2.5.0` gepinnt (v2.6+ hard-failt bei Rev <3.0). Kein Upgrade auf 2.6.x, solange nicht bekannt ist, dass der Chip Rev≥3.0 ist.
2. **Mic ist analog** (DSDIN GPIO9) → `use_microphone: false` in `diag_full.yaml` (verhindert PDM-Modus).
3. **Mic-Samplerate ist 8 kHz** mono (Fix für den wandernden ~1,5-kHz-Ton, s. §7.10); Opus wird mit 8-kHz-Input encodiert, RTP-Clock ist **immer 48 kHz** (RFC 7587; libopus resampled beim Decode intern auf 48 kHz → Frequenzen/Frequenzgang korrekt). Speaker-Pipeline bleibt 8 kHz. **Opus kennt keine 32 kHz** (nur 8/12/16/24/48 kHz) und es gibt keinen Resampler in der Sendekette → Mic-Rate muss der RTSP-Audio-Rate entsprechen.
4. **go2rtc akzeptiert `a=sendrecv` nicht** in der Stream-Engine (matcht nur `recvonly`/`sendonly`). Daher: Audio-Sendetrack = `a=recvonly` (aus go2rtc-Sicht „go2rtc empfängt"), Backchannel-Track = `a=sendonly`.
5. **go2rtc-Source nativ `rtsp://`**, NICHT `ffmpeg:` — der `ffmpeg:`-Prefix ließ ffmpeg PCMA (PT 8) wählen, während die Kamera nur Opus sendet → Audio-Stall.
6. **Session-Limit 2** auf dem Gerät; ein verwaister ffmpeg-Prozess auf dem Host kann eine Session blockieren → „kein Bild/kein Ton".
7. WiFi `Authentication Failed`-Reconnects am Router treten gelegentlich auf; das Gerät verbindet sich selbstständig wieder (kein Code-Problem).
8. **Task-Stacks:** RTSP-Session-Tasks brauchen **32 KB** Stack (Opus SILK-Encode/Decode); 8 KB → Stack-Panic, 16 KB → weiterhin Panic, 32 KB stabil.

---

## 4. Verwendete Codecs

| Richtung | Codec | Details |
|---|---|---|
| Video (send) | **H.264 Constrained Baseline** | 800×800 @ ~35 fps, 2 Mbps, GOP 25, `profile-level-id=42001f` |
| Audio Mic (send) | **Opus** | PT 111, `opus/48000/2`, Encoder 8 kHz mono VoIP 32 kbps, 20-ms-Frames, FEC on, `minptime=10;useinbandfec=1` |
| Audio Speaker (backchannel) | **PCMU / PCMA (G.711)** | PT 0/8, 8 kHz, 1 Kanal; Opus-Decode ebenfalls implementiert (PT 111) |
| Legacy | L16 (PT 97) | Nur noch im Backchannel-Decoder-Pfad vorhanden, wird nicht mehr gesendet |

**Wichtigste Fixes:**
- Opus ersetzt PCMA als Sendecodec → behebt das Aliasing/Rauschen aus der 16k→8k-Decimation.
- SDP kündigt Opus + PCMU/PCMA korrekt an (3 Streams, siehe unten).

---

## 5. Bereitgestellte Streams (RTSP-SDP)

```
m=video 0 RTP/AVP 96          → trackID=0, a=recvonly, H264/90000
m=audio 0 RTP/AVP 111 0 8     → trackID=1, a=recvonly, opus/48000/2 + PCMU/PCMA
m=audio 0 RTP/AVP 0 8         → trackID=2, a=sendonly, PCMU/PCMA (nur mit ?backchannel=1)
```

- `rtsp://192.168.178.196:554/live` → Video + Mic-Audio (Opus).
- `rtsp://192.168.178.196:554/live?backchannel=1` → zusätzlich Backchannel-Track (Speaker).
- **go2rtc:** `rtsp://127.0.0.1:8554/esp32p4_cam` (WebUI `http://127.0.0.1:1984`, WebRTC `:8555`).
- go2rtc-API-Check: `http://127.0.0.1:1984/api/streams?src=esp32p4_cam&video=all&audio=all&microphone`
  → erwartet: `video recvonly H264`, `audio recvonly OPUS`, `audio sendonly PCMU/PCMA` (+ `senders` mit dem Backchannel-Codec).

---

## 6. Verifikation / Debugging

```powershell
# Stream-Codecs prüfen (muss opus/48k zeigen)
ffprobe -rtsp_transport tcp -i "rtsp://192.168.178.196:554/live" -show_streams

# Audio wirklich hören/messen (volumedetect, n_samples > 0)
ffmpeg -rtsp_transport tcp -i "rtsp://127.0.0.1:8554/esp32p4_cam" -map 0:a:0 -t 5 -af volumedetect -f null NUL

# Aufnahme für Offline-Analyse
ffmpeg -rtsp_transport tcp -i "rtsp://127.0.0.1:8554/esp32p4_cam" -t 22 -c:a pcm_s16le out.wav -y

# Chip-Revision
py -m esptool --port COM6 chip-id
```

- **Audio-Analyse (Host):** numpy/scipy-Skripte (FFT, RMS-Envelope, Band-Energien), Whisper (`base`-Modell) für Transkriptionsversuche. Bei sehr leisem Input (−45…−53 dB) halluziniert Whisper — Ergebnisse mit Konfidenz prüfen. Hinweis: die „dBFS“-Werte der Skripte sind relativ zu 1,0 (nicht Full-Scale) — die Vergleichslogik bleibt gültig (echte dBFS ≈ gemessen − 90,3 dB).
- **Audio-Artefakt behoben (2026-08-07):** siehe §7.10 — Ursache war ein **sample-takt-gekoppeltes analoges Signal am Mic-Eingang** (nicht Akustik/Radio/Netz/Kamera), das bei 16 kHz als wandernder ~1,5-kHz-Ton im Sprachband lag. **Fix: 8-kHz-Samplerate** → Ton komplett weg, Sprachband flach (~−21…−24 dB), >4 kHz leer (−49 dB).

---

## 7. Bekannte Probleme & getroffene Fixes (chronologisch)

1. **esp_video_ipa_config.c fehlt (Build-Fail):** SCons erkannte die per Custom-Command generierte Datei nicht. **Fix:** Build-Environment neu aufgesetzt (ESPHome 2026.7.4, IDF 5.5.5, Platform 55.3.39, `.esphome` gelöscht) → lief sauber.
2. **Stack-Overflow bei Opus-Encode:** `rtsp_snd`/`rtsp_ctl` mit 8 KB → Guru Meditation (`silk_encode_frame_FIX`). 16 KB reichten nicht. **Fix:** beide Tasks auf **32768** (`rtsp_server.cpp` `RtspSession::start()`).
3. **Kein Audio über go2rtc (`bytes=0`):** SDP `a=sendrecv` → go2rtc matcht nur `recvonly`/`sendonly`. **Fix:** Audio-Track `a=recvonly`, Backchannel als separater `a=sendonly`-Track.
4. **go2rtc `ffmpeg:`-Source wählte PCMA:** Kamera sendet aber nur Opus → Audio-Stall. **Fix:** nativer `rtsp://…`-Source in `go2rtc.yaml`.
5. **Verwaister ffmpeg-Prozess blockierte Session:** Device-Limit 2 Sessions. **Fix:** Prozess beenden; beim Debuggen nicht mehrere ffmpeg-Probes parallel laufen lassen.
6. **WiFi `Authentication Failed`-Reconnect:** tritt gelegentlich am Router `Zuhause` auf, Gerät verbindet sich selbst wieder. Kein Code-Fix nötig (beobachten).
7. **Speaker-Mute (älter):** ESPHome-`es8311` ruft `set_mute_off()` nicht auf → `P4RtspStream::run_speaker_sequence_()` ruft explizit `set_mute_state(false)` + `set_volume(1.0f)`.
8. **Mic-PDM (älter):** `use_microphone: false` → analoger Pfad statt PDM.
9. **LWIP-Socket-Limit (älter):** `CONFIG_LWIP_MAX_SOCKETS=32` in `components/p4_rtsp/__init__.py`.
10. **Mic-Gain übersteuert (76,5 dB):** PGA 30 dB (REG14) + ADC-Scale 42 dB (REG16) + ADC-Volume +4,5 dB (REG17) verstärkten Raum-/Radio-Audio und einen wandernden ~1,5-kHz-Ton auf sprachüberdeckendes Niveau. **Fix:** `mic_gain: 24DB` (REG16=0x04). PDM ist mit dem analogen Onboard-Mic **nicht** nutzbar (REG14 bit6 liefert Konstante −5504, vgl. `mic_test.wav` und ESPHome-Issue #17695).
11. **Wandernder ~1,5-kHz-Ton (Root Cause):** Durch systematisches Ausschlussverfahren (16.08.: Mic abdecken, Radio an/aus, Batteriebetrieb, Kamera-Pipeline aus, PGA-Analogtest, Sample-Rate-Sweep) als **sample-takt-gekoppeltes analoges Signal am Mic-Eingang** identifiziert:
    - **Skaliert 1:1 mit analogem PGA (REG14):** PGA 30 dB→6 dB → Ton von dominant (−10 dB Band) auf Rauschboden (−43 dB). → Signal liegt VOR dem ADC am MIC-Eingang, kein Codec-internes digitales Artefakt.
    - **Skaliert mit Abtastrate:** 16 kHz → ~1,3–1,6 kHz (wandernd, 0 dB, **im Sprachband** → Maskierung); 24 kHz → ~9,5 kHz (über Sprache); 48 kHz → Töne bei 2,45 kHz (im Sprachband, −2 dB), 7,78 kHz (stabil) und ~10,7 kHz (dominant). 8 kHz → **kein Ton**.
    - **Ausgeschlossen:** Akustik (Mic abgedeckt → unverändert), Radio-RF (an/aus → identisch), Netz/PSU (Batterie → identisch), Kamera/ISP/H.264 (`video:`-Block deaktiviert → identisch). Wahrscheinlicher Mechanismus: Clock-/Board-EMI (MCLK/BCLK/Regler-Burst) koppelt in MIC_P/MIC_N ein; Frequenz wandert mit Temperatur/PLL.
    - **Fix:** alle vier Raten auf **8 kHz** (`es8311_dac`, `microphone`, `speaker`, `p4_rtsp.audio.sample_rate`) → Sprachband sauber, kein Ton, Rauschboden ~−21…−24 dB, >4 kHz leer. Bandbreite = 4 kHz (Telefonqualität, ausreichend für Sprache). Alternative: 24 kHz (12-kHz-Bandbreite, leiser 9,5-kHz-Pfeifton). **48 kHz abgelehnt** (2,45-kHz-Ton im Sprachband).

---

## 8. Offene Punkte / TODO

- **Backchannel praktisch verifizieren:** Browser-Mikro über go2rtc-WebUI/WebRTC → Ton aus Speaker (Pfad ist eingerichtet, `senders: PCMU bytes>0` sollte nach Browser-Interaktion fließen).
- **Ton-Nachbau / Board-EMI:** Der wandernde Ton ist zwar mit 8 kHz behoben, aber die physikalische Quelle (Clock-/Regler-Kopplung in MIC_P/MIC_N) besteht weiter. Optional: MCLK-Multiple 256×→512× bei 16 kHz testen (Ton aus Sprachband schieben), Mic-Eingang schirmen oder DRE/HP-Filter im ES8311.
- **Frigate-Integration testen:** `frigate.yml` liegt bereit; Opus-in-RTSP über ffmpeg (Frigate 0.14+) verifizieren.
- **Board-Rev:** nur Chip-Rev (v1.0) auslesbar; Board-Revision hat kein ID-EEPROM.

---

## 9. Repo-Hygiene

- Component lebt in `components/p4_rtsp/` (lokal, `refresh: 0s`).
- `diag_full.yaml` nutzt `!secret` (WiFi) — sicher committen.
- Design-Spec: `docs/superpowers/specs/2026-08-07-opus-audio-design.md`
- Implementierungsplan: `docs/superpowers/plans/2026-08-07-opus-audio.md`
- Commit-Stil: `git log --oneline -10` folgen; keine Temp-Dateien/venv committen.
