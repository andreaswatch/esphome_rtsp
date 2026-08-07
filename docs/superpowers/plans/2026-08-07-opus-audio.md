# Opus Audio über RTSP Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Mic-Audio (16 kHz PCM) als Opus über den RTSP-Stream senden und den Backchannel (Browser → go2rtc → Kamera) als Opus empfangen/dekodieren, statt 8 kHz A-law (PCMA).

**Architecture:** Neuer `OpusPacketizer` (PCM16 → Opus-Frame 20 ms → RFC 7587 RTP mit 48 kHz Clock) ersetzt `PCMAPacketizer` im RTSP-Sender. Der Backchannel-Pfad dekodiert Opus (PT 111) über `esp_audio_codec` auf 16 kHz für die Speaker-Pipeline; G.711 (PT 0/8) bleibt als Fallback-Decode erhalten. SDP kündigt Opus als `m=audio ... RTP/AVP 111 0 8` an (`sendrecv` bei backchannel).

**Tech Stack:** ESPHome 2026.7.4, ESP-IDF 5.5.5 (esp32 framework), IDF-Component `espressif/esp_audio_codec` (Opus-Encoder/Decoder, prebuilt `.a`), C++17, FreeRTOS (LWIP Sockets), PlatformIO (pioarduino platform-espressif32 55.3.39).

## Global Constraints

- ESPHome-Version: 2026.7.4 (installiert, via `esphome compile`/`esphome run`).
- ESP-IDF-Framework: `esp-idf` (diag_full.yaml), IDF-Version 5.5.5.
- `esp_audio_codec` VERSION GENAU `2.5.0` pinnen — v2.6+ erzwingt ESP32-P4 Rev ≥ 3.0 und schlägt sonst beim Build fehl. (Opus läuft in 2.5.0 auf allen P4-Revisions als portable libopus, da die ASM-Optimierung nur ein Performance-Bonus ist.)
- Opus-RTP-Clock ist **immer 48000**, unabhängig vom Mic-Sample-Rate (RFC 7587). Timestamp pro 20-ms-Frame = 960.
- Mic-Pipeline bleibt **16 kHz**, mono, 16 bit; Speaker bleibt **16 kHz**, mono.
- Backchannel-Decoder: Opus **und** G.711 (PCMU/PCMA) akzeptieren; beide liefern 16 kHz PCM an die bestehende Speaker-Pipeline.
- Kein Fragmenting nötig: 1 Opus-Paket = 1 RTP-Paket.
- Datei-Endungen: `.cpp`/`.h` im Component `components/p4_rtsp/`; YAML `diag_full.yaml`, `go2rtc.yaml`.
- Kommentar-Stil: kein Dozierkommentar, Konventionen des Repos folgen (siehe vorhandene Dateien).

---

### Task 1: esp_audio_codec-Dependency + RTP-PT-Konstante

**Files:**
- Modify: `diag_full.yaml` (Framework-Components-Liste)
- Modify: `components/p4_rtsp/rtp.h` (Konstante `RTP_PT_OPUS`)

**Interfaces:**
- Produces: `RTP_PT_OPUS` (`uint8_t` = 111), für Task 2/3/4.
- Produces: Framework-Dependency `espressif/esp_audio_codec` → Task 2/4 brauchen `esp_opus_enc.h` / `esp_audio_dec.h` aus dem Component.

- [ ] **Step 1: esp_audio_codec zur YAML-Framework-Config hinzufügen**

Öffne `diag_full.yaml`. Die `esp32:` → `framework:` → `components:` Liste enthält derzeit `espressif/esp_video` und `espressif/esp_h264`. Ergänze `espressif/esp_audio_codec` als dritten Eintrag. Zielzustand des Blocks:

```yaml
  framework:
    type: esp-idf
    components:
      - name: espressif/esp_video
        ref: "^0.8.0"
      - name: espressif/esp_h264
        ref: "^1.3.0"
      - name: espressif/esp_audio_codec
        ref: "2.5.0"
    advanced:
      enable_idf_experimental_features: true
```

