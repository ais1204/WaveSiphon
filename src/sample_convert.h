#pragma once

// Sample format conversion between what WASAPI delivers and what we write to
// the WAV file. Header-only and free of Windows dependencies so it can be
// unit-tested anywhere.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ws {

// Sample layout of the captured stream.
enum class SampleType { Float32, Int16, Int24, Int32 };

// Sample layout of the WAV file we write. The numeric values are stored in
// the settings file, so keep them stable.
enum class OutputFormat { Pcm16 = 0, Pcm24 = 1, Float32 = 2 };

inline int BytesPerSample(SampleType t) {
    switch (t) {
        case SampleType::Int16: return 2;
        case SampleType::Int24: return 3;
        default: return 4;
    }
}

inline int BytesPerSample(OutputFormat f) {
    switch (f) {
        case OutputFormat::Pcm16: return 2;
        case OutputFormat::Pcm24: return 3;
        default: return 4;
    }
}

// Triangular (TPDF) dither in units of one 16-bit LSB, used only when
// reducing to 16 bits.
class Dither {
public:
    float Next() {
        const float a = static_cast<float>(NextU32() >> 8) * (1.0f / 16777216.0f);
        const float b = static_cast<float>(NextU32() >> 8) * (1.0f / 16777216.0f);
        return a - b;
    }

private:
    uint32_t NextU32() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }
    uint32_t state_ = 0x9E3779B9u;
};

inline float ReadSample(const uint8_t* p, SampleType t) {
    switch (t) {
        case SampleType::Float32: {
            float f;
            std::memcpy(&f, p, 4);
            return (f == f) ? f : 0.0f;  // NaN guard
        }
        case SampleType::Int16: {
            int16_t v;
            std::memcpy(&v, p, 2);
            return static_cast<float>(v) * (1.0f / 32768.0f);
        }
        case SampleType::Int24: {
            int32_t v = static_cast<int32_t>(static_cast<uint32_t>(p[0]) |
                                             (static_cast<uint32_t>(p[1]) << 8) |
                                             (static_cast<uint32_t>(p[2]) << 16));
            v = static_cast<int32_t>(static_cast<uint32_t>(v) << 8) >> 8;  // sign-extend
            return static_cast<float>(v) * (1.0f / 8388608.0f);
        }
        case SampleType::Int32: {
            int32_t v;
            std::memcpy(&v, p, 4);
            return static_cast<float>(static_cast<double>(v) * (1.0 / 2147483648.0));
        }
    }
    return 0.0f;
}

inline void WriteSample(float x, uint8_t* out, OutputFormat f, Dither& dither) {
    switch (f) {
        case OutputFormat::Float32:
            std::memcpy(out, &x, 4);
            break;
        case OutputFormat::Pcm16: {
            float v = x * 32768.0f;
            if (x != 0.0f) v += dither.Next();  // keep digital silence silent
            v = std::floor(v + 0.5f);
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            const int16_t s = static_cast<int16_t>(v);
            std::memcpy(out, &s, 2);
            break;
        }
        case OutputFormat::Pcm24: {
            double v = std::floor(static_cast<double>(x) * 8388608.0 + 0.5);
            if (v > 8388607.0) v = 8388607.0;
            if (v < -8388608.0) v = -8388608.0;
            const int32_t s = static_cast<int32_t>(v);
            out[0] = static_cast<uint8_t>(s & 0xFF);
            out[1] = static_cast<uint8_t>((s >> 8) & 0xFF);
            out[2] = static_cast<uint8_t>((s >> 16) & 0xFF);
            break;
        }
    }
}

// True when the conversion is a plain byte copy (bit-exact).
inline bool IsPassthrough(SampleType src, OutputFormat dst) {
    return (src == SampleType::Float32 && dst == OutputFormat::Float32) ||
           (src == SampleType::Int16 && dst == OutputFormat::Pcm16) ||
           (src == SampleType::Int24 && dst == OutputFormat::Pcm24);
}

// Converts `frames` interleaved frames of `channels` channels from `src` to
// `dst`. Updates peaks[0] / peaks[1] with the absolute peak of the first two
// channels (a mono stream feeds both). Returns the number of bytes written.
inline size_t ConvertFrames(const uint8_t* src, SampleType srcType, uint8_t* dst,
                            OutputFormat dstFmt, size_t frames, int channels,
                            float peaks[2], Dither& dither) {
    const int inSize = BytesPerSample(srcType);
    const int outSize = BytesPerSample(dstFmt);
    const size_t samples = frames * static_cast<size_t>(channels);
    const bool copy = IsPassthrough(srcType, dstFmt);
    if (copy) std::memcpy(dst, src, samples * static_cast<size_t>(inSize));

    for (size_t i = 0; i < samples; ++i) {
        const float x = ReadSample(src + i * inSize, srcType);
        const int ch = static_cast<int>(i % static_cast<size_t>(channels));
        const float a = std::fabs(x);
        if (ch == 0) {
            if (a > peaks[0]) peaks[0] = a;
            if (channels == 1 && a > peaks[1]) peaks[1] = a;
        } else if (ch == 1) {
            if (a > peaks[1]) peaks[1] = a;
        }
        if (!copy) WriteSample(x, dst + i * outSize, dstFmt, dither);
    }
    return samples * static_cast<size_t>(outSize);
}

}  // namespace ws
