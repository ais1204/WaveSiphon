"""Verifies the WAV files written by wav_writer_test.exe."""
import math
import struct
import sys

RATE = 48000
TONE = RATE // 2
SILENCE = RATE // 4


def parse(path):
    data = open(path, "rb").read()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE", "not RIFF/WAVE"
    riff_size = struct.unpack_from("<I", data, 4)[0]
    assert riff_size == len(data) - 8, f"RIFF size {riff_size} != {len(data) - 8}"
    pos, chunks = 12, {}
    while pos < len(data):
        cid, size = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        chunks[cid] = data[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)
    return chunks


def expected(i, c):
    if i >= TONE:
        return 0.0
    v = 0.5 / (1 + c) * math.sin(2 * math.pi * 1000.0 * (1 + c) * i / RATE)
    return -v if c % 2 else v


def check(path, bits, is_float, channels):
    chunks = parse(path)
    fmt = chunks[b"fmt "]
    tag, ch, rate, avg, align, bps = struct.unpack_from("<HHIIHH", fmt)
    if channels > 2:
        assert tag == 0xFFFE and len(fmt) == 40, (tag, len(fmt))
        cb, valid, mask = struct.unpack_from("<HHI", fmt, 16)
        assert cb == 22 and valid == bits and mask == 0x3F
        sub = struct.unpack_from("<I", fmt, 24)[0]
        assert sub == (3 if is_float else 1)
        assert fmt[28:40] == bytes([0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71])
    else:
        assert tag == (3 if is_float else 1), tag
        assert len(fmt) == (18 if is_float else 16)
    assert ch == channels and rate == RATE and bps == bits
    assert align == channels * bits // 8 and avg == RATE * align
    pcm = chunks[b"data"]
    frames = len(pcm) // align
    assert len(pcm) == frames * align
    assert frames == TONE + SILENCE, frames
    if is_float:
        assert struct.unpack("<I", chunks[b"fact"])[0] == frames
    else:
        assert b"fact" not in chunks
    step = bits // 8
    worst = 0.0
    for i in range(frames):
        for c in range(channels):
            off = (i * channels + c) * step
            if is_float:
                v = struct.unpack_from("<f", pcm, off)[0]
            elif bits == 16:
                v = struct.unpack_from("<h", pcm, off)[0] / 32768.0
            else:
                raw = pcm[off] | pcm[off + 1] << 8 | pcm[off + 2] << 16
                if raw & 0x800000:
                    raw -= 1 << 24
                v = raw / 8388608.0
            if i >= TONE:
                assert v == 0.0, f"silence not zero at frame {i}"
            worst = max(worst, abs(v - expected(i, c)))
    limit = {16: 2.0 / 32768, 24: 1.0 / 8388608, 32: 1e-7}[bits]
    assert worst <= limit, f"max error {worst} > {limit}"
    print(f"ok  {path}: {channels}ch {bits}bit{' float' if is_float else ''}, {frames} frames, max err {worst:.2e}")


if __name__ == "__main__":
    d = sys.argv[1]
    check(f"{d}/s16.wav", 16, False, 2)
    check(f"{d}/s24.wav", 24, False, 2)
    check(f"{d}/f32.wav", 32, True, 2)
    check(f"{d}/m24.wav", 24, False, 1)
    check(f"{d}/c6_24.wav", 24, False, 6)
    check(f"{d}/c6_f32.wav", 32, True, 6)
    print("all WAV checks passed")