- [ ] **Step 2: RTP-PT-Konstante in rtp.h ergänzen**

In `components/p4_rtsp/rtp.h` nach Zeile 17 (`RTP_PT_L16`) einfügen:

```cpp
static constexpr uint8_t RTP_PT_OPUS = 111;
```

- [ ] **Step 3: Dependency-Auflösung verifizieren (Build)**

Run: `esphome compile diag_full.yaml`
Expected: Erfolgreiche Konfiguration; im Build-Log erscheint `espressif__esp_audio_codec` als managed component (Download + Kompilieren der `src/*.c` + Linken der prebuilt `lib/esp32p4/libesp_audio_codec.a`). Abbruch bei Fehler: Meldung prüfen — falls "ESP32-P4 chip version >= 3.0"-Fehler erscheint, wurde versehentlich v2.6.x gezogen → `ref` auf `2.5.0` prüfen (kein `^`).

- [ ] **Step 4: Commit**

```bash
git add diag_full.yaml components/p4_rtsp/rtp.h
git commit -m "feat: add esp_audio_codec dependency and RTP_PT_OPUS constant"
```

---

### Task 2: OpusPacketizer (Encoder) in rtp.h/rtp.cpp

**Files:**
- Modify: `components/p4_rtsp/rtp.h` (Klasse `OpusPacketizer` deklarieren)
- Modify: `components/p4_rtsp/rtp.cpp` (Implementierung + `#include "esp_opus_enc.h"`)

**Interfaces:**
- Consumes: `RTP_PT_OPUS` (Task 1), bestehendes `rtp_write_header`, `RTP_HEADER_SIZE` (rtp.h), `ESP_OPUS_ENC_*` (esp_audio_codec).
- Produces: `class OpusPacketizer` mit:
  - `OpusPacketizer()`, `~OpusPacketizer()`
  - `void set_send_callback(RtpSendCallback callback)`
  - `void set_ssrc(uint32_t ssrc)`
  - `void set_payload_type(uint8_t pt)`
  - `void set_input_sample_rate(int rate)`
  - `void set_channels(int channels)`
  - `void push_pcm16(const uint8_t *data, size_t len)`
  - `uint16_t sequence_number() const { return this->seq_; }`
- Ersetzt `PCMAPacketizer` im Sender (Task 3), nutzt in Task 4 nichts (getrennt).

- [ ] **Step 1: Klassendeklaration in rtp.h**

In `components/p4_rtsp/rtp.h` nach der `PCMAPacketizer`-Klasse (nach Zeile 94) einfügen:

```cpp
// Opus (RFC 7587) packetizer. Accepts raw 16-bit little-endian PCM (as
// delivered by the I2S microphone) and emits Opus frames over RTP with a
// 48 kHz clock. The Opus encoder comes from esp_audio_codec (prebuilt lib).
class OpusPacketizer {
 public:
  OpusPacketizer();
  ~OpusPacketizer();
  void set_send_callback(RtpSendCallback callback);
  void set_ssrc(uint32_t ssrc);
  void set_payload_type(uint8_t pt);
  void set_input_sample_rate(int rate);
  void set_channels(int channels);
  void push_pcm16(const uint8_t *data, size_t len);

  uint16_t sequence_number() const { return this->seq_; }

 protected:
  bool init_encoder_();

  RtpSendCallback send_callback_;
  uint32_t ssrc_{0};
  uint8_t payload_type_{RTP_PT_OPUS};
  uint16_t seq_{0};
  int input_sample_rate_{16000};
  int channels_{1};
  uint32_t timestamp_{0};
  static constexpr uint32_t timestamp_increment_ = 960;  // 20 ms @ 48 kHz

  void *enc_{nullptr};
  size_t frame_bytes_{0};
  std::vector<uint8_t> pcm_buf_;
  std::vector<uint8_t> enc_out_buf_;
};
```

