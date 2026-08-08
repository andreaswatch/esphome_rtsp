# RTSP Audio Tools — Design

Datum: 2026-08-09

## Ziel

Zwei Python-Kommandozeilen-Werkzeuge für die ESP32-P4 RTSP-Cam
(`rtsp://<ip>:554/live`):

1. `get_mic.py` — Mic-Audio aus dem RTSP-Stream als WAV-Datei speichern (ffmpeg-Wrapper).
2. `send_audio.py` — eine WAV-Datei an das Gerät senden (Backchannel, pure-Python RTSP-Client).

## Geräte-Hintergrund (aus `components/p4_rtsp`)

- Mic → RTSP: L16 (PT 97), 16 kHz, mono, Big-Endian. SDP: `m=audio ... 97`,
  `a=control:trackID=1`, `a=recvonly`, `a=rtpmap:97 L16/16000/1`.
- Backchannel (`?backchannel=1`): SDP `m=audio ... 0 8`, `a=control:trackID=2`,
  `a=sendonly`, `a=rtpmap:0 PCMU/8000/1`, `a=rtpmap:8 PCMA/8000/1`.
  Der Server-Handler (`rtsp_server.cpp:handle_interleaved_`) akzeptiert im
  Backchannel zusätzlich **L16 (PT 97)** und decodiert es ohne Ratenkonvertierung —
  passt 1:1 auf den 16-kHz-Speaker-Bus. Daher genügt L16, kein G.711 nötig.
- Transport: TCP interleaved (Channels: 0-1 Video, 2-3 Audio, 4-5 Backchannel)
  oder UDP. Wir nutzen TCP interleaved (robust, kein NAT-Problem).
- Session-Limit: max. 2 gleichzeitige RTSP-Sessions; Skripte räumen mit TEARDOWN auf.

## `get_mic.py`

CLI: `python get_mic.py <stream_address> [-s SECONDS=10] [-o OUT.WAV=out.wav]`

- ffmpeg-Aufruf:
  `ffmpeg -hide_banner -loglevel error -rtsp_transport tcp -i <url> -vn -map 0:a:0 -t <s> -ar 16000 -ac 1 -c:a pcm_s16le -y <out>`
- `-vn` deaktiviert das H.264-Decoding; `-map 0:a:0` zieht nur den L16-Audio-Track.
- Ausgabe wird auf 16 kHz / mono / s16 normalisiert (exakt das Geräteformat).
- Fehlerbehandlung:
  - ffmpeg nicht installiert → klarer Hinweis (FileNotFoundError abfangen).
  - ffmpeg-Rückgabecode ≠ 0 → stderr ausgeben, Skript beendet mit Exit-Code 1.
  - `-t 0` oder negativ → ValueError.
- Nach Lauf: Dauer, Zielpfad, Dateigröße ausgeben.

## `send_audio.py`

CLI: `python send_audio.py <stream_address> <wav> [-r|--repeat] [-v VOL=1.0]`

- URL um `?backchannel=1` ergänzen, falls `backchannel` noch nicht im Query steht.
- RTSP-Client (nur stdlib `socket`):
  1. `OPTIONS` (optional, dient der Erreichbarkeitsprüfung)
  2. `DESCRIBE` → SDP parsen, `trackID=2` bestätigen
  3. `SETUP trackID=2` mit `Transport: RTP/AVP/TCP;unicast;interleaved=4-5`
     → Session-ID und Interleave-Kanal merken
  4. `PLAY`
  5. Audio-RTP-Pakete auf Interleave-Kanal 4 senden
  6. `TEARDOWN`
- CSeq hochzählen; Antworten mit erwarteten Statuscodes validieren (200).
- WAV-Eingabe (`wave`-Modul):
  - Format auf 16 kHz / 16-bit / mono normalisieren: lineares Resampling bei
    abweichender Sample-Rate; Stereo → mono mischen; Lautstärke `-v` multipliziert.
  - Little-Endian-PCM → Big-Endian (L16) konvertieren.
- L16-RTP: PT 97, 20-ms-Pakete (320 Samples = 640 Byte), `seq++`, `ts += 320`,
  SSRC zufällig. Senden als `$`-Interleave-Paket (4-Byte-Header + RTP) über den
  bestehenden TCP-Socket.
- `-r|--repeat`: Endlos in Schleife bis Ctrl-C.
- Fehlerbehandlung:
  - Verbindung fehlgeschlagen / Timeout → klare Meldung, Exit 1.
  - RTSP-Antwort nicht 200 → Fehlermeldung mit Status, Exit 1.
  - WAV nicht lesbar / kein PCM → Meldung, Exit 1.
  - `-v` außerhalb 0.0–1.0 → ValueError.

## Nicht-Ziele

- Kein Video (weder Empfang noch Senden).
- Kein G.711-Encoding (Gerät akzeptiert L16 im Backchannel).
- Keine Abhängigkeiten außer stdlib; `get_mic.py` benötigt `ffmpeg` auf dem Rechner.
- Kein UDP-Transport (nur TCP interleaved).

## Tests / Verifikation

- Manuell gegen Gerät `192.168.178.196`:
  - `get_mic.py` erzeugt hörbare 16-kHz-WAV.
  - `send_audio.py` gibt Ton am Speaker aus (Duplex, Mic läuft weiter).
  - Round-Trip: `get_mic.py ... -o t.wav` → `send_audio.py ... t.wav` klingt gleich.
- Reine Logik (Resampler, Mono-Mix, Little→Big-Endian, RTP-Header) ist ohne Gerät
  isoliert prüfbar und wird im Implementierungsplan als Unit-Snippet abgedeckt.
