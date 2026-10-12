#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sample_convert.h"
#include "wav_writer.h"

namespace ws {

struct RecordOptions {
    // Process whose audio (including its child processes) is recorded.
    // 0 records everything that plays on the default output device.
    DWORD targetPid = 0;
    // Full path of the output file without the ".wav" extension.
    std::wstring pathStem;
    OutputFormat format = OutputFormat::Pcm24;
    // Receives `notifyMessage` when recording ends on its own (error, target
    // app exited). The window should then call Recorder::Stop().
    HWND notifyWindow = nullptr;
    UINT notifyMessage = 0;
};

struct RecorderStatus {
    uint64_t bytes = 0;      // audio bytes written, all parts
    double seconds = 0;      // audio duration written, all parts
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    bool waitingForDevice = false;
};

struct StreamFormat {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint32_t channelMask = 0;
    SampleType sampleType = SampleType::Float32;
    uint16_t blockAlign = 0;
};

class CaptureStream;

class Recorder {
public:
    Recorder();
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Starts capturing. Blocks until the audio stream is running or has
    // failed; on failure returns false and fills `error`.
    bool Start(const RecordOptions& options, std::wstring* error);
    // Stops and finalizes the file. Safe to call at any time.
    void Stop();

    bool IsRunning() const { return thread_.joinable(); }
    RecorderStatus Status() const;
    // Peak levels (0..1) since the previous call.
    void TakePeaks(float* left, float* right);
    // Files that received audio. Empty files are deleted.
    std::vector<std::wstring> Files() const;
    // Why recording ended on its own; empty after a normal Stop().
    std::wstring EndReason() const;

private:
    void Run();
    bool Initialize(CaptureStream& stream, std::wstring* error);
    HRESULT Drain(CaptureStream& stream, bool* writeFailed);
    bool WriteFrames(const uint8_t* data, uint64_t frames);
    bool OpenNextFile();
    void CloseFile();
    void SetEndReason(const std::wstring& reason);

    RecordOptions options_;
    std::thread thread_;
    HANDLE stopEvent_ = nullptr;
    HANDLE initDone_ = nullptr;
    bool initOk_ = false;
    std::wstring initError_;

    // Capture-thread state.
    StreamFormat streamFormat_;
    StreamFormat fileFormat_;
    WavWriter writer_;
    int part_ = 0;
    std::vector<uint8_t> scratch_;
    Dither dither_;
    uint64_t lastPacketEnd_ = 0;  // QPC time (100 ns) where the previous packet ended

    // Shared with the UI thread.
    std::atomic<uint64_t> bytes_{0};
    std::atomic<double> seconds_{0.0};
    std::atomic<uint32_t> sampleRate_{0};
    std::atomic<uint16_t> channels_{0};
    std::atomic<bool> waitingForDevice_{false};
    std::atomic<float> peakLeft_{0.0f};
    std::atomic<float> peakRight_{0.0f};
    mutable std::mutex mutex_;
    std::vector<std::wstring> files_;
    std::wstring endReason_;
};

}  // namespace ws