- [ ] **Step 2: Includes in rtp.cpp**

Am Dateianfang von `components/p4_rtsp/rtp.cpp` die Includes ergänzen (nach den vorhandenen Includes):

```cpp
#include "esp_audio_enc.h"
#include "esp_opus_enc.h"
```

Hinweis: `rtp.cpp` hat bereits einen Log-TAG (`static const char *const TAG = "p4_rtsp.rtp";`, Zeile 11). `OpusPacketizer`-Logs (in Step 3) verwenden diesen bestehenden `TAG` — KEINEN neuen `TAG` definieren (wäre ein Redefinition-Fehler).

- [ ] **Step 3: Implementierung am Dateiende (vor `} // namespace p4_rtsp`)**

```cpp
OpusPacketizer::OpusPacketizer() { this->ssrc_ = random_uint32(); }

OpusPacketizer::~OpusPacketizer() {
  if (this->enc_ != nullptr) {
    esp_opus_enc_close(this->enc_);
    this->enc_ = nullptr;
  }
}

void OpusPacketizer::set_send_callback(RtpSendCallback callback) {
  this->send_callback_ = std::move(callback);
}

void OpusPacketizer::set_ssrc(uint32_t ssrc) { this->ssrc_ = ssrc; }

void OpusPacketizer::set_payload_type(uint8_t pt) { this->payload_type_ = pt; }

void OpusPacketizer::set_input_sample_rate(int rate) {
  this->input_sample_rate_ = rate;
}

void OpusPacketizer::set_channels(int channels) { this->channels_ = channels; }

bool OpusPacketizer::init_encoder_() {
  if (this->enc_ != nullptr) {
    return true;
  }
  esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
  cfg.sample_rate = this->input_sample_rate_;
  cfg.channel = this->channels_;
  cfg.bits_per_sample = ESP_AUDIO_BIT16;
  cfg.bitrate = 32000;
  cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
  cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
  cfg.complexity = 2;
  cfg.enable_fec = true;
  cfg.enable_dtx = false;
  cfg.enable_vbr = false;

  esp_audio_err_t err = esp_opus_enc_open(&cfg, sizeof(cfg), &this->enc_);
  if (err != ESP_AUDIO_ERR_OK || this->enc_ == nullptr) {
    ESP_LOGE(TAG, "Opus encoder open failed: %d", err);
    this->enc_ = nullptr;
    return false;
  }
  int in_size = 0;
  int out_size = 0;
  if (esp_opus_enc_get_frame_size(this->enc_, &in_size, &out_size) !=
      ESP_AUDIO_ERR_OK) {
    esp_opus_enc_close(this->enc_);
    this->enc_ = nullptr;
    return false;
  }
  this->frame_bytes_ = static_cast<size_t>(in_size);
  this->enc_out_buf_.resize(static_cast<size_t>(out_size));
  ESP_LOGI(TAG, "Opus encoder ready (frame_in=%d frame_out=%d)", in_size,
           out_size);
  return true;
}

void OpusPacketizer::push_pcm16(const uint8_t *data, size_t len) {
  if (!this->init_encoder_()) {
    return;
  }
  this->pcm_buf_.insert(this->pcm_buf_.end(), data, data + len);
  while (this->pcm_buf_.size() >= this->frame_bytes_) {
    esp_audio_enc_in_frame_t in_frame;
    in_frame.buffer = this->pcm_buf_.data();
    in_frame.len = static_cast<uint32_t>(this->frame_bytes_);
    esp_audio_enc_out_frame_t out_frame;
    out_frame.buffer = this->enc_out_buf_.data();
    out_frame.len = static_cast<uint32_t>(this->enc_out_buf_.size());
    esp_audio_err_t err =
        esp_opus_enc_process(this->enc_, &in_frame, &out_frame);
    this->pcm_buf_.erase(this->pcm_buf_.begin(),
                         this->pcm_buf_.begin() + this->frame_bytes_);
    if (err != ESP_AUDIO_ERR_OK || out_frame.encoded_bytes == 0) {
      ESP_LOGW(TAG, "Opus encode failed: %d", err);
      continue;
    }
    std::vector<uint8_t> pkt(RTP_HEADER_SIZE + out_frame.encoded_bytes);
    rtp_write_header(pkt.data(), true, this->payload_type_, this->seq_,
                     this->timestamp_, this->ssrc_);
    memcpy(pkt.data() + RTP_HEADER_SIZE, this->enc_out_buf_.data(),
           out_frame.encoded_bytes);
    this->seq_++;
    this->timestamp_ += timestamp_increment_;
    if (this->send_callback_) {
      this->send_callback_(pkt.data(), pkt.size());
    }
  }
}
```

