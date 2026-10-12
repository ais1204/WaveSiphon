#include "wav_writer.h"

#include <algorithm>

namespace ws {

namespace {

constexpr size_t kBufferSize = 1 << 20;  // 1 MiB

void Put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}

void Put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

void PutTag(std::vector<uint8_t>& v, const char* tag) { v.insert(v.end(), tag, tag + 4); }

// KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT share everything but the first field.
void PutSubFormat(std::vector<uint8_t>& v, uint16_t formatTag) {
    static const uint8_t kTail[] = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
                                    0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    Put32(v, formatTag);
    v.insert(v.end(), kTail, kTail + sizeof(kTail));
}

}  // namespace

bool WavWriter::Open(const std::wstring& path, OutputFormat format, uint16_t channels,
                     uint32_t sampleRate, uint32_t channelMask) {
    Close();

    const bool isFloat = format == OutputFormat::Float32;
    const uint16_t bits = static_cast<uint16_t>(BytesPerSample(format) * 8);
    // WAVE_FORMAT_EXTENSIBLE is required for more than two channels.
    const bool extensible = channels > 2;
    blockAlign_ = static_cast<uint16_t>(channels * (bits / 8));

    std::vector<uint8_t> h;
    PutTag(h, "RIFF");
    Put32(h, 0);
    PutTag(h, "WAVE");

    PutTag(h, "fmt ");
    if (extensible) {
        Put32(h, 40);
        Put16(h, 0xFFFE);
    } else if (isFloat) {
        Put32(h, 18);
        Put16(h, 3);  // WAVE_FORMAT_IEEE_FLOAT
    } else {
        Put32(h, 16);
        Put16(h, 1);  // WAVE_FORMAT_PCM
    }
    Put16(h, channels);
    Put32(h, sampleRate);
    Put32(h, sampleRate * blockAlign_);
    Put16(h, blockAlign_);
    Put16(h, bits);
    if (extensible) {
        Put16(h, 22);
        Put16(h, bits);
        Put32(h, channelMask);
        PutSubFormat(h, isFloat ? 3 : 1);
    } else if (isFloat) {
        Put16(h, 0);
    }

    factOffset_ = 0;
    if (isFloat) {  // non-PCM formats should carry a fact chunk
        PutTag(h, "fact");
        Put32(h, 4);
        factOffset_ = static_cast<uint32_t>(h.size());
        Put32(h, 0);
    }

    PutTag(h, "data");
    dataSizeOffset_ = static_cast<uint32_t>(h.size());
    Put32(h, 0);
    headerSize_ = static_cast<uint32_t>(h.size());

    file_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) return false;

    path_ = path;
    dataBytes_ = 0;
    used_ = 0;
    failed_ = false;
    buffer_.resize(kBufferSize);
    if (!WriteRaw(h.data(), static_cast<DWORD>(h.size()))) {
        Close();
        return false;
    }
    return true;
}

bool WavWriter::Write(const void* data, size_t bytes) {
    if (!IsOpen() || failed_) return false;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    dataBytes_ += bytes;
    while (bytes > 0) {
        if (used_ == buffer_.size() && !FlushBuffer()) return false;
        const size_t n = std::min(bytes, buffer_.size() - used_);
        std::memcpy(buffer_.data() + used_, p, n);
        used_ += n;
        p += n;
        bytes -= n;
    }
    return true;
}

bool WavWriter::WriteSilence(size_t bytes) {
    if (!IsOpen() || failed_) return false;
    dataBytes_ += bytes;
    while (bytes > 0) {
        if (used_ == buffer_.size() && !FlushBuffer()) return false;
        const size_t n = std::min(bytes, buffer_.size() - used_);
        std::memset(buffer_.data() + used_, 0, n);
        used_ += n;
        bytes -= n;
    }
    return true;
}

bool WavWriter::Close() {
    if (!IsOpen()) return true;
    bool ok = FlushBuffer();
    if (ok && (dataBytes_ & 1)) {  // RIFF chunks are word aligned
        const uint8_t pad = 0;
        ok = WriteRaw(&pad, 1);
    }
    ok = UpdateHeader() && ok;
    CloseHandle(file_);
    file_ = INVALID_HANDLE_VALUE;
    buffer_.clear();
    buffer_.shrink_to_fit();
    return ok && !failed_;
}

bool WavWriter::FlushBuffer() {
    if (used_ == 0) return !failed_;
    const bool ok = WriteRaw(buffer_.data(), static_cast<DWORD>(used_));
    used_ = 0;
    return ok && UpdateHeader();
}

bool WavWriter::UpdateHeader() {
    const uint64_t pad = dataBytes_ & 1;
    const uint64_t riff = std::min<uint64_t>(headerSize_ - 8 + dataBytes_ + pad, 0xFFFFFFFFull);
    const uint64_t data = std::min<uint64_t>(dataBytes_, 0xFFFFFFFFull);
    bool ok = WriteAt(4, static_cast<uint32_t>(riff)) &&
              WriteAt(dataSizeOffset_, static_cast<uint32_t>(data));
    if (ok && factOffset_ != 0 && blockAlign_ != 0) {
        ok = WriteAt(factOffset_, static_cast<uint32_t>(dataBytes_ / blockAlign_));
    }
    LARGE_INTEGER zero = {};
    ok = SetFilePointerEx(file_, zero, nullptr, FILE_END) && ok;
    if (!ok) failed_ = true;
    return ok;
}

bool WavWriter::WriteRaw(const void* data, DWORD bytes) {
    DWORD written = 0;
    if (!WriteFile(file_, data, bytes, &written, nullptr) || written != bytes) {
        failed_ = true;
        return false;
    }
    return true;
}

bool WavWriter::WriteAt(uint32_t offset, uint32_t value) {
    LARGE_INTEGER pos;
    pos.QuadPart = offset;
    if (!SetFilePointerEx(file_, pos, nullptr, FILE_BEGIN)) return false;
    uint8_t b[4];
    for (int i = 0; i < 4; ++i) b[i] = static_cast<uint8_t>(value >> (8 * i));
    return WriteRaw(b, 4);
}

}  // namespace ws
