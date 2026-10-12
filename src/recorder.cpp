#include "recorder.h"

#include <mmreg.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <propidl.h>

#include <algorithm>
#include <cwchar>

#include "com_ptr.h"

#ifndef AUDCLNT_E_RESOURCES_INVALIDATED
#define AUDCLNT_E_RESOURCES_INVALIDATED AUDCLNT_ERR(0x026)
#endif

namespace ws {

namespace {

constexpr REFERENCE_TIME kBufferDuration = 2 * 10000000;  // 2 s, in 100 ns units
constexpr uint64_t kHnsPerSecond = 10000000;
// Loopback streams deliver nothing while the source is silent. Gaps longer
// than this between packets are filled with silence so timing is preserved.
constexpr uint64_t kGapThreshold = 30 * 10000;  // 30 ms
constexpr uint64_t kMaxGap = 24ull * 3600 * kHnsPerSecond;

// Same layout as AUDIOCLIENT_ACTIVATION_PARAMS in audioclientactivationparams.h
// (Windows SDK 10.0.20348+). Declared here so older SDKs and MinGW can build.
struct ProcessLoopbackParams {
    DWORD targetProcessId;
    DWORD processLoopbackMode;  // PROCESS_LOOPBACK_MODE
};
struct ActivationParams {
    DWORD activationType;  // AUDIOCLIENT_ACTIVATION_TYPE
    ProcessLoopbackParams processLoopbackParams;
};
constexpr DWORD kActivationTypeProcessLoopback = 1;
constexpr DWORD kIncludeTargetProcessTree = 0;
constexpr wchar_t kProcessLoopbackDevice[] = L"VAD\\Process_Loopback";

using ActivateAudioInterfaceAsyncFn =
    HRESULT(WINAPI*)(LPCWSTR, REFIID, PROPVARIANT*, IActivateAudioInterfaceCompletionHandler*,
                     IActivateAudioInterfaceAsyncOperation**);

const GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

std::wstring WithCode(const std::wstring& message, HRESULT hr) {
    wchar_t code[48];
    swprintf(code, 48, L"\n（エラーコード 0x%08lX）", static_cast<unsigned long>(hr));
    return message + code;
}

bool IsDeviceLost(HRESULT hr) {
    return hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING ||
           hr == AUDCLNT_E_RESOURCES_INVALIDATED;
}

void UpdatePeak(std::atomic<float>& slot, float value) {
    float current = slot.load(std::memory_order_relaxed);
    while (value > current &&
           !slot.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

bool ParseFormat(const WAVEFORMATEX* w, StreamFormat* f) {
    WORD tag = w->wFormatTag;
    f->channelMask = 0;
    if (tag == WAVE_FORMAT_EXTENSIBLE && w->cbSize >= 22) {
        const auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(w);
        if (IsEqualGUID(x->SubFormat, kSubtypeFloat)) {
            tag = WAVE_FORMAT_IEEE_FLOAT;
        } else if (IsEqualGUID(x->SubFormat, kSubtypePcm)) {
            tag = WAVE_FORMAT_PCM;
        } else {
            return false;
        }
        f->channelMask = x->dwChannelMask;
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT && w->wBitsPerSample == 32) {
        f->sampleType = SampleType::Float32;
    } else if (tag == WAVE_FORMAT_PCM && w->wBitsPerSample == 16) {
        f->sampleType = SampleType::Int16;
    } else if (tag == WAVE_FORMAT_PCM && w->wBitsPerSample == 24) {
        f->sampleType = SampleType::Int24;
    } else if (tag == WAVE_FORMAT_PCM && w->wBitsPerSample == 32) {
        f->sampleType = SampleType::Int32;
    } else {
        return false;
    }
    f->sampleRate = w->nSamplesPerSec;
    f->channels = w->nChannels;
    f->blockAlign = w->nBlockAlign;
    return f->channels > 0 && f->sampleRate > 0 &&
           f->blockAlign == f->channels * BytesPerSample(f->sampleType);
}

#ifdef WAVESIPHON_TEST_INPUT_DEVICE
// Test builds record the default *input* device instead, so the capture path
// can be exercised where loopback is unavailable (e.g. under Wine).
constexpr EDataFlow kSystemFlow = eCapture;
constexpr DWORD kSystemLoopbackFlag = 0;
#else
constexpr EDataFlow kSystemFlow = eRender;
constexpr DWORD kSystemLoopbackFlag = AUDCLNT_STREAMFLAGS_LOOPBACK;
#endif

HRESULT GetDefaultDevice(EDataFlow flow, IMMDevice** device) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), enumerator.PutVoid());
    if (FAILED(hr)) return hr;
    return enumerator->GetDefaultAudioEndpoint(flow, eConsole, device);
}

// Sample rate the default output device mixes at. Capturing an app at this
// rate avoids an extra resampling step.
uint32_t DefaultRenderSampleRate() {
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    WAVEFORMATEX* mix = nullptr;
    uint32_t rate = 48000;
    if (SUCCEEDED(GetDefaultDevice(eRender, device.Put())) &&
        SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.PutVoid())) &&
        SUCCEEDED(client->GetMixFormat(&mix))) {
        if (mix->nSamplesPerSec >= 8000 && mix->nSamplesPerSec <= 384000) rate = mix->nSamplesPerSec;
        CoTaskMemFree(mix);
    }
    return rate;
}

class ActivateHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    ActivateHandler() : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    HANDLE DoneEvent() const { return done_; }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(IActivateAudioInterfaceCompletionHandler))) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        } else if (IsEqualIID(riid, __uuidof(IAgileObject))) {
            *ppv = static_cast<IAgileObject*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
        SetEvent(done_);
        return S_OK;
    }

private:
    ~ActivateHandler() { CloseHandle(done_); }

    LONG refs_ = 1;
    HANDLE done_;
};

// Activates an IAudioClient that captures what `pid` and its child processes
// play (Windows 10 build 20348+ / Windows 11).
HRESULT ActivateProcessLoopbackClient(DWORD pid, IAudioClient** client) {
    static const auto activate = [] {
        HMODULE module = LoadLibraryExW(L"mmdevapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        FARPROC proc = module ? GetProcAddress(module, "ActivateAudioInterfaceAsync") : nullptr;
        return reinterpret_cast<ActivateAudioInterfaceAsyncFn>(reinterpret_cast<void*>(proc));
    }();
    if (!activate) return E_NOTIMPL;

    ActivationParams params = {};
    params.activationType = kActivationTypeProcessLoopback;
    params.processLoopbackParams.targetProcessId = pid;
    params.processLoopbackParams.processLoopbackMode = kIncludeTargetProcessTree;

    PROPVARIANT activateParams;
    PropVariantInit(&activateParams);
    activateParams.vt = VT_BLOB;
    activateParams.blob.cbSize = sizeof(params);
    activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto* handler = new ActivateHandler();
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    HRESULT hr = activate(kProcessLoopbackDevice, __uuidof(IAudioClient), &activateParams, handler,
                          operation.Put());
    if (SUCCEEDED(hr)) {
        if (WaitForSingleObject(handler->DoneEvent(), 10000) != WAIT_OBJECT_0) {
            hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        } else {
            HRESULT activateResult = E_FAIL;
            ComPtr<IUnknown> unknown;
            hr = operation->GetActivateResult(&activateResult, unknown.Put());
            if (SUCCEEDED(hr)) hr = activateResult;
            if (SUCCEEDED(hr)) {
                hr = unknown->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(client));
            }
        }
    }
    handler->Release();
    return hr;
}

WAVEFORMATEXTENSIBLE MakeFormat(bool isFloat, bool extensible, uint32_t rate) {
    WAVEFORMATEXTENSIBLE wfx = {};
    const WORD bits = isFloat ? 32 : 16;
    wfx.Format.nChannels = 2;
    wfx.Format.nSamplesPerSec = rate;
    wfx.Format.wBitsPerSample = bits;
    wfx.Format.nBlockAlign = static_cast<WORD>(2 * bits / 8);
    wfx.Format.nAvgBytesPerSec = rate * wfx.Format.nBlockAlign;
    if (extensible) {
        wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wfx.Samples.wValidBitsPerSample = bits;
        wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        wfx.SubFormat = isFloat ? kSubtypeFloat : kSubtypePcm;
    } else {
        wfx.Format.wFormatTag = isFloat ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    }
    return wfx;
}

}  // namespace