Hinweis: `random_uint32()` und `ESP_LOGE/LOGI/LOGW` kommen aus den bereits von `rtp.cpp` bzw. dem Parent-Component geerbten ESPHome-Headern (identische Nutzung wie in `PCMAPacketizer`). Falls `memcpy` fehlt, `#include <cstring>` ergänzen.

- [ ] **Step 4: Build-Verifikation (inkrementell)**

Run: `esphome compile diag_full.yaml`
Expected: kompiliert durch; im Log erscheint die Opus-Encoder-Init-Logzeile erst zur Laufzeit. Bei Compilerfehlern: Include-Pfade prüfen — `esp_opus_enc.h` wird von `esp_audio_codec` bereitgestellt und ist über die Component-INCLUDE_DIRS sichtbar.

- [ ] **Step 5: Commit**

```bash
git add components/p4_rtsp/rtp.h components/p4_rtsp/rtp.cpp
git commit -m "feat: add Opus RTP packetizer (RFC 7587) via esp_audio_codec"
```

---

### Task 3: RtspSession auf Opus-Sender umstellen

**Files:**
- Modify: `components/p4_rtsp/rtsp_server.h` (Member `opus_`, entferne `pcma_`)
- Modify: `components/p4_rtsp/rtsp_server.cpp` (Ctor, `sender_loop`, `build_sdp_`)

**Interfaces:**
- Consumes: `OpusPacketizer` (Task 2), `RTP_PT_OPUS` (Task 1).
- Produces: Audio-Track sendet Opus (PT 111); SDP kündigt Opus + PCMU/PCMA an; kein `PCMAPacketizer`-Member mehr.

- [ ] **Step 1: Member in rtsp_server.h austauschen**

In `components/p4_rtsp/rtsp_server.h` die Zeile 158 (`PCMAPacketizer pcma_;`) ersetzen:

```cpp
  OpusPacketizer opus_;
```

- [ ] **Step 2: Konstruktor anpassen (rtsp_server.cpp)**

In `rtsp_server.cpp` im Konstruktor `RtspSession::RtspSession` die `pcma_`-Initialisierung (Zeilen 239-245) ersetzen durch:

```cpp
  this->opus_.set_ssrc(random_uint32());
  this->opus_.set_payload_type(RTP_PT_OPUS);
  this->opus_.set_input_sample_rate(this->server_->audio_sample_rate());
  this->opus_.set_channels(this->server_->audio_channels());
  this->opus_.set_send_callback([this](const uint8_t *data, size_t len) {
    this->send_track_packet_(this->audio_track_, data, len);
  });
```

- [ ] **Step 3: sender_loop auf Opus umstellen (rtsp_server.cpp)**

In `RtspSession::sender_loop()` die Zeile 372 (`this->pcma_.push_pcm16(...)`) ersetzen:

```cpp
        this->opus_.push_pcm16(audio_chunk.data(), audio_chunk.size());
```

- [ ] **Step 4: SDP anpassen (build_sdp_)**

In `RtspSession::build_sdp_(bool backchannel)` den Audio-Block (Zeilen 780-786) ersetzen durch:

```cpp
  sdp += "m=audio 0 RTP/AVP 111 0 8\r\n";
  sdp += "a=control:trackID=1\r\n";
  sdp += "a=" + std::string(backchannel ? "sendrecv" : "sendonly") + "\r\n";
  sdp += "a=rtpmap:111 opus/48000/2\r\n";
  sdp += "a=fmtp:111 minptime=10;useinbandfec=1\r\n";
  if (backchannel) {
    sdp += "a=rtpmap:0 PCMU/8000/1\r\n";
    sdp += "a=rtpmap:8 PCMA/8000/1\r\n";
  }
```

Hinweis: Die `m=`-Zeile führt drei Payload-Types (111, 0, 8). Clients wählen; go2rtc bevorzugt Opus (111). `opus/48000/2` ist RFC-7587-Pflicht (mono-Daten bleiben mono).

- [ ] **Step 5: Build-Verifikation**

Run: `esphome compile diag_full.yaml`
Expected: kompiliert durch. Auf tote Verweise auf `pcma_` prüfen: `Select-String -Path "components\p4_rtsp\*.cpp","components\p4_rtsp\*.h" -Pattern "pcma_"` → nur Treffer in Kommentaren erlaubt.

- [ ] **Step 6: Commit**

```bash
git add components/p4_rtsp/rtsp_server.h components/p4_rtsp/rtsp_server.cpp
git commit -m "feat: send Opus audio track in RTSP, advertise opus/pcmu/pcma in SDP"
```

---

### Task 4: Opus-Backchannel-Decoder (handle_interleaved_)

**Files:**
- Modify: `components/p4_rtsp/rtsp_server.h` (Member `opus_dec_`, `opus_pcm_buf_`)
- Modify: `components/p4_rtsp/rtsp_server.cpp` (Includes, Destruktor, `handle_interleaved_`)

**Interfaces:**
- Consumes: `esp_opus_dec_*` (esp_audio_codec), `ESP_AUDIO_SAMPLE_RATE_16K`, `ESP_AUDIO_MONO` (esp_audio_types.h).
- Produces: PT 111 im interleaved-Backchannel wird zu 16 kHz PCM dekodiert und an `backchannel_callback_` übergeben (identisch zum bestehenden G.711-Pfad).

- [ ] **Step 1: Includes in rtsp_server.cpp**

Am Dateianfang von `components/p4_rtsp/rtsp_server.cpp` (bei den anderen Includes) ergänzen:

```cpp
#include "esp_audio_dec.h"
#include "esp_opus_dec.h"
```

- [ ] **Step 2: Member in rtsp_server.h ergänzen**

In `components/p4_rtsp/rtsp_server.h` nach Zeile 158 (`OpusPacketizer opus_;`) einfügen:

```cpp
  void *opus_dec_{nullptr};
  std::vector<uint8_t> opus_pcm_buf_;
```

- [ ] **Step 3: Decoder im Destruktor freigeben**

In `RtspSession::~RtspSession()` (rtsp_server.cpp, nach den socket-`close`-Aufrufen, vor dem schließenden `}`) ergänzen:

```cpp
  if (this->opus_dec_ != nullptr) {
    esp_opus_dec_close(this->opus_dec_);
    this->opus_dec_ = nullptr;
  }
```

- [ ] **Step 4: Opus-Zweig in handle_interleaved_**

In `RtspSession::handle_interleaved_()` den `else`-Block der PT-Abfrage erweitern. Der aktuelle Code (Zeilen 478-501) endet mit `} else { return; }`. Ersetze den gesamten Block `if (pt == RTP_PT_L16) { ... } else if (pt == RTP_PT_PCMU || pt == RTP_PT_PCMA) { ... } else { return; }` durch:

