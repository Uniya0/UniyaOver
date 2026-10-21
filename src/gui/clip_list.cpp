#include "gui/clip_list.h"

#include "gui/gdi_utils.h"
#include "util/i18n.h"
#include "util/paths.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace ir::gui {
namespace {

std::wstring format_size(uint64_t bytes) {
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    wchar_t buf[64];
    if (mb >= 100.0) {
        std::swprintf(buf, 64, L"%.0f MB", mb);
    } else {
        std::swprintf(buf, 64, L"%.1f MB", mb);
    }
    return buf;
}

std::wstring format_filetime(const FILETIME& ft) {
    FILETIME local{};
    SYSTEMTIME st{};
    FileTimeToLocalFileTime(&ft, &local);
    FileTimeToSystemTime(&local, &st);
    wchar_t buf[64];
    std::swprintf(
        buf,
        64,
        L"%02u.%02u.%04u %02u:%02u",
        st.wDay,
        st.wMonth,
        st.wYear,
        st.wHour,
        st.wMinute);
    return buf;
}

uint64_t filetime_to_u64(const FILETIME& ft) {
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

}  // namespace

std::wstring clips_dir_wide() {
    return utf8_to_wide(ir::paths::clips_directory());
}

std::vector<ClipInfo> scan_clips(int max_count) {
    std::vector<ClipInfo> out;
    const std::wstring dir = clips_dir_wide();
    if (dir.empty()) {
        return out;
    }

    const std::wstring pattern = dir + L"\\*.mp4";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return out;
    }

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        ClipInfo c;
        c.name = fd.cFileName;
        c.path = dir + L"\\" + c.name;
        c.size_bytes =
            (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        c.size_display = format_size(c.size_bytes);
        c.date_display = format_filetime(fd.ftLastWriteTime);
        c.mtime_q = filetime_to_u64(fd.ftLastWriteTime);
        out.push_back(std::move(c));
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(out.begin(), out.end(), [](const ClipInfo& a, const ClipInfo& b) {
        return a.mtime_q > b.mtime_q;
    });
    if (max_count > 0 && static_cast<int>(out.size()) > max_count) {
        out.resize(static_cast<size_t>(max_count));
    }
    return out;
}

std::wstring free_space_display(const std::wstring& path_on_volume) {
    ULARGE_INTEGER avail{};
    ULARGE_INTEGER total{};
    ULARGE_INTEGER free_bytes{};
    if (!GetDiskFreeSpaceExW(path_on_volume.c_str(), &avail, &total, &free_bytes)) {
        return L"—";
    }
    const double gb = static_cast<double>(avail.QuadPart) / (1024.0 * 1024.0 * 1024.0);
    wchar_t buf[64];
    std::swprintf(buf, 64, ir::i18n::tr(L"Свободно: %.1f ГБ", L"Free: %.1f GB"), gb);
    return buf;
}

bool open_clip(const std::wstring& path) {
    const HINSTANCE r = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32;
}

bool reveal_clip(const std::wstring& path) {
    std::wstring params = L"/select,\"" + path + L"\"";
    const HINSTANCE r =
        ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32;
}

bool open_clips_folder() {
    const std::wstring dir = clips_dir_wide();
    const HINSTANCE r =
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32;
}

}  // namespace ir::gui
