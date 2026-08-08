import math
import random
import struct
import wave


def resample_linear(samples, src_rate, dst_rate):
    if src_rate == dst_rate:
        return list(samples)
    out_len = int(len(samples) * dst_rate / src_rate)
    out = []
    for i in range(out_len):
        pos = i * src_rate / dst_rate
        lo = int(pos)
        hi = min(lo + 1, len(samples) - 1)
        frac = pos - lo
        a = samples[lo]
        b = samples[hi]
        out.append(int(a + (b - a) * frac))
    return out


def to_mono(samples, channels):
    if channels == 1:
        return list(samples)
    mono = []
    for i in range(0, len(samples) - channels + 1, channels):
        mono.append(sum(samples[i:i + channels]) // channels)
    return mono


def apply_volume(samples, volume):
    return [int(s * volume) for s in samples]


def samples_to_l16_be(samples):
    out = bytearray()
    for s in samples:
        s = max(-32768, min(32767, s))
        out += struct.pack(">h", s)
    return bytes(out)


class RtpPacker:
    def __init__(self, pt=97, ssrc=None):
        self.pt = pt & 0x7F
        self.ssrc = random.getrandbits(32) if ssrc is None else ssrc & 0xFFFFFFFF
        self.seq = 0
        self.ts = 0

    def pack(self, chunk):
        header = bytearray(12)
        header[0] = 0x80  # v=2
        header[1] = self.pt
        header[2:4] = self.seq.to_bytes(2, "big")
        header[4:8] = self.ts.to_bytes(4, "big")
        header[8:12] = self.ssrc.to_bytes(4, "big")
        self.seq = (self.seq + 1) & 0xFFFF
        self.ts += len(chunk) // 2
        return bytes(header) + chunk


def wave_to_pcm(path, rate=16000, volume=1.0):
    with wave.open(path, "rb") as w:
        if w.getcomptype() != "NONE":
            raise ValueError("WAV must be uncompressed PCM")
        channels = w.getnchannels()
        sw = w.getsampwidth()
        src_rate = w.getframerate()
        frames = w.readframes(w.getnframes())
    if sw != 2:
        raise ValueError("WAV must be 16-bit PCM")
    samples = list(struct.unpack("<%dh" % (len(frames) // 2), frames))
    samples = to_mono(samples, channels)
    samples = resample_linear(samples, src_rate, rate)
    samples = apply_volume(samples, volume)
    return rate, samples_to_l16_be(samples)