```cpp
  std::vector<int16_t> decoded;
  if (pt == RTP_PT_L16) {
    // L16: big-endian 16-bit PCM.
    size_t samples = payload_len / 2;
    decoded.resize(samples);
    for (size_t i = 0; i < samples; i++) {
      uint16_t v =
          static_cast<uint16_t>((payload[i * 2] << 8) | payload[i * 2 + 1]);
      decoded[i] = static_cast<int16_t>(v);
    }
  } else if (pt == RTP_PT_PCMU || pt == RTP_PT_PCMA) {
    // Frigate and most NVRs send two-way audio as G.711 (µ-law / A-law) at
    // 8 kHz. Decode to 16-bit PCM and upsample x2 to the 16 kHz speaker.
    bool mulaw = (pt == RTP_PT_PCMU);
    decoded.reserve(payload_len * 2);
    for (size_t i = 0; i < payload_len; i++) {
      uint8_t u = payload[i];
      int32_t s = mulaw ? static_cast<int32_t>(mulaw_decode(u))
                        : static_cast<int32_t>(alaw_decode(u));
      decoded.push_back(static_cast<int16_t>(s));
      decoded.push_back(static_cast<int16_t>(s));
    }
  } else if (pt == RTP_PT_OPUS) {
    if (this->opus_dec_ == nullptr) {
      esp_opus_dec_cfg_t dcfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
      dcfg.sample_rate = ESP_AUDIO_SAMPLE_RATE_16K;
      dcfg.channel = ESP_AUDIO_MONO;
      dcfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_INVALID;
      dcfg.self_delimited = false;
      esp_audio_err_t err =
          esp_opus_dec_open(&dcfg, sizeof(dcfg), &this->opus_dec_);
      if (err != ESP_AUDIO_ERR_OK || this->opus_dec_ == nullptr) {
        ESP_LOGE(TAG, "Opus decoder open failed: %d", err);
        this->opus_dec_ = nullptr;
        return;
      }
      this->opus_pcm_buf_.resize(4096);
    }
    esp_audio_dec_in_raw_t raw;
    raw.buffer = const_cast<uint8_t *>(payload);
    raw.len = static_cast<uint32_t>(payload_len);
    esp_audio_dec_out_frame_t out;
    out.buffer = this->opus_pcm_buf_.data();
    out.len = static_cast<uint32_t>(this->opus_pcm_buf_.size());
    esp_audio_dec_info_t info;
    esp_audio_err_t err = esp_opus_dec_decode(this->opus_dec_, &raw, &out, &info);
    if (err == ESP_AUDIO_ERR_OK && out.decoded_size > 0) {
      size_t samples = out.decoded_size / sizeof(int16_t);
      decoded.assign(reinterpret_cast<int16_t *>(out.buffer),
                     reinterpret_cast<int16_t *>(out.buffer) + samples);
    } else {
      return;
    }
  } else {
    return;
  }
```

Der nachfolgende Aufruf `if (this->server_->backchannel_callback_) { ... }` bleibt unverändert.

- [ ] **Step 5: Build-Verifikation**

Run: `esphome compile diag_full.yaml`
Expected: kompiliert durch. Achtung: `esp_opus_dec_open` erwartet `void *cfg` — Übergabe von `&dcfg` ist korrekt.

- [ ] **Step 6: Commit**

```bash
git add components/p4_rtsp/rtsp_server.h components/p4_rtsp/rtsp_server.cpp
git commit -m "feat: decode Opus backchannel audio to 16kHz PCM for speaker"
```

---

### Task 5: go2rtc.yaml aufräumen + Hardware-Verifikation

**Files:**
- Modify: `go2rtc.yaml` (Stream-URL-Aufruf)

**Interfaces:**
- Consumes: Kompletter Opus-Stream aus Task 1-4.
- Produces: Keine — Verifikation.

- [ ] **Step 1: go2rtc.yaml vereinfachen**

`go2rtc.yaml` (11 Zeilen) — die Stream-URL behält `?backchannel=1`, aber der explizite Codec-Override `#audio=pcma` entfällt (go2rtc nimmt Opus nativ). Zielzustand:

```yaml
streams:
  esp32p4_cam:
    - ffmpeg:rtsp://192.168.178.196:554/live?backchannel=1
api:
  listen: :1984
rtsp:
  listen: :8554
webrtc:
  listen: :8555
log:
  level: info
```

- [ ] **Step 2: Firmware flashen**

Run: `esphome run diag_full.yaml --device COM6`
Expected: Build erfolgreich, Upload auf COM6, "Successfully uploaded program."

- [ ] **Step 3: Stream per ffmpeg verifizieren**

Run (auf dem Host, IP 192.168.178.196):
`ffmpeg -rtsp_transport tcp -i "rtsp://192.168.178.196:554/live?backchannel=1" -map 0:a:0 -t 5 -af volumedetect -f null NUL`
Expected: ffmpeg meldet Audio-Track; `mean_volume`/`max_volume` nicht mehr alle Null. Anschließend:
`ffprobe -rtsp_transport tcp -i "rtsp://192.168.178.196:554/live?backchannel=1" -show_streams`
Expected: `codec_name=opus` (oder `libopus`), `sample_rate=48000` (RTP-Clock), Mono deklariert als 2 Kanäle laut RFC.
Fehlerfall: Wenn ffmpeg `non-existing PPS` oder `unknown codec` meldet → SDP/PT prüfen; wenn Audio fehlt → `build_sdp_` und `sender_loop` prüfen.

- [ ] **Step 4: go2rtc Ingestion testen**

go2rtc mit der YAML starten, im WebUI (Port 1984) den Stream wählen und `probe` ausführen.
Expected: `audio sendonly OPUS` (bzw. OPUS + PCMU/PCMA sendonly). Fehlerfall: Logs von go2rtc (`log.level: info`) zeigen, welcher Codec ausgehandelt wurde.

- [ ] **Step 5: Backchannel testen**

Über go2rtc WebUI / Advanced Camera Card Browser-Mikro aktivieren und sprechen.
Expected: Ton aus dem Speaker; Gerätelog zeigt dekodierte Opus-Frames (oder keine Fehler im Opus-Decoder-Zweig). Fehlerfall: Opus-Decoder-Logzeilen (`p4_rtsp.opus` nicht vorhanden — `TAG` ist `p4_rtsp.server`; dort erscheinen `Opus decoder open failed` bei Init-Fehlern) prüfen.

- [ ] **Step 6: Commit**

```bash
git add go2rtc.yaml
git commit -m "chore: drop pcma override from go2rtc, use native opus"
```

---

## Self-Review

**Spec-Coverage:**
- ✅ `diag_full.yaml` → esp_audio_codec (Task 1)
- ✅ OpusPacketizer Encoder 16 kHz / 20 ms / VoIP / RFC 7587 / Clock 48 kHz (Task 2)
- ✅ SDP opus/48000/2 + fmtp (Task 3)
- ✅ Backchannel Opus-Decoder 16 kHz Output + G.711-Fallback (Task 4)
- ✅ go2rtc.yaml ohne `#audio=pcma` (Task 5)
- ✅ Mic/Speaker bleiben 16 kHz — kein YAML-Audio-Teil geändert

**Placeholder-Check:** Alle Schritte enthalten vollständigen Code bzw. exakte Kommandos. Keine TBD/TODO.

**Typ-Konsistenz:** `RTP_PT_OPUS=111` in Task 1 definiert, in 2/3/4 verwendet. `OpusPacketizer`-Signatur aus Task 2 wird in Task 3 exakt so genutzt (`set_ssrc`, `set_payload_type`, `set_input_sample_rate`, `set_channels`, `set_send_callback`, `push_pcm16`). `opus_dec_`/`opus_pcm_buf_` aus Task 4 im Header von Task 3 angelegt (Task 4 erweitert denselben Header). Kein verbleibender `pcma_`-Referenz (Task 3 Step 5 prüft).