// One running WASAPI loopback stream.
class CaptureStream {
public:
    ~CaptureStream() { Close(); }

    HRESULT Open(DWORD pid, std::wstring* error) {
        Close();
        HRESULT hr = pid ? OpenProcessLoopback(pid, error) : OpenSystemLoopback(error);
        if (FAILED(hr)) {
            Close();
            return hr;
        }
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr = client->SetEventHandle(event);
        if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioCaptureClient), capture.PutVoid());
        if (SUCCEEDED(hr)) hr = client->Start();
        if (FAILED(hr)) {
            *error = WithCode(L"録音を開始できませんでした。", hr);
            Close();
        }
        return hr;
    }

    void Close() {
        if (client) client->Stop();
        capture.Reset();
        client.Reset();
        if (event) {
            CloseHandle(event);
            event = nullptr;
        }
    }

    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    HANDLE event = nullptr;
    StreamFormat format;

private:
    HRESULT OpenSystemLoopback(std::wstring* error) {
        ComPtr<IMMDevice> device;
        HRESULT hr = GetDefaultDevice(kSystemFlow, device.Put());
        if (FAILED(hr)) {
            *error = WithCode(L"再生デバイス（スピーカー／ヘッドホン）が見つかりません。", hr);
            return hr;
        }
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.PutVoid());
        WAVEFORMATEX* mix = nullptr;
        if (SUCCEEDED(hr)) hr = client->GetMixFormat(&mix);
        if (FAILED(hr)) {
            *error = WithCode(L"再生デバイスに接続できませんでした。", hr);
            return hr;
        }
        if (!ParseFormat(mix, &format)) {
            CoTaskMemFree(mix);
            *error = L"再生デバイスの音声形式に対応していません。";
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                kSystemLoopbackFlag | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                kBufferDuration, 0, mix, nullptr);
        CoTaskMemFree(mix);
        if (FAILED(hr)) *error = WithCode(L"再生デバイスの音を取得できませんでした。", hr);
        return hr;
    }

    HRESULT OpenProcessLoopback(DWORD pid, std::wstring* error) {
        // A process loopback client has no mix format of its own, so we pick
        // one: 32-bit float stereo at the output device's rate (lossless for
        // what the app renders). Plainer formats are fallbacks.
        const uint32_t rate = DefaultRenderSampleRate();
        const WAVEFORMATEXTENSIBLE candidates[] = {
            MakeFormat(true, true, rate),
            MakeFormat(true, false, rate),
            MakeFormat(false, false, rate),
        };
        HRESULT hr = E_FAIL;
        for (const auto& wfx : candidates) {
            client.Reset();
            hr = ActivateProcessLoopbackClient(pid, client.Put());
            if (FAILED(hr)) {
                *error = WithCode(L"アプリの音を取得できませんでした。\n"
                                  L"（この機能は Windows 11 / Windows 10 の新しいビルドが必要です）",
                                  hr);
                return hr;
            }
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                    kBufferDuration, 0, &wfx.Format, nullptr);
            if (SUCCEEDED(hr)) {
                ParseFormat(&wfx.Format, &format);
                return hr;
            }
        }
        *error = WithCode(L"アプリの音を取得する形式を設定できませんでした。", hr);
        return hr;
    }
};

Recorder::Recorder()
    : stopEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      initDone_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

Recorder::~Recorder() {
    Stop();
    CloseHandle(stopEvent_);
    CloseHandle(initDone_);
}

bool Recorder::Start(const RecordOptions& options, std::wstring* error) {
    Stop();
    options_ = options;
    ResetEvent(stopEvent_);
    ResetEvent(initDone_);
    initOk_ = false;
    initError_.clear();
    part_ = 0;
    lastPacketEnd_ = 0;
    bytes_ = 0;
    seconds_ = 0.0;
    sampleRate_ = 0;
    channels_ = 0;
    waitingForDevice_ = false;
    peakLeft_ = 0.0f;
    peakRight_ = 0.0f;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        files_.clear();
        endReason_.clear();
    }

    thread_ = std::thread(&Recorder::Run, this);
    WaitForSingleObject(initDone_, INFINITE);  // Run() always signals it
    if (!initOk_) {
        thread_.join();
        if (error) *error = initError_;
        return false;
    }
    return true;
}

