// WaveSiphon - records what an app (e.g. Chrome) or the whole PC is playing,
// straight from the Windows audio engine, into a WAV file.

#include <windows.h>
#include <commctrl.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>
#include <vector>

#include "audio_apps.h"
#include "com_ptr.h"
#include "recorder.h"
#include "resource.h"

using ws::AudioApp;
using ws::OutputFormat;

namespace {

constexpr wchar_t kWindowClass[] = L"WaveSiphonWindow";
constexpr wchar_t kAppName[] = L"WaveSiphon";
constexpr wchar_t kSystemSource[] = L"*system*";
constexpr UINT kRecordingEndedMessage = WM_APP + 1;
constexpr UINT_PTR kMeterTimer = 1;
constexpr DWORD kWindowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

// Layout in 96-DPI pixels.
constexpr int kClientWidth = 520;
constexpr int kClientHeight = 290;
constexpr int kMargin = 12;
constexpr int kLabelWidth = 76;
constexpr int kFieldX = kMargin + kLabelWidth + 8;
constexpr int kRight = kClientWidth - kMargin;
constexpr int kButtonWidth = 84;
constexpr int kGap = 6;
constexpr int kRowHeight = 26;

enum ControlId {
    kSourceLabelId = 100,
    kSourceId,
    kHintId,
    kFormatLabelId,
    kFormatId,
    kFolderLabelId,
    kFolderId,
    kBrowseId,
    kOpenId,
    kTopMostId,
    kRecordId,
    kLeftLabelId,
    kMeterLeftId,
    kRightLabelId,
    kMeterRightId,
    kStatusId,
};

struct FormatChoice {
    OutputFormat format;
    const wchar_t* label;
};
const FormatChoice kFormats[] = {
    {OutputFormat::Pcm24, L"WAV 24bit（高音質・おすすめ）"},
    {OutputFormat::Pcm16, L"WAV 16bit（CDと同じ・互換性重視）"},
    {OutputFormat::Float32, L"WAV 32bit float（完全無劣化・編集向け）"},
};

struct SourceItem {
    bool system = false;
    AudioApp app;  // pid 0: app is not running right now
};

struct AppState {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    UINT dpi = 96;
    HFONT font = nullptr;
    HFONT bigFont = nullptr;
    HWND source = nullptr;
    HWND hint = nullptr;
    HWND format = nullptr;
    HWND folderEdit = nullptr;
    HWND browse = nullptr;
    HWND open = nullptr;
    HWND topMost = nullptr;
    HWND record = nullptr;
    HWND meterLeft = nullptr;
    HWND meterRight = nullptr;
    HWND status = nullptr;

    std::vector<SourceItem> sources;
    std::wstring folder;
    std::wstring sourceSetting = L"chrome.exe";  // exe name or kSystemSource
    int formatIndex = 0;
    bool alwaysOnTop = false;

    ws::Recorder recorder;
    bool recording = false;
    ULONGLONG startTick = 0;
    float levelLeft = 0.0f;  // displayed meter position, 0..1
    float levelRight = 0.0f;
    std::wstring lastFile;
    std::wstring statusText;
};

AppState g;

int Scale(int value) { return MulDiv(value, static_cast<int>(g.dpi), 96); }

bool SameName(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

std::wstring StripExe(const std::wstring& exe) {
    if (exe.size() > 4 && SameName(exe.substr(exe.size() - 4), L".exe")) return exe.substr(0, exe.size() - 4);
    return exe;
}

std::wstring KnownAppLabel(const std::wstring& exe) {
    if (SameName(exe, L"chrome.exe")) return L"Google Chrome";
    if (SameName(exe, L"msedge.exe")) return L"Microsoft Edge";
    if (SameName(exe, L"firefox.exe")) return L"Firefox";
    return StripExe(exe);
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &path))) result = path;
    CoTaskMemFree(path);
    return result;
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) return dir + name;
    return dir + L"\\" + name;
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool FileExists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// ---------------------------------------------------------------------------
// Settings (%APPDATA%\WaveSiphon\settings.ini)

std::wstring SettingsPath() {
    const std::wstring dir = JoinPath(KnownFolder(FOLDERID_RoamingAppData), kAppName);
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = JoinPath(dir, L"settings.ini");
    if (!FileExists(path)) {
        // A UTF-16 BOM makes the *PrivateProfile* APIs store Unicode, so
        // folder names in any language survive.
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            const BYTE bom[] = {0xFF, 0xFE};
            DWORD written = 0;
            WriteFile(file, bom, sizeof(bom), &written, nullptr);
            CloseHandle(file);
        }
    }
    return path;
}

