#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace ws {

// An application that can be recorded on its own.
struct AudioApp {
    DWORD pid = 0;          // top-most process of the app (e.g. Chrome's browser process)
    std::wstring exeName;   // "chrome.exe"
    std::wstring label;     // "Google Chrome"
};

// Apps that currently own an audio session, plus well-known browsers that are
// running. Requires COM to be initialized on the calling thread.
std::vector<AudioApp> ListAudioApps();

// Finds the running app with the given executable name. Returns 0 when it is
// not running. `preferredPid` wins if it still belongs to that app.
DWORD FindAppProcess(const std::wstring& exeName, DWORD preferredPid);

}  // namespace ws