void Recorder::Stop() {
    if (!thread_.joinable()) return;
    SetEvent(stopEvent_);
    thread_.join();
}

RecorderStatus Recorder::Status() const {
    RecorderStatus s;
    s.bytes = bytes_.load();
    s.seconds = seconds_.load();
    s.sampleRate = sampleRate_.load();
    s.channels = channels_.load();
    s.waitingForDevice = waitingForDevice_.load();
    return s;
}

void Recorder::TakePeaks(float* left, float* right) {
    *left = peakLeft_.exchange(0.0f);
    *right = peakRight_.exchange(0.0f);
}

std::vector<std::wstring> Recorder::Files() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return files_;
}

std::wstring Recorder::EndReason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return endReason_;
}

void Recorder::SetEndReason(const std::wstring& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    endReason_ = reason;
}

void Recorder::Run() {
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
    HANDLE target = options_.targetPid ? OpenProcess(SYNCHRONIZE, FALSE, options_.targetPid) : nullptr;

    {
        CaptureStream stream;
        if (options_.targetPid && !target) {
            initError_ = L"録音するアプリが見つかりません。起動しているか確認してください。";
        } else {
            initOk_ = Initialize(stream, &initError_);
        }
        SetEvent(initDone_);

        bool endedOnItsOwn = false;
        while (initOk_) {
            HANDLE waits[] = {stopEvent_, stream.event, target};
            const DWORD w = WaitForMultipleObjects(target ? 3 : 2, waits, FALSE, 10);
            if (w == WAIT_OBJECT_0) break;

            bool writeFailed = false;
            const HRESULT hr = Drain(stream, &writeFailed);
            if (writeFailed) {
                SetEndReason(L"ファイルへの書き込みに失敗したため、録音を停止しました。\n"
                             L"保存先の空き容量を確認してください。");
                endedOnItsOwn = true;
                break;
            }
            if (w == WAIT_OBJECT_0 + 2) {
                SetEndReason(L"録音していたアプリが終了したため、録音を停止しました。");
                endedOnItsOwn = true;
                break;
            }
            if (IsDeviceLost(hr)) {
                // Output device changed or disappeared: wait for one to come
                // back and carry on in the same file when the format allows.
                stream.Close();
                waitingForDevice_ = true;
                bool reopened = false;
                HANDLE retryWaits[] = {stopEvent_, target};
                while (WaitForMultipleObjects(target ? 2 : 1, retryWaits, FALSE, 500) == WAIT_TIMEOUT) {
                    std::wstring ignored;
                    if (SUCCEEDED(stream.Open(options_.targetPid, &ignored))) {
                        reopened = true;
                        break;
                    }
                }
                waitingForDevice_ = false;
                if (!reopened) {
                    if (WaitForSingleObject(stopEvent_, 0) != WAIT_OBJECT_0) {
                        SetEndReason(L"録音していたアプリが終了したため、録音を停止しました。");
                        endedOnItsOwn = true;
                    }
                    break;
                }
                streamFormat_ = stream.format;
                sampleRate_ = streamFormat_.sampleRate;
                channels_ = streamFormat_.channels;
                lastPacketEnd_ = 0;
                if (streamFormat_.sampleRate != fileFormat_.sampleRate ||
                    streamFormat_.channels != fileFormat_.channels) {
                    if (!OpenNextFile()) {
                        SetEndReason(L"新しいファイルを作成できなかったため、録音を停止しました。");
                        endedOnItsOwn = true;
                        break;
                    }
                }
                continue;
            }
            if (FAILED(hr)) {
                SetEndReason(WithCode(L"録音中にエラーが発生したため、停止しました。", hr));
                endedOnItsOwn = true;
                break;
            }
        }

        stream.Close();
        CloseFile();
        if (endedOnItsOwn && options_.notifyWindow) {
            PostMessageW(options_.notifyWindow, options_.notifyMessage, 0, 0);
        }
    }

    if (target) CloseHandle(target);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (SUCCEEDED(coHr)) CoUninitialize();
}