std::wstring ReadSetting(const wchar_t* key, const std::wstring& fallback) {
    wchar_t buffer[2048];
    GetPrivateProfileStringW(kAppName, key, fallback.c_str(), buffer, ARRAYSIZE(buffer), SettingsPath().c_str());
    return buffer;
}

void LoadSettings() {
    g.folder = ReadSetting(L"Folder", JoinPath(KnownFolder(FOLDERID_Music), kAppName));
    g.sourceSetting = ReadSetting(L"Source", L"chrome.exe");
    g.formatIndex = static_cast<int>(GetPrivateProfileIntW(kAppName, L"Format", 0, SettingsPath().c_str()));
    if (g.formatIndex < 0 || g.formatIndex >= static_cast<int>(ARRAYSIZE(kFormats))) g.formatIndex = 0;
    g.alwaysOnTop = GetPrivateProfileIntW(kAppName, L"TopMost", 0, SettingsPath().c_str()) != 0;
}

void SaveSettings() {
    const std::wstring path = SettingsPath();
    WritePrivateProfileStringW(kAppName, L"Folder", g.folder.c_str(), path.c_str());
    WritePrivateProfileStringW(kAppName, L"Source", g.sourceSetting.c_str(), path.c_str());
    WritePrivateProfileStringW(kAppName, L"Format", std::to_wstring(g.formatIndex).c_str(), path.c_str());
    WritePrivateProfileStringW(kAppName, L"TopMost", g.alwaysOnTop ? L"1" : L"0", path.c_str());
}

// ---------------------------------------------------------------------------
// Formatting helpers

std::wstring FormatClock(ULONGLONG totalSeconds) {
    wchar_t text[32];
    swprintf(text, 32, L"%02llu:%02llu:%02llu", totalSeconds / 3600, (totalSeconds / 60) % 60, totalSeconds % 60);
    return text;
}

std::wstring FormatDuration(double seconds) {
    const unsigned long long s = static_cast<unsigned long long>(seconds + 0.5);
    wchar_t text[48];
    if (s >= 3600) {
        swprintf(text, 48, L"%llu時間%02llu分%02llu秒", s / 3600, (s / 60) % 60, s % 60);
    } else {
        swprintf(text, 48, L"%llu分%02llu秒", s / 60, s % 60);
    }
    return text;
}

std::wstring FormatSize(uint64_t bytes) {
    wchar_t text[32];
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (mb >= 1024.0) {
        swprintf(text, 32, L"%.2f GB", mb / 1024.0);
    } else {
        swprintf(text, 32, L"%.1f MB", mb);
    }
    return text;
}

std::wstring FormatStream(const ws::RecorderStatus& s) {
    wchar_t rate[16];
    if (s.sampleRate % 1000 == 0) {
        swprintf(rate, 16, L"%u", s.sampleRate / 1000);
    } else {
        swprintf(rate, 16, L"%.1f", s.sampleRate / 1000.0);
    }
    const int bits = ws::BytesPerSample(kFormats[g.formatIndex].format) * 8;
    wchar_t text[64];
    swprintf(text, 64, L"%ls kHz / %d bit%ls / %u ch", rate, bits,
             kFormats[g.formatIndex].format == OutputFormat::Float32 ? L" float" : L"",
             static_cast<unsigned>(s.channels));
    return text;
}

// ---------------------------------------------------------------------------
// UI

void SetStatus(const std::wstring& text) {
    if (text == g.statusText) return;  // avoid flicker from redundant updates
    g.statusText = text;
    SetWindowTextW(g.status, text.c_str());
}

const SourceItem& CurrentSource() {
    LRESULT index = SendMessageW(g.source, CB_GETCURSEL, 0, 0);
    if (index < 0 || index >= static_cast<LRESULT>(g.sources.size())) index = 0;
    return g.sources[static_cast<size_t>(index)];
}

void UpdateHint() {
    const SourceItem& item = CurrentSource();
    std::wstring text;
    if (item.system) {
        text = L"PCで鳴っているすべての音を録音します（通知音なども入ります）";
    } else if (item.app.pid == 0) {
        text = L"「" + item.app.label + L"」はまだ起動していません（起動後に録音できます）";
    } else {
        text = L"「" + item.app.label + L"」の音だけを録音します（他の音は入りません）";
    }
    SetWindowTextW(g.hint, text.c_str());
}

