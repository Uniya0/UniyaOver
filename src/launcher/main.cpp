// Tiny root launcher: starts bin\UniyaOverCore.exe (DLLs live beside the core).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Shellapi.h>

#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmd_line, int show) {
    wchar_t module[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, module, MAX_PATH) == 0) {
        return 1;
    }
    std::wstring dir(module);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        dir.resize(slash);
    }

    const std::wstring core = dir + L"\\bin\\UniyaOverCore.exe";
    const DWORD attrs = GetFileAttributesW(core.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        MessageBoxW(
            nullptr,
            L"Missing bin\\UniyaOverCore.exe.\nReinstall UniyaOver or keep the folder layout intact.",
            L"UniyaOver",
            MB_OK | MB_ICONERROR);
        return 1;
    }

    std::wstring params = cmd_line ? cmd_line : L"";
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = core.c_str();
    sei.lpParameters = params.empty() ? nullptr : params.c_str();
    sei.lpDirectory = dir.c_str();
    sei.nShow = show ? show : SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        MessageBoxW(nullptr, L"Failed to start UniyaOverCore.exe", L"UniyaOver", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        return static_cast<int>(code);
    }
    return 0;
}
