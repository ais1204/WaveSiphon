// Writes reference WAV files with WavWriter + ConvertFrames so that
// tests/check_wav.py can verify headers and sample values.
// Usage: wav_writer_test.exe <output-dir>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/sample_convert.h"
#include "../src/wav_writer.h"

using namespace ws;

static bool WriteCase(const std::wstring& path, OutputFormat format, int channels) {
    const uint32_t rate = 48000;
    const size_t frames = rate / 2;  // 0.5 s of tone, then 0.25 s of silence
    std::vector<float> src(frames * channels);
    for (size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        for (int c = 0; c < channels; ++c) {
            // channel c: sine at (1 + c) kHz, amplitude 0.5 / (1 + c), odd channels inverted
            const double v = 0.5 / (1 + c) * std::sin(2 * 3.14159265358979 * 1000.0 * (1 + c) * t);
            src[i * channels + c] = static_cast<float>(c % 2 ? -v : v);
        }
    }
    WavWriter writer;
    const uint32_t mask = channels == 6 ? 0x3F : (channels == 2 ? 0x3 : 0x4);
    if (!writer.Open(path, format, static_cast<uint16_t>(channels), rate, mask)) return false;
    Dither dither;
    std::vector<uint8_t> out(frames * channels * 4);
    // Feed in uneven chunks like WASAPI packets.
    size_t done = 0;
    size_t chunk = 441;
    float peaks[2] = {0, 0};
    while (done < frames) {
        const size_t n = std::min(chunk, frames - done);
        const size_t bytes = ConvertFrames(reinterpret_cast<const uint8_t*>(&src[done * channels]),
                                           SampleType::Float32, out.data(), format, n, channels, peaks, dither);
        if (!writer.Write(out.data(), bytes)) return false;
        done += n;
        chunk = chunk == 441 ? 480 : 441;
    }
    if (!writer.WriteSilence(static_cast<size_t>(rate / 4) * writer.BlockAlign())) return false;
    std::printf("peaks %.4f %.4f\n", peaks[0], peaks[1]);
    return writer.Close();
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    const std::wstring dir = argv[1];
    struct Case { const wchar_t* name; OutputFormat format; int channels; };
    const Case cases[] = {
        {L"\\s16.wav", OutputFormat::Pcm16, 2},
        {L"\\s24.wav", OutputFormat::Pcm24, 2},
        {L"\\f32.wav", OutputFormat::Float32, 2},
        {L"\\m24.wav", OutputFormat::Pcm24, 1},
        {L"\\c6_24.wav", OutputFormat::Pcm24, 6},
        {L"\\c6_f32.wav", OutputFormat::Float32, 6},
    };
    int failures = 0;
    for (const Case& c : cases) {
        const std::wstring path = dir + c.name;
        DeleteFileW(path.c_str());
        if (!WriteCase(path, c.format, c.channels)) {
            std::wprintf(L"FAILED %ls\n", c.name);
            ++failures;
        }
    }
    // Opening an existing file must fail (never overwrite).
    WavWriter again;
    if (again.Open(dir + L"\\s16.wav", OutputFormat::Pcm16, 2, 48000, 3)) {
        std::printf("FAILED: overwrote an existing file\n");
        ++failures;
    }
    // Int16 -> Pcm16 and Int24 -> Pcm24 must be bit-exact copies.
    {
        const int16_t in[4] = {-32768, -1, 1, 32767};
        uint8_t outBytes[8];
        float peaks[2] = {0, 0};
        Dither d;
        ConvertFrames(reinterpret_cast<const uint8_t*>(in), SampleType::Int16, outBytes, OutputFormat::Pcm16, 2, 2, peaks, d);
        if (std::memcmp(in, outBytes, 8) != 0) { std::printf("FAILED: int16 passthrough\n"); ++failures; }
        // Digital silence stays exactly zero even with dither.
        const float zeros[4] = {0, 0, 0, 0};
        int16_t z[4] = {1, 1, 1, 1};
        ConvertFrames(reinterpret_cast<const uint8_t*>(zeros), SampleType::Float32, reinterpret_cast<uint8_t*>(z), OutputFormat::Pcm16, 2, 2, peaks, d);
        if (z[0] || z[1] || z[2] || z[3]) { std::printf("FAILED: silence not silent\n"); ++failures; }
        // Clipping.
        const float loud[2] = {1.5f, -1.5f};
        uint8_t o24[6];
        ConvertFrames(reinterpret_cast<const uint8_t*>(loud), SampleType::Float32, o24, OutputFormat::Pcm24, 1, 2, peaks, d);
        const int32_t a = (o24[0] | o24[1] << 8 | o24[2] << 16), b = (o24[3] | o24[4] << 8 | o24[5] << 16);
        if (a != 0x7FFFFF || b != 0x800000) { std::printf("FAILED: 24-bit clipping %x %x\n", a, b); ++failures; }
        if (std::fabs(peaks[0] - 1.5f) > 1e-6 || std::fabs(peaks[1] - 1.5f) > 1e-6) { std::printf("FAILED: peaks\n"); ++failures; }
    }
    std::printf(failures ? "FAIL (%d)\n" : "OK\n", failures);
    return failures ? 1 : 0;
}
