#include "audio_apps.h"

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwchar>
#include <map>
#include <set>

#include "com_ptr.h"

namespace ws {

namespace {

// Listed even before they have played anything.
const wchar_t* const kBrowsers[] = {L"chrome.exe", L"msedge.exe",  L"firefox.exe",
                                    L"brave.exe",  L"vivaldi.exe", L"opera.exe"};

struct ProcessEntry {
    DWORD parent;
    std::wstring exe;
};
using ProcessTable = std::map<DWORD, ProcessEntry>;

bool SameName(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

ProcessTable SnapshotProcesses() {
    ProcessTable table;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return table;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            table[entry.th32ProcessID] = {entry.th32ParentProcessID, entry.szExeFile};
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return table;
}

// Walks up while the parent runs the same executable, so e.g. Chrome's audio
// service process resolves to the browser process that owns the whole tree.
DWORD RootOf(DWORD pid, const ProcessTable& table) {
    for (int depth = 0; depth < 64; ++depth) {
        const auto self = table.find(pid);
        if (self == table.end()) break;
        const auto parent = table.find(self->second.parent);
        if (parent == table.end() || parent->first == pid ||
            !SameName(parent->second.exe, self->second.exe)) {
            break;
        }
        pid = parent->first;
    }
    return pid;
}

// Processes that own an audio session on any active output device.
std::set<DWORD> AudioSessionProcesses() {
    std::set<DWORD> pids;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), enumerator.PutVoid()))) {
        return pids;
    }
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.Put()))) return pids;
    UINT deviceCount = 0;
    devices->GetCount(&deviceCount);
    for (UINT i = 0; i < deviceCount; ++i) {
        ComPtr<IMMDevice> device;
        ComPtr<IAudioSessionManager2> manager;
        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(devices->Item(i, device.Put())) ||
            FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                    manager.PutVoid())) ||
            FAILED(manager->GetSessionEnumerator(sessions.Put()))) {
            continue;
        }
        int sessionCount = 0;
        sessions->GetCount(&sessionCount);
        for (int j = 0; j < sessionCount; ++j) {
            ComPtr<IAudioSessionControl> control;
            ComPtr<IAudioSessionControl2> control2;
            if (FAILED(sessions->GetSession(j, control.Put())) ||
                FAILED(control->QueryInterface(__uuidof(IAudioSessionControl2), control2.PutVoid()))) {
                continue;
            }
            if (control2->IsSystemSoundsSession() == S_OK) continue;
            DWORD pid = 0;
            if (SUCCEEDED(control2->GetProcessId(&pid)) && pid != 0) pids.insert(pid);
        }
    }
    return pids;
}

std::wstring ProcessImagePath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t buffer[1024];
    DWORD size = ARRAYSIZE(buffer);
    std::wstring path;
    if (QueryFullProcessImageNameW(process, 0, buffer, &size)) path.assign(buffer, size);
    CloseHandle(process);
    return path;
}

// "Google Chrome" for chrome.exe etc.
std::wstring FileDescription(const std::wstring& path) {
    if (path.empty()) return {};
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};

    struct LangCodePage {
        WORD language;
        WORD codePage;
    };
    LangCodePage* translations = nullptr;
    UINT bytes = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void**>(&translations), &bytes) ||
        bytes < sizeof(LangCodePage)) {
        return {};
    }
    wchar_t key[64];
    swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\FileDescription", translations[0].language,
             translations[0].codePage);
    wchar_t* text = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&text), &length) || length == 0) {
        return {};
    }
    std::wstring description(text);
    while (!description.empty() && iswspace(description.back())) description.pop_back();
    return description;
}

std::wstring StripExe(const std::wstring& exe) {
    if (exe.size() > 4 && SameName(exe.substr(exe.size() - 4), L".exe")) return exe.substr(0, exe.size() - 4);
    return exe;
}

}  // namespace

std::vector<AudioApp> ListAudioApps() {
    const ProcessTable table = SnapshotProcesses();
    std::set<DWORD> roots;
    for (DWORD pid : AudioSessionProcesses()) {
        if (table.count(pid)) roots.insert(RootOf(pid, table));
    }
    for (const auto& process : table) {
        for (const wchar_t* browser : kBrowsers) {
            if (SameName(process.second.exe, browser)) roots.insert(RootOf(process.first, table));
        }
    }

    std::vector<AudioApp> apps;
    const DWORD self = GetCurrentProcessId();
    for (DWORD pid : roots) {
        const auto it = table.find(pid);
        if (pid == 0 || pid == self || it == table.end()) continue;
        AudioApp app;
        app.pid = pid;
        app.exeName = it->second.exe;
        app.label = FileDescription(ProcessImagePath(pid));
        if (app.label.empty()) app.label = StripExe(app.exeName);
        apps.push_back(app);
    }

    // Chrome first (it's what this tool is mostly for), then alphabetical.
    std::stable_sort(apps.begin(), apps.end(), [](const AudioApp& a, const AudioApp& b) {
        const bool aChrome = SameName(a.exeName, L"chrome.exe");
        const bool bChrome = SameName(b.exeName, L"chrome.exe");
        if (aChrome != bChrome) return aChrome;
        return _wcsicmp(a.label.c_str(), b.label.c_str()) < 0;
    });
    return apps;
}

DWORD FindAppProcess(const std::wstring& exeName, DWORD preferredPid) {
    const ProcessTable table = SnapshotProcesses();
    const auto preferred = table.find(preferredPid);
    if (preferredPid != 0 && preferred != table.end() && SameName(preferred->second.exe, exeName)) {
        return RootOf(preferredPid, table);
    }
    // Prefer an instance that is actually producing audio.
    for (DWORD pid : AudioSessionProcesses()) {
        const auto it = table.find(pid);
        if (it != table.end() && SameName(it->second.exe, exeName)) return RootOf(pid, table);
    }
    for (const auto& process : table) {
        if (SameName(process.second.exe, exeName)) return RootOf(process.first, table);
    }
    return 0;
}

}  // namespace ws