bool Recorder::Initialize(CaptureStream& stream, std::wstring* error) {
    if (FAILED(stream.Open(options_.targetPid, error))) return false;
    streamFormat_ = stream.format;
    sampleRate_ = streamFormat_.sampleRate;
    channels_ = streamFormat_.channels;
    if (!OpenNextFile()) {
        *error = L"保存用のファイルを作成できませんでした。保存先フォルダを確認してください。\n" +
                 options_.pathStem + L".wav";
        stream.Close();
        return false;
    }
    return true;
}

HRESULT Recorder::Drain(CaptureStream& stream, bool* writeFailed) {
    for (;;) {
        UINT32 packetFrames = 0;
        HRESULT hr = stream.capture->GetNextPacketSize(&packetFrames);
        if (FAILED(hr)) return hr;
        if (packetFrames == 0) return S_OK;

        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 qpcPosition = 0;  // 100 ns units
        hr = stream.capture->GetBuffer(&data, &frames, &flags, nullptr, &qpcPosition);
        if (FAILED(hr)) return hr;
        if (hr == AUDCLNT_S_BUFFER_EMPTY) return S_OK;

        const StreamFormat& f = streamFormat_;
        bool ok = true;
        if (qpcPosition != 0) {
            if (lastPacketEnd_ != 0 && qpcPosition > lastPacketEnd_ + kGapThreshold) {
                const uint64_t gap = std::min(qpcPosition - lastPacketEnd_, kMaxGap);
                ok = WriteFrames(nullptr, gap * f.sampleRate / kHnsPerSecond);
            }
            lastPacketEnd_ = qpcPosition + static_cast<uint64_t>(frames) * kHnsPerSecond / f.sampleRate;
        }
        if (ok && frames > 0) {
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                ok = WriteFrames(nullptr, frames);
            } else {
                const size_t outBytes =
                    static_cast<size_t>(frames) * f.channels * BytesPerSample(options_.format);
                if (scratch_.size() < outBytes) scratch_.resize(outBytes);
                float peaks[2] = {0.0f, 0.0f};
                ConvertFrames(data, f.sampleType, scratch_.data(), options_.format, frames, f.channels,
                              peaks, dither_);
                UpdatePeak(peakLeft_, peaks[0]);
                UpdatePeak(peakRight_, peaks[1]);
                ok = WriteFrames(scratch_.data(), frames);
            }
        }

        hr = stream.capture->ReleaseBuffer(frames);
        if (!ok) {
            *writeFailed = true;
            return S_OK;
        }
        if (FAILED(hr)) return hr;
    }
}

bool Recorder::WriteFrames(const uint8_t* data, uint64_t frames) {
    while (frames > 0) {
        const uint64_t block = writer_.BlockAlign();
        const uint64_t used = std::min(writer_.DataBytes(), WavWriter::kMaxDataBytes);
        const uint64_t room = (WavWriter::kMaxDataBytes - used) / block;
        if (room == 0) {  // 4 GB WAV limit: continue in the next part
            if (!OpenNextFile()) return false;
            continue;
        }
        const uint64_t n = std::min(frames, room);
        const size_t bytes = static_cast<size_t>(n * block);
        if (!(data ? writer_.Write(data, bytes) : writer_.WriteSilence(bytes))) return false;
        if (data) data += bytes;
        frames -= n;
        bytes_ += bytes;
        seconds_ = seconds_.load() + static_cast<double>(n) / fileFormat_.sampleRate;
    }
    return true;
}

bool Recorder::OpenNextFile() {
    CloseFile();
    ++part_;
    std::wstring path = options_.pathStem;
    if (part_ > 1) path += L"_part" + std::to_wstring(part_);
    path += L".wav";
    fileFormat_ = streamFormat_;
    if (!writer_.Open(path, options_.format, fileFormat_.channels, fileFormat_.sampleRate,
                      fileFormat_.channelMask)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    files_.push_back(path);
    return true;
}

void Recorder::CloseFile() {
    if (!writer_.IsOpen()) return;
    const bool empty = writer_.DataBytes() == 0;
    const std::wstring path = writer_.Path();
    writer_.Close();
    if (empty) {  // nothing was captured: don't leave a 0-second file behind
        DeleteFileW(path.c_str());
        std::lock_guard<std::mutex> lock(mutex_);
        files_.erase(std::remove(files_.begin(), files_.end(), path), files_.end());
    }
}

}  // namespace ws