// Rebuilds the source list, keeping `select` (exe name or kSystemSource)
// selected when possible.
void RefreshSources(const std::wstring& select) {
    g.sources.clear();
    g.sources.push_back(SourceItem{true, {}});
    for (const AudioApp& app : ws::ListAudioApps()) g.sources.push_back(SourceItem{false, app});

    size_t selected = 0;
    if (select != kSystemSource) {
        for (size_t i = 1; i < g.sources.size(); ++i) {
            if (SameName(g.sources[i].app.exeName, select)) {
                selected = i;
                break;
            }
        }
        if (selected == 0 && !select.empty()) {
            // Keep the user's choice even while the app is closed.
            SourceItem placeholder;
            placeholder.app.exeName = select;
            placeholder.app.label = KnownAppLabel(select);
            g.sources.push_back(placeholder);
            selected = g.sources.size() - 1;
        }
    }

    SendMessageW(g.source, CB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < g.sources.size(); ++i) {
        const SourceItem& item = g.sources[i];
        std::wstring text;
        if (item.system) {
            text = L"PC全体の音（すべてのアプリ）";
        } else {
            text = item.app.label + L"（" + item.app.exeName + L"）";
            int sameExeBefore = 0;
            int sameExeTotal = 0;
            for (size_t j = 1; j < g.sources.size(); ++j) {
                if (!SameName(g.sources[j].app.exeName, item.app.exeName)) continue;
                ++sameExeTotal;
                if (j < i) ++sameExeBefore;
            }
            if (sameExeTotal > 1) text += L" #" + std::to_wstring(sameExeBefore + 1);
            if (item.app.pid == 0) text += L" ─ 未起動";
        }
        SendMessageW(g.source, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    SendMessageW(g.source, CB_SETCURSEL, selected, 0);
    UpdateHint();
}

void UpdateControls() {
    const BOOL idle = g.recording ? FALSE : TRUE;
    EnableWindow(g.source, idle);
    EnableWindow(g.format, idle);
    EnableWindow(g.browse, idle);
    SetWindowTextW(g.record, g.recording ? L"■  録音停止" : L"●  録音開始");
    SetWindowTextW(g.window, g.recording ? L"● 録音中 - WaveSiphon" : kAppName);
}

void ApplyTopMost() {
    SetWindowPos(g.window, g.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void InvalidateMeters() {
    InvalidateRect(g.meterLeft, nullptr, FALSE);
    InvalidateRect(g.meterRight, nullptr, FALSE);
}

float MeterPosition(float peak) {
    if (peak <= 0.001f) return 0.0f;  // below -60 dBFS
    const float db = 20.0f * std::log10(peak);
    return std::min(1.0f, std::max(0.0f, (db + 60.0f) / 60.0f));
}

void DrawMeter(const DRAWITEMSTRUCT* dis, float level) {
    const RECT& rc = dis->rcItem;
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    HDC dc = CreateCompatibleDC(dis->hDC);
    HBITMAP bitmap = CreateCompatibleBitmap(dis->hDC, width, height);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);

    auto fill = [&](int x0, int x1, COLORREF color) {
        if (x1 <= x0) return;
        RECT r = {x0, 0, x1, height};
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc, &r, brush);
        DeleteObject(brush);
    };
    fill(0, width, RGB(48, 48, 48));
    const int filled = static_cast<int>(level * width + 0.5f);
    const int yellowAt = width * 48 / 60;  // -12 dBFS
    const int redAt = width * 57 / 60;     // -3 dBFS
    fill(0, std::min(filled, yellowAt), RGB(46, 204, 113));
    fill(yellowAt, std::min(filled, redAt), RGB(241, 196, 15));
    fill(redAt, filled, RGB(231, 76, 60));

    BitBlt(dis->hDC, rc.left, rc.top, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

std::wstring MakePathStem(const SourceItem& item) {
    const std::wstring base = item.system ? L"system" : StripExe(item.app.exeName);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[32];
    swprintf(stamp, 32, L"_%04u-%02u-%02u_%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const std::wstring stem = JoinPath(g.folder, base + stamp);
    std::wstring candidate = stem;
    for (int n = 2; FileExists(candidate + L".wav"); ++n) candidate = stem + L"_" + std::to_wstring(n);
    return candidate;
}

void StartRecording() {
    const SourceItem item = CurrentSource();
    DWORD pid = 0;
    if (!item.system) {
        pid = ws::FindAppProcess(item.app.exeName, item.app.pid);
        if (pid == 0) {
            const std::wstring message = L"「" + item.app.label + L"」が起動していません。\n先にアプリを起動してから録音を開始してください。";
            MessageBoxW(g.window, message.c_str(), kAppName, MB_ICONINFORMATION);
            return;
        }
    }
    const int dirResult = SHCreateDirectoryExW(nullptr, g.folder.c_str(), nullptr);
    if (dirResult != ERROR_SUCCESS && dirResult != ERROR_ALREADY_EXISTS && dirResult != ERROR_FILE_EXISTS) {
        const std::wstring message = L"保存先フォルダを作成できませんでした。\n" + g.folder;
        MessageBoxW(g.window, message.c_str(), kAppName, MB_ICONERROR);
        return;
    }

    ws::RecordOptions options;
    options.targetPid = pid;
    options.pathStem = MakePathStem(item);
    options.format = kFormats[g.formatIndex].format;
    options.notifyWindow = g.window;
    options.notifyMessage = kRecordingEndedMessage;

    HCURSOR oldCursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    std::wstring error;
    const bool started = g.recorder.Start(options, &error);
    SetCursor(oldCursor);
    if (!started) {
        MessageBoxW(g.window, error.c_str(), kAppName, MB_ICONERROR);
        return;
    }

    g.recording = true;
    g.startTick = GetTickCount64();
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);  // don't let the PC sleep mid-recording
    UpdateControls();
    SetStatus(L"● 録音中 00:00:00");
    SetTimer(g.window, kMeterTimer, 50, nullptr);
}

void StopRecording() {
    KillTimer(g.window, kMeterTimer);
    g.recorder.Stop();
    g.recording = false;
    SetThreadExecutionState(ES_CONTINUOUS);

    const std::vector<std::wstring> files = g.recorder.Files();
    const ws::RecorderStatus status = g.recorder.Status();
    if (files.empty()) {
        SetStatus(L"音が届かなかったため、ファイルは作成されませんでした。");
    } else {
        g.lastFile = files.back();
        std::wstring text = L"保存しました: " + FileNameOf(files.front());
        if (files.size() > 1) text += L" ほか" + std::to_wstring(files.size() - 1) + L"ファイル";
        text += L"（" + FormatDuration(status.seconds) + L"・" + FormatSize(status.bytes) + L"）";
        SetStatus(text);
    }
    g.levelLeft = 0.0f;
    g.levelRight = 0.0f;
    InvalidateMeters();
    UpdateControls();

    const std::wstring reason = g.recorder.EndReason();
    if (!reason.empty()) MessageBoxW(g.window, reason.c_str(), kAppName, MB_ICONWARNING);
}

void OnMeterTimer() {
    float left = 0.0f;
    float right = 0.0f;
    g.recorder.TakePeaks(&left, &right);
    // Instant attack, ~30 dB/s fall.
    g.levelLeft = std::max(MeterPosition(left), g.levelLeft - 0.025f);
    g.levelRight = std::max(MeterPosition(right), g.levelRight - 0.025f);
    InvalidateMeters();

    const ws::RecorderStatus s = g.recorder.Status();
    std::wstring text = L"● 録音中 " + FormatClock((GetTickCount64() - g.startTick) / 1000) + L"　";
    if (s.waitingForDevice) {
        text += L"出力デバイスの切り替えを待っています…";
    } else if (s.bytes == 0) {
        text += L"音が流れるのを待っています…";
    } else {
        text += FormatSize(s.bytes) + L"　" + FormatStream(s);
    }
    SetStatus(text);
}

void BrowseFolder() {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IFileOpenDialog),
                                dialog.PutVoid()))) {
        return;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"録音ファイルの保存先を選択");
    ComPtr<IShellItem> current;
    if (SUCCEEDED(SHCreateItemFromParsingName(g.folder.c_str(), nullptr, __uuidof(IShellItem), current.PutVoid()))) {
        dialog->SetFolder(current.Get());
    }
    if (FAILED(dialog->Show(g.window))) return;
    ComPtr<IShellItem> result;
    PWSTR path = nullptr;
    if (SUCCEEDED(dialog->GetResult(result.Put())) && SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        g.folder = path;
        CoTaskMemFree(path);
        SetWindowTextW(g.folderEdit, g.folder.c_str());
        SaveSettings();
    }
}

