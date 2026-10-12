#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "sample_convert.h"

namespace ws {

// Streams a RIFF/WAVE file to disk. The header sizes are rewritten every time
// the internal buffer is flushed, so the file stays playable even if the app
// is killed mid-recording.
class WavWriter {
public:
    // Classic RIFF size fields are 32-bit; callers should start a new file
    // before reaching this many bytes of audio data.
    static constexpr uint64_t kMaxDataBytes = 4000000000ull;

    WavWriter() = default;
    ~WavWriter() { Close(); }
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    // Creates a new file (never overwrites an existing one).
    bool Open(const std::wstring& path, OutputFormat format, uint16_t channels,
              uint32_t sampleRate, uint32_t channelMask);
    bool Write(const void* data, size_t bytes);
    bool WriteSilence(size_t bytes);
    // Flushes, finalizes the header and closes. Returns false if any write
    // failed since Open.
    bool Close();

    bool IsOpen() const { return file_ != INVALID_HANDLE_VALUE; }
    uint64_t DataBytes() const { return dataBytes_; }
    uint16_t BlockAlign() const { return blockAlign_; }
    const std::wstring& Path() const { return path_; }

private:
    bool FlushBuffer();
    bool UpdateHeader();
    bool WriteRaw(const void* data, DWORD bytes);
    bool WriteAt(uint32_t offset, uint32_t value);

    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::wstring path_;
    std::vector<uint8_t> buffer_;
    size_t used_ = 0;
    uint64_t dataBytes_ = 0;
    uint32_t headerSize_ = 0;
    uint32_t dataSizeOffset_ = 0;
    uint32_t factOffset_ = 0;  // 0 when there is no fact chunk
    uint16_t blockAlign_ = 0;
    bool failed_ = false;
};

}  // namespace ws
