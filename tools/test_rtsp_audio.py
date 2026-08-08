import unittest
from tools.rtsp_audio import (apply_volume, resample_linear, samples_to_l16_be,
                              to_mono, RtpPacker, wave_to_pcm)


class TestAudioOps(unittest.TestCase):
    def test_resample_halves_length(self):
        out = resample_linear(list(range(400)), 16000, 8000)
        self.assertEqual(len(out), 200)
        self.assertEqual(out[0], 0)

    def test_resample_doubles_length(self):
        out = resample_linear([0, 32767], 8000, 16000)
        self.assertEqual(len(out), 4)
        self.assertEqual(out[0], 0)
        self.assertEqual(out[-1], 32767)

    def test_to_mono_mixes(self):
        self.assertEqual(to_mono([1000, -1000, 2000, -2000], 2), [0, 0])

    def test_apply_volume(self):
        self.assertEqual(apply_volume([1000, -1000], 0.5), [500, -500])

    def test_l16_be(self):
        self.assertEqual(samples_to_l16_be([0x1234]), b"\x12\x34")

    def test_rtp_packer_header(self):
        p = RtpPacker(pt=97, ssrc=0xDEADBEEF)
        pkt = p.pack(b"\x00\x01")
        self.assertEqual(len(pkt), 12 + 2)
        self.assertEqual(pkt[0] & 0xC0, 0x80)          # version 2, no padding/x/cc
        self.assertEqual(pkt[1] & 0x7F, 97)            # payload type
        self.assertEqual(int.from_bytes(pkt[8:12], "big"), 0xDEADBEEF)
        self.assertEqual(pkt[2:4], (0).to_bytes(2, "big"))           # initial seq
        p2 = p.pack(b"\x00\x01")
        self.assertEqual(p2[2:4], (1).to_bytes(2, "big"))            # seq incremented


class TestWaveToPcm(unittest.TestCase):
    def test_wave_to_pcm_16000(self):
        import struct, tempfile, wave
        with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
            p = f.name
        w = wave.open(p, "wb")
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(struct.pack("<h", 0x1234))
        w.close()
        try:
            rate, data = wave_to_pcm(p, rate=16000, volume=1.0)
            self.assertEqual(rate, 16000)
            self.assertEqual(data, b"\x12\x34")
        finally:
            import os
            os.unlink(p)

    def test_wave_to_pcm_volume_clips(self):
        import os, struct, tempfile, wave
        with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
            p = f.name
        w = wave.open(p, "wb")
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(struct.pack("<h", 20000))
        w.close()
        try:
            _, data = wave_to_pcm(p, rate=16000, volume=2.0)
            self.assertEqual(int.from_bytes(data, "big"), 32767)
        finally:
            os.unlink(p)


if __name__ == "__main__":
    unittest.main()