void OpenFolder() {
    if (!g.lastFile.empty() && FileExists(g.lastFile)) {
        // Open Explorer with the latest recording selected.
        PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(g.lastFile.c_str());
        if (pidl) {
            SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
            ILFree(pidl);
            return;
        }
    }
    SHCreateDirectoryExW(nullptr, g.folder.c_str(), nullptr);
    ShellExecuteW(g.window, L"open", g.folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

HWND MakeControl(const wchar_t* className, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0) {
    return CreateWindowExW(exStyle, className, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, g.window,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g.instance, nullptr);
}

void CreateControls() {
    MakeControl(WC_STATICW, L"録音する音", SS_CENTERIMAGE, kSourceLabelId);
    g.source = MakeControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kSourceId);
    g.hint = MakeControl(WC_STATICW, L"", SS_NOPREFIX | SS_ENDELLIPSIS, kHintId);
    MakeControl(WC_STATICW, L"保存形式", SS_CENTERIMAGE, kFormatLabelId);
    g.format = MakeControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kFormatId);
    MakeControl(WC_STATICW, L"保存先", SS_CENTERIMAGE, kFolderLabelId);
    g.folderEdit = MakeControl(WC_EDITW, g.folder.c_str(), ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP, kFolderId,
                               WS_EX_CLIENTEDGE);
    g.browse = MakeControl(WC_BUTTONW, L"変更…", BS_PUSHBUTTON | WS_TABSTOP, kBrowseId);
    g.open = MakeControl(WC_BUTTONW, L"開く", BS_PUSHBUTTON | WS_TABSTOP, kOpenId);
    g.topMost = MakeControl(WC_BUTTONW, L"常に最前面に表示", BS_AUTOCHECKBOX | WS_TABSTOP, kTopMostId);
    g.record = MakeControl(WC_BUTTONW, L"", BS_PUSHBUTTON | WS_TABSTOP, kRecordId);
    MakeControl(WC_STATICW, L"L", SS_CENTER | SS_CENTERIMAGE, kLeftLabelId);
    g.meterLeft = MakeControl(WC_STATICW, L"", SS_OWNERDRAW, kMeterLeftId);
    MakeControl(WC_STATICW, L"R", SS_CENTER | SS_CENTERIMAGE, kRightLabelId);
    g.meterRight = MakeControl(WC_STATICW, L"", SS_OWNERDRAW, kMeterRightId);
    g.status = MakeControl(WC_STATICW, L"", SS_NOPREFIX | SS_ENDELLIPSIS | SS_CENTERIMAGE, kStatusId);

    for (const FormatChoice& choice : kFormats) {
        SendMessageW(g.format, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice.label));
    }
    SendMessageW(g.format, CB_SETCURSEL, g.formatIndex, 0);
    SendMessageW(g.topMost, BM_SETCHECK, g.alwaysOnTop ? BST_CHECKED : BST_UNCHECKED, 0);
}

