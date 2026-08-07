# Opus Audio für ESP32-P4 RTSP Stream — Design

**Datum:** 2026-08-07
**Status:** Freigegeben (Brainstorming)
**Betroffene Dateien:** `diag_full.yaml`, `components/p4_rtsp/rtp.h`, `components/p4_rtsp/rtp.cpp`, `components/p4_rtsp/rtsp_server.cpp`, `go2rtc.yaml`

---

## Problem

Die Mic-Audio-Pipeline (16 kHz PCM) wird aktuell als **PCMA (8 kHz A-law)** über RTSP
gestreamt. Der Downsample-Pfad in `rtp.cpp` (Decimation 16 kHz → 8 kHz ohne Low-Pass)
verursacht Aliasing, das sich als Rauschpegel (~1185 Hz) im Audio manifestiert. Zusätzlich
ist PCMA 8 kHz qualitativ schwach und passt nicht zum modernen WebRTC/RTSP-Ökosystem.

## Ziel

Mic-Audio als **Opus** über RTSP streamen (statt 8 kHz A-law) und den RTSP-Backchannel
ebenfalls auf **Opus** umstellen (statt nur G.711), sodass ein Codec in beide Richtungen
läuft. G.711 bleibt als Fallback für Clients, die kein Opus sprechen.

## Architektur & Datenfluss

```
Mic (16 kHz PCM)
   │
   ▼
OpusPacketizer (neu, rtp.h/rtp.cpp)
   │  PCM16 → Opus-Frame (20 ms), RFC 7587 RTP (Clock 48 kHz)
   ▼
RtspSession::sender_loop → send_track_packet_ (UDP/TCP-interleaved)
   │
   ▼
go2rtc (RTSP, Opus nativ) / ffmpeg / Frigate
```

```
Browser → go2rtc → RTSP-Backchannel (Opus RTP, interleaved)
   │
   ▼
RtspSession::handle_interleaved_ (rtsp_server.cpp)
   │  Opus → PCM (esp_audio_codec decoder, 16 kHz Output)
   ▼
backchannel_callback_ → P4RtspStream::on_backchannel_audio_ → Speaker
```

## Komponenten

### 1. IDF-Dependency: `espressif/esp_audio_codec`

`diag_full.yaml` → Framework `components:` erhält zusätzlich
`espressif/esp_audio_codec` (Version prüfen, ≥2.x). Liefert den Opus-Encoder/Decoder
mit P4-Assembly-Optimierung.

### 2. `OpusPacketizer` (rtp.h / rtp.cpp)

Neue Klasse analog zu `PCMAPacketizer`:

- **Init:** Opus-Encoder, 16 kHz Input, 1 Kanal, `OPUS_APPLICATION_VOIP`,
  Frame-Dauer 20 ms (960 PCM-Samples/Frame). RTP-Clock **48000**.
- **`push_pcm16(data, len)`:** akkumuliert 16 kHz PCM in einem Ring/Chunk-Puffer;
  bei 960 Samples → `opus_encode` → 1 Opus-Paket → 1 RTP-Paket
  (`rtp_write_header`, PT 111, Marker=1, Timestamp += 960).
- **Kein Fragmenting** nötig (Opus-Pakete sind klein genug für den MTU).
- API an `PCMAPacketizer` angelehnt (`set_send_callback`, `set_ssrc`,
  `set_payload_type`, `set_input_sample_rate`, `set_channels`, `push_pcm16`).

### 3. RTSP-Server (rtsp_server.cpp)

- **Member:** `pcma_` → durch `opus_` ersetzt (oder ergänzt). Neuer
  `OpusDecoder`-Wraper (esp_audio_codec) für den Backchannel.
- **`sender_loop`:** `push_pcm16` an den Opus-Packetizer (statt PCMA).
- **`build_sdp_` (backchannel true):**
  ```
  m=audio 0 RTP/AVP 111
  a=control:trackID=1
  a=sendrecv
  a=rtpmap:111 opus/48000/2
  a=fmtp:111 minptime=10;useinbandfec=1
  a=rtpmap:8 PCMA/8000/1
  a=rtpmap:0 PCMU/8000/1
  ```
  ohne Backchannel: `a=sendonly` + nur Opus anbieten (PCMA/PCMU entfallen).
- **`handle_interleaved_`:** PT 111 → Opus-Payload → `opus_decode` → PCM
  (16 kHz Output, mono) → `backchannel_callback_`. G.711-Dekodierung (PT 0/8)
  bleibt als Fallback erhalten.
- **RTP_PT:** neue Konstante `RTP_PT_OPUS = 111` in `rtp.h`.

### 4. Konfiguration

- `diag_full.yaml`: Audio-Sektion unverändert (Mic 16 kHz, Speaker 16 kHz, mono).
  Framework-Components-Liste erweitert um `esp_audio_codec`.
- `go2rtc.yaml`: Stream-URL bleibt `rtsp://192.168.178.196:554/live?backchannel=1`.
  Kein `#audio=pcma`-Override mehr nötig (go2rtc nimmt Opus nativ).

## Fehlerbehandlung

- **Opus-Encoder-Init-Fehler:** Loggen und Audio-Track deaktivieren (fallback wie
  bisher, kein Crash).
- **Opus-Decoder-Fehler** im Backchannel: Frame verwerfen, weiterlaufen.
- **Warteschlange:** bestehende `MAX_AUDIO_QUEUE_BYTES`-Begrenzung beibehalten.
- **Kein Client:** Verhalten unverändert (`always_on` / Client-getriebenes Starten).

## Testing

1. **Build & Flash** auf COM6 (USB) mit aktuellem Environment (ESPHome 2026.7.4,
   IDF 5.5.5).
2. **Stream verifizieren:**
   `ffmpeg -rtsp_transport tcp -i rtsp://192.168.178.196:554/live -t 5 out.mkv`
   → ffprobe zeigt `opus`, 48 kHz Clock, tatsächliche Bandbreite 16 kHz.
3. **Rauschpegel:** `ffmpeg -af volumedetect` → Pegel vs. vorher (PCMA).
4. **go2rtc Ingestion:** Stream in WebUI wählen, `probe` zeigt `audio sendonly OPUS`.
5. **Backchannel (Opus):** Browser-Mikro über WebRTC/Advanced Camera Card → Ton aus
   Speaker. Log zeigt dekodierte Opus-Frames.
6. **Fallback G.711:** Client der PCMU/PCMA sendet → funktioniert weiter.

## Risiken / offene Punkte

- Opus-Encoder auf der CPU (neben 800x800 H.264 HW-Encoder + Hosted-WiFi):
  Last messen; Opus 16 kHz VoIP ist sehr leicht, erwartet unkritisch.
- Frigate-Aufnahme: unterstützt Opus-in-RTSP über ffmpeg (Frigate 0.14+), muss live
  verifiziert werden.
- `esp_audio_codec`-Version: konkrete Pinnung beim Implementieren festlegen.