void CreateFonts() {
    if (g.font) DeleteObject(g.font);
    if (g.bigFont) DeleteObject(g.bigFont);
    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, g.dpi);
    g.font = CreateFontIndirectW(&metrics.lfMessageFont);
    LOGFONTW big = metrics.lfMessageFont;
    big.lfHeight = big.lfHeight * 4 / 3;
    big.lfWeight = FW_SEMIBOLD;
    g.bigFont = CreateFontIndirectW(&big);

    for (HWND child = GetWindow(g.window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(child == g.record ? g.bigFont : g.font), TRUE);
    }
}

void Place(int id, int x, int y, int width, int height) {
    SetWindowPos(GetDlgItem(g.window, id), nullptr, Scale(x), Scale(y), Scale(width), Scale(height),
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void Layout() {
    int y = kMargin;
    Place(kSourceLabelId, kMargin, y, kLabelWidth, kRowHeight);
    Place(kSourceId, kFieldX, y, kRight - kFieldX, 320);  // height includes the drop-down list
    y += 30;
    Place(kHintId, kFieldX, y, kRight - kFieldX, 18);
    y += 26;
    Place(kFormatLabelId, kMargin, y, kLabelWidth, kRowHeight);
    Place(kFormatId, kFieldX, y, kRight - kFieldX, 200);
    y += 36;
    Place(kFolderLabelId, kMargin, y, kLabelWidth, kRowHeight);
    Place(kFolderId, kFieldX, y, kRight - 2 * kButtonWidth - 2 * kGap - kFieldX, kRowHeight);
    Place(kBrowseId, kRight - 2 * kButtonWidth - kGap, y, kButtonWidth, kRowHeight);
    Place(kOpenId, kRight - kButtonWidth, y, kButtonWidth, kRowHeight);
    y += 34;
    Place(kTopMostId, kFieldX, y, 200, 20);
    y += 28;
    Place(kRecordId, kMargin, y, kRight - kMargin, 46);
    y += 60;
    Place(kLeftLabelId, kMargin, y - 4, 14, 18);
    Place(kMeterLeftId, kMargin + 18, y, kRight - kMargin - 18, 10);
    y += 16;
    Place(kRightLabelId, kMargin, y - 4, 14, 18);
    Place(kMeterRightId, kMargin + 18, y, kRight - kMargin - 18, 10);
    y += 20;
    Place(kStatusId, kMargin, y, kRight - kMargin, 22);
}

void ResizeWindow() {
    RECT rc = {0, 0, Scale(kClientWidth), Scale(kClientHeight)};
    AdjustWindowRectExForDpi(&rc, kWindowStyle, FALSE, 0, g.dpi);
    SetWindowPos(g.window, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            g.window = hwnd;
            g.dpi = GetDpiForWindow(hwnd);
            CreateControls();
            CreateFonts();
            Layout();
            ResizeWindow();
            RefreshSources(g.sourceSetting);
            UpdateControls();
            SetStatus(L"待機中 ─ 「録音開始」を押すと録音します");
            return 0;

        case WM_DPICHANGED: {
            g.dpi = HIWORD(wParam);
            CreateFonts();
            Layout();
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);
            if (id == kRecordId && code == BN_CLICKED) {
                if (g.recording) {
                    StopRecording();
                } else {
                    StartRecording();
                }
            } else if (id == kSourceId && code == CBN_DROPDOWN) {
                const SourceItem current = CurrentSource();
                RefreshSources(current.system ? kSystemSource : current.app.exeName);
            } else if (id == kSourceId && code == CBN_SELCHANGE) {
                const SourceItem& current = CurrentSource();
                g.sourceSetting = current.system ? kSystemSource : current.app.exeName;
                UpdateHint();
                SaveSettings();
            } else if (id == kFormatId && code == CBN_SELCHANGE) {
                const LRESULT index = SendMessageW(g.format, CB_GETCURSEL, 0, 0);
                if (index >= 0) g.formatIndex = static_cast<int>(index);
                SaveSettings();
            } else if (id == kBrowseId && code == BN_CLICKED) {
                BrowseFolder();
            } else if (id == kOpenId && code == BN_CLICKED) {
                OpenFolder();
            } else if (id == kTopMostId && code == BN_CLICKED) {
                g.alwaysOnTop = SendMessageW(g.topMost, BM_GETCHECK, 0, 0) == BST_CHECKED;
                ApplyTopMost();
                SaveSettings();
            }
            return 0;
        }

        case WM_TIMER:
            if (wParam == kMeterTimer) OnMeterTimer();
            return 0;

        case kRecordingEndedMessage:
            if (g.recording) StopRecording();
            return 0;

        case WM_DRAWITEM: {
            const auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (dis->CtlID == kMeterLeftId) DrawMeter(dis, g.levelLeft);
            if (dis->CtlID == kMeterRightId) DrawMeter(dis, g.levelRight);
            return TRUE;
        }

        case WM_CTLCOLORSTATIC:
            if (reinterpret_cast<HWND>(lParam) == g.hint) {
                HDC dc = reinterpret_cast<HDC>(wParam);
                SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
                SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
            }
            break;

        case WM_CLOSE:
            if (g.recording) {
                const int answer = MessageBoxW(hwnd, L"録音中です。録音を停止して保存し、終了しますか？", kAppName,
                                               MB_YESNO | MB_ICONQUESTION);
                if (answer != IDYES) return 0;
                StopRecording();
            }
            SaveSettings();
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (g.font) DeleteObject(g.font);
            if (g.bigFont) DeleteObject(g.bigFont);
            g.font = g.bigFont = nullptr;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    g.instance = instance;
    LoadSettings();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    const DWORD exStyle = g.alwaysOnTop ? WS_EX_TOPMOST : 0;
    HWND window = CreateWindowExW(exStyle, kWindowClass, kAppName, kWindowStyle, CW_USEDEFAULT, CW_USEDEFAULT, 400,
                                  300, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (SUCCEEDED(coHr)) CoUninitialize();
    return static_cast<int>(msg.wParam);
}
