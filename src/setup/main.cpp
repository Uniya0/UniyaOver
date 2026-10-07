// Riot-inspired UniyaOver installer: one dark screen, path picker, single SFX exe.

#include "app_resources.h"
#include "gui/gdi_utils.h"
#include "gui/theme.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <windowsx.h>
#include <ShlObj.h>
#include <Shellapi.h>
#include <objbase.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

namespace {

using ir::gui::argb;
using ir::gui::draw_brand_uo_ring;
using ir::gui::draw_text;
using ir::gui::fill_round_rect;
using ir::gui::utf8_to_wide;
using ir::gui::wide_to_utf8;
namespace theme = ir::gui::theme;

constexpr int kWinW = 720;
constexpr int kWinH = 480;
constexpr wchar_t kAppName[] = L"UniyaOver";
constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\UniyaOver";
constexpr char kPackMagic[8] = {'U', 'O', 'P', 'A', 'C', 'K', '0', '1'};
constexpr char kSfxMagic[8] = {'U', 'O', 'S', 'F', 'X', '0', '0', '1'};

enum class Phase { Ready, Working, Done, Failed };

struct PackFile {
    std::string rel_utf8;
    std::vector<uint8_t> data;
};

HWND g_hwnd = nullptr;
ULONG_PTR g_gdiplus = 0;
Phase g_phase = Phase::Ready;
bool g_uninstall_mode = false;
float g_progress = 0.0f;
std::wstring g_status;
std::wstring g_detail;
std::wstring g_install_dir;
std::wstring g_error;
std::atomic<bool> g_busy{false};
RECT g_btn{};
RECT g_close{};
RECT g_browse{};
RECT g_secondary{};

std::wstring exe_path() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

std::wstring exe_dir() {
    std::wstring p = exe_path();
    const size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        p.resize(slash);
    }
    return p;
}

std::wstring default_install_dir() {
    wchar_t pf[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr, 0, pf))) {
        return L"C:\\Program Files\\UniyaOver";
    }
    return std::wstring(pf) + L"\\UniyaOver";
}

bool dir_exists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool ensure_dir(const std::wstring& path) {
    if (dir_exists(path)) {
        return true;
    }
    const int r = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS;
}

bool write_file(const std::wstring& path, const uint8_t* data, size_t len) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        ensure_dir(path.substr(0, slash));
    }
    HANDLE h = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok =
        WriteFile(h, data, static_cast<DWORD>(len), &written, nullptr) && written == len;
    CloseHandle(h);
    return ok != 0;
}

bool read_u32(std::ifstream& in, uint32_t* v) {
    return static_cast<bool>(in.read(reinterpret_cast<char*>(v), 4));
}

bool read_u64(std::ifstream& in, uint64_t* v) {
    return static_cast<bool>(in.read(reinterpret_cast<char*>(v), 8));
}

bool load_pack_from_stream(std::ifstream& in, std::vector<PackFile>* out) {
    char magic[8]{};
    if (!in.read(magic, 8) || memcmp(magic, kPackMagic, 8) != 0) {
        return false;
    }
    uint32_t count = 0;
    if (!read_u32(in, &count) || count == 0 || count > 100000) {
        return false;
    }
    out->clear();
    out->reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t path_len = 0;
        if (!read_u32(in, &path_len) || path_len == 0 || path_len > 4096) {
            return false;
        }
        PackFile f;
        f.rel_utf8.resize(path_len);
        if (!in.read(f.rel_utf8.data(), path_len)) {
            return false;
        }
        uint64_t data_len = 0;
        if (!read_u64(in, &data_len) || data_len > (8ull << 30)) {
            return false;
        }
        f.data.resize(static_cast<size_t>(data_len));
        if (data_len > 0 &&
            !in.read(reinterpret_cast<char*>(f.data.data()), static_cast<std::streamsize>(data_len))) {
            return false;
        }
        // Reject path traversal.
        if (f.rel_utf8.find("..") != std::string::npos) {
            return false;
        }
        out->push_back(std::move(f));
    }
    return true;
}

bool load_embedded_pack(std::vector<PackFile>* out) {
    const std::wstring self = exe_path();
    std::ifstream in(self, std::ios::binary);
    if (!in) {
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 24) {
        return false;
    }
    in.seekg(size - 16, std::ios::beg);
    uint64_t offset = 0;
    char magic[8]{};
    if (!read_u64(in, &offset) || !in.read(magic, 8)) {
        return false;
    }
    if (memcmp(magic, kSfxMagic, 8) != 0) {
        return false;
    }
    if (offset >= static_cast<uint64_t>(size)) {
        return false;
    }
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    return load_pack_from_stream(in, out);
}

bool load_pack_file(const std::wstring& path, std::vector<PackFile>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    return load_pack_from_stream(in, out);
}

bool browse_folder(HWND owner, std::wstring* selected) {
    IFileDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
    if (FAILED(hr) || !dlg) {
        return false;
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Choose install folder");
    hr = dlg->Show(owner);
    bool ok = false;
    if (SUCCEEDED(hr)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                *selected = path;
                while (!selected->empty() &&
                       (selected->back() == L'\\' || selected->back() == L'/')) {
                    selected->pop_back();
                }
                const bool already =
                    selected->size() >= 9 &&
                    _wcsicmp(selected->c_str() + selected->size() - 9, L"UniyaOver") == 0 &&
                    (selected->size() == 9 ||
                     (*selected)[selected->size() - 10] == L'\\' ||
                     (*selected)[selected->size() - 10] == L'/');
                if (!already) {
                    *selected += L"\\UniyaOver";
                }
                CoTaskMemFree(path);
                ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

bool create_shortcut(const std::wstring& link_path, const std::wstring& target, const std::wstring& work_dir, const std::wstring& icon) {
    IShellLinkW* sl = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&sl));
    if (FAILED(hr) || !sl) {
        return false;
    }
    sl->SetPath(target.c_str());
    sl->SetWorkingDirectory(work_dir.c_str());
    sl->SetDescription(kAppName);
    if (!icon.empty()) {
        sl->SetIconLocation(icon.c_str(), 0);
    }
    IPersistFile* pf = nullptr;
    hr = sl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&pf));
    bool ok = false;
    if (SUCCEEDED(hr) && pf) {
        ok = SUCCEEDED(pf->Save(link_path.c_str(), TRUE));
        pf->Release();
    }
    sl->Release();
    return ok;
}

std::wstring start_menu_dir() {
    wchar_t buf[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_PROGRAMS, nullptr, 0, buf))) {
        return {};
    }
    return std::wstring(buf) + L"\\UniyaOver";
}

void write_uninstall_registry(const std::wstring& install_dir, const std::wstring& setup_exe) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(
            HKEY_LOCAL_MACHINE,
            kUninstallKey,
            0,
            nullptr,
            0,
            KEY_WRITE | KEY_WOW64_64KEY,
            nullptr,
            &key,
            nullptr) != ERROR_SUCCESS) {
        return;
    }
    auto set_sz = [&](const wchar_t* name, const std::wstring& v) {
        RegSetValueExW(
            key,
            name,
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(v.c_str()),
            static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
    };
    set_sz(L"DisplayName", kAppName);
    set_sz(L"DisplayIcon", install_dir + L"\\assets\\uniyaover.ico");
    set_sz(L"InstallLocation", install_dir);
    set_sz(L"UninstallString", L"\"" + setup_exe + L"\" /uninstall");
    set_sz(L"Publisher", L"UNIYA");
    const DWORD one = 1;
    RegSetValueExW(key, L"NoModify", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
    RegSetValueExW(key, L"NoRepair", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
    RegCloseKey(key);
}

void remove_uninstall_registry() {
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, KEY_WOW64_64KEY, 0);
}

bool remove_tree(const std::wstring& path) {
    if (!dir_exists(path)) {
        return true;
    }
    std::wstring from = path;
    from.push_back(L'\0');
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    return SHFileOperationW(&op) == 0;
}

std::vector<std::wstring> list_files_recursive(const std::wstring& root) {
    std::vector<std::wstring> out;
    std::vector<std::wstring> stack{root};
    while (!stack.empty()) {
        const std::wstring dir = stack.back();
        stack.pop_back();
        const std::wstring pattern = dir + L"\\*";
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            continue;
        }
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
                continue;
            }
            const std::wstring full = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                stack.push_back(full);
            } else {
                out.push_back(full);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return out;
}

void set_ui(Phase phase, const std::wstring& status, const std::wstring& detail, float progress) {
    g_phase = phase;
    g_status = status;
    g_detail = detail;
    g_progress = (std::max)(0.0f, (std::min)(1.0f, progress));
    if (g_hwnd) {
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }
}

bool read_sfx_stub_size(const std::wstring& path, uint64_t* stub_size) {
    HANDLE h = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER li{};
    if (!GetFileSizeEx(h, &li) || li.QuadPart < 16) {
        CloseHandle(h);
        return false;
    }
    LARGE_INTEGER seek{};
    seek.QuadPart = li.QuadPart - 16;
    if (!SetFilePointerEx(h, seek, nullptr, FILE_BEGIN)) {
        CloseHandle(h);
        return false;
    }
    uint64_t offset = 0;
    char magic[8]{};
    DWORD n = 0;
    if (!ReadFile(h, &offset, 8, &n, nullptr) || n != 8 ||
        !ReadFile(h, magic, 8, &n, nullptr) || n != 8) {
        CloseHandle(h);
        return false;
    }
    CloseHandle(h);
    if (memcmp(magic, kSfxMagic, 8) != 0 || offset == 0 ||
        offset >= static_cast<uint64_t>(li.QuadPart)) {
        return false;
    }
    *stub_size = offset;
    return true;
}

bool write_uninstall_stub(const std::wstring& dst) {
    const std::wstring self = exe_path();
    uint64_t stub_size = 0;
    if (!read_sfx_stub_size(self, &stub_size)) {
        // Dev build / non-SFX stub: copy whole file (small).
        return CopyFileW(self.c_str(), dst.c_str(), FALSE) != 0;
    }
    HANDLE hin = CreateFileW(
        self.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hin == INVALID_HANDLE_VALUE) {
        return false;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(stub_size));
    DWORD got = 0;
    const BOOL ok_read =
        ReadFile(hin, buf.data(), static_cast<DWORD>(stub_size), &got, nullptr) &&
        got == stub_size;
    CloseHandle(hin);
    if (!ok_read) {
        return false;
    }
    return write_file(dst, buf.data(), buf.size());
}

void delete_files_except(const std::wstring& root, const std::wstring& keep) {
    auto files = list_files_recursive(root);
    for (const auto& f : files) {
        if (_wcsicmp(f.c_str(), keep.c_str()) == 0) {
            continue;
        }
        SetFileAttributesW(f.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(f.c_str());
    }
    std::vector<std::wstring> dirs;
    std::vector<std::wstring> stack{root};
    while (!stack.empty()) {
        const std::wstring dir = stack.back();
        stack.pop_back();
        const std::wstring pattern = dir + L"\\*";
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            continue;
        }
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
                continue;
            }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                const std::wstring full = dir + L"\\" + fd.cFileName;
                stack.push_back(full);
                dirs.push_back(full);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(dirs.begin(), dirs.end(), [](const std::wstring& a, const std::wstring& b) {
        return a.size() > b.size();
    });
    for (const auto& d : dirs) {
        RemoveDirectoryW(d.c_str());
    }
}

void schedule_final_cleanup(const std::wstring& install_dir, const std::wstring& self) {
    std::wstring args = L"/C ping 127.0.0.1 -n 2 >nul & del /f /q \"";
    args += self;
    args += L"\" & rd /s /q \"";
    args += install_dir;
    args += L"\"";
    ShellExecuteW(nullptr, L"open", L"cmd.exe", args.c_str(), nullptr, SW_HIDE);
}

void do_install() {
    g_busy = true;
    set_ui(Phase::Working, L"Installing", L"Reading package…", 0.02f);

    std::vector<PackFile> files;
    const std::wstring beside = exe_dir() + L"\\payload.uopk";
    bool ok = load_embedded_pack(&files);
    if (!ok) {
        ok = load_pack_file(beside, &files);
    }
    if (!ok || files.empty()) {
        g_error = L"Setup package not found (SFX payload missing).";
        set_ui(Phase::Failed, L"Install failed", g_error, 0.0f);
        g_busy = false;
        return;
    }

    if (!ensure_dir(g_install_dir)) {
        g_error = L"Cannot create install folder.";
        set_ui(Phase::Failed, L"Install failed", g_error, 0.0f);
        g_busy = false;
        return;
    }

    const size_t n = files.size();
    for (size_t i = 0; i < n; ++i) {
        const auto& f = files[i];
        std::wstring rel = utf8_to_wide(f.rel_utf8);
        for (auto& ch : rel) {
            if (ch == L'/') {
                ch = L'\\';
            }
        }
        const std::wstring dst = g_install_dir + L"\\" + rel;
        set_ui(
            Phase::Working,
            L"Installing",
            rel,
            0.05f + 0.85f * static_cast<float>(i) / static_cast<float>((std::max)(size_t{1}, n)));
        if (!write_file(dst, f.data.data(), f.data.size())) {
            g_error = L"Failed to write: " + rel;
            set_ui(Phase::Failed, L"Install failed", g_error, g_progress);
            g_busy = false;
            return;
        }
    }

    // Small stub only (~100KB), not the full 125MB SFX.
    const std::wstring setup_dst = g_install_dir + L"\\Uninstall.exe";
    if (!write_uninstall_stub(setup_dst)) {
        g_error = L"Failed to write Uninstall.exe";
        set_ui(Phase::Failed, L"Install failed", g_error, g_progress);
        g_busy = false;
        return;
    }

    const std::wstring menu = start_menu_dir();
    if (!menu.empty()) {
        ensure_dir(menu);
        create_shortcut(
            menu + L"\\UniyaOver.lnk",
            g_install_dir + L"\\UniyaOver.exe",
            g_install_dir,
            g_install_dir + L"\\assets\\uniyaover.ico");
    }

    write_uninstall_registry(g_install_dir, setup_dst);
    set_ui(Phase::Done, L"Ready", L"Installed to " + g_install_dir, 1.0f);
    g_busy = false;
}

void do_uninstall() {
    g_busy = true;
    set_ui(Phase::Working, L"Removing", L"Shortcuts…", 0.15f);

    const std::wstring self = exe_path();
    const std::wstring install = g_install_dir;

    const std::wstring menu = start_menu_dir();
    if (!menu.empty()) {
        remove_tree(menu);
    }
    remove_uninstall_registry();

    set_ui(Phase::Working, L"Removing", L"Program files…", 0.45f);
    delete_files_except(install, self);

    set_ui(Phase::Working, L"Removing", L"Finishing…", 0.85f);
    schedule_final_cleanup(install, self);

    set_ui(Phase::Done, L"Removed", L"UniyaOver was uninstalled.", 1.0f);
    g_busy = false;
}

void paint(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);

    Gdiplus::Graphics g(mem);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    Gdiplus::SolidBrush bg(argb(theme::kBg));
    g.FillRectangle(&bg, 0, 0, w, h);
    Gdiplus::LinearGradientBrush wash(
        Gdiplus::Point(0, 0),
        Gdiplus::Point(0, h),
        argb(0x33181818),
        argb(0x00000000));
    g.FillRectangle(&wash, 0, 0, w, h);

    g_close = {w - 44, 12, w - 14, 42};
    draw_text(
        g,
        L"✕",
        Gdiplus::RectF(
            static_cast<float>(g_close.left),
            static_cast<float>(g_close.top),
            30.0f,
            30.0f),
        theme::kTextDim,
        11.0f,
        false,
        Gdiplus::StringAlignmentCenter,
        Gdiplus::StringAlignmentCenter);

    const float mark = 88.0f;
    draw_brand_uo_ring(g, Gdiplus::RectF((w - mark) * 0.5f, 40.0f, mark, mark), theme::kText);
    draw_text(
        g,
        L"UniyaOver",
        Gdiplus::RectF(40.0f, 140.0f, static_cast<float>(w - 80), 34.0f),
        theme::kText,
        22.0f,
        true,
        Gdiplus::StringAlignmentCenter);
    draw_text(
        g,
        g_uninstall_mode ? L"Remove this app from your PC" : L"Instant replay for Windows",
        Gdiplus::RectF(60.0f, 176.0f, static_cast<float>(w - 120), 22.0f),
        theme::kTextMuted,
        10.5f,
        false,
        Gdiplus::StringAlignmentCenter);
    draw_text(
        g,
        L"© UNIYA",
        Gdiplus::RectF(60.0f, 198.0f, static_cast<float>(w - 120), 18.0f),
        theme::kTextMuted,
        8.5f,
        false,
        Gdiplus::StringAlignmentCenter);

    // Install path row (hidden while uninstalling / after done failure states keep showing path).
    const bool show_path = !g_uninstall_mode && g_phase == Phase::Ready;
    g_browse = {};
    if (show_path) {
        draw_text(
            g,
            L"Install location",
            Gdiplus::RectF(56.0f, 220.0f, static_cast<float>(w - 112), 18.0f),
            theme::kTextDim,
            9.0f,
            false);
        const float path_y = 242.0f;
        const float browse_w = 100.0f;
        const float path_w = static_cast<float>(w) - 112.0f - browse_w - 10.0f;
        fill_round_rect(g, Gdiplus::RectF(56.0f, path_y, path_w, 36.0f), 6.0f, theme::kCard);
        draw_text(
            g,
            g_install_dir,
            Gdiplus::RectF(68.0f, path_y, path_w - 24.0f, 36.0f),
            theme::kTextMuted,
            8.5f,
            false,
            Gdiplus::StringAlignmentNear,
            Gdiplus::StringAlignmentCenter);
        g_browse = {
            static_cast<LONG>(56.0f + path_w + 10.0f),
            static_cast<LONG>(path_y),
            static_cast<LONG>(56.0f + path_w + 10.0f + browse_w),
            static_cast<LONG>(path_y + 36.0f)};
        fill_round_rect(
            g,
            Gdiplus::RectF(
                static_cast<float>(g_browse.left),
                static_cast<float>(g_browse.top),
                browse_w,
                36.0f),
            6.0f,
            theme::kCardHover);
        draw_text(
            g,
            L"Browse",
            Gdiplus::RectF(
                static_cast<float>(g_browse.left),
                static_cast<float>(g_browse.top),
                browse_w,
                36.0f),
            theme::kText,
            9.5f,
            true,
            Gdiplus::StringAlignmentCenter,
            Gdiplus::StringAlignmentCenter);
    }

    draw_text(
        g,
        g_status.empty() ? (g_uninstall_mode ? L"Uninstall" : L"Install") : g_status,
        Gdiplus::RectF(48.0f, show_path ? 292.0f : 250.0f, static_cast<float>(w - 96), 22.0f),
        theme::kTextDim,
        10.0f,
        true,
        Gdiplus::StringAlignmentCenter);
    draw_text(
        g,
        g_detail.empty() ? g_install_dir : g_detail,
        Gdiplus::RectF(48.0f, show_path ? 314.0f : 272.0f, static_cast<float>(w - 96), 20.0f),
        theme::kTextMuted,
        8.5f,
        false,
        Gdiplus::StringAlignmentCenter);

    const float bar_y = show_path ? 348.0f : 308.0f;
    const float bar_x = 80.0f;
    const float bar_w = static_cast<float>(w) - 160.0f;
    fill_round_rect(g, Gdiplus::RectF(bar_x, bar_y, bar_w, 4.0f), 2.0f, theme::kBorder);
    if (g_progress > 0.001f) {
        fill_round_rect(
            g, Gdiplus::RectF(bar_x, bar_y, bar_w * g_progress, 4.0f), 2.0f, theme::kAccent);
    }

    const float btn_w = 220.0f;
    const float btn_h = 44.0f;
    const float btn_x = (static_cast<float>(w) - btn_w) * 0.5f;
    const float btn_y = show_path ? 372.0f : 340.0f;
    g_btn = {
        static_cast<LONG>(btn_x),
        static_cast<LONG>(btn_y),
        static_cast<LONG>(btn_x + btn_w),
        static_cast<LONG>(btn_y + btn_h)};

    std::wstring label = g_uninstall_mode ? L"UNINSTALL" : L"INSTALL";
    uint32_t btn_bg = theme::kText;
    uint32_t btn_fg = theme::kBg;
    if (g_phase == Phase::Working) {
        label = L"PLEASE WAIT";
        btn_bg = theme::kCard;
        btn_fg = theme::kTextDim;
    } else if (g_phase == Phase::Done) {
        label = g_uninstall_mode ? L"CLOSE" : L"LAUNCH";
    } else if (g_phase == Phase::Failed) {
        label = L"CLOSE";
        btn_bg = theme::kCardHover;
        btn_fg = theme::kText;
    }

    fill_round_rect(g, Gdiplus::RectF(btn_x, btn_y, btn_w, btn_h), 6.0f, btn_bg);
    draw_text(
        g,
        label,
        Gdiplus::RectF(btn_x, btn_y, btn_w, btn_h),
        btn_fg,
        11.0f,
        true,
        Gdiplus::StringAlignmentCenter,
        Gdiplus::StringAlignmentCenter);

    g_secondary = {};
    if (g_phase == Phase::Done && !g_uninstall_mode) {
        g_secondary = {
            static_cast<LONG>(btn_x),
            static_cast<LONG>(btn_y + btn_h + 6),
            static_cast<LONG>(btn_x + btn_w),
            static_cast<LONG>(btn_y + btn_h + 26)};
        draw_text(
            g,
            L"Open install folder",
            Gdiplus::RectF(
                static_cast<float>(g_secondary.left),
                static_cast<float>(g_secondary.top),
                btn_w,
                20.0f),
            theme::kTextMuted,
            8.5f,
            false,
            Gdiplus::StringAlignmentCenter);
    }

    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

bool hit(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

void on_primary() {
    if (g_busy) {
        return;
    }
    if (g_phase == Phase::Ready) {
        std::thread([] {
            if (g_uninstall_mode) {
                do_uninstall();
            } else {
                do_install();
            }
        }).detach();
        return;
    }
    if (g_phase == Phase::Done && !g_uninstall_mode) {
        const std::wstring exe = g_install_dir + L"\\UniyaOver.exe";
        ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, g_install_dir.c_str(), SW_SHOWNORMAL);
        PostQuitMessage(0);
        return;
    }
    if (g_phase == Phase::Done || g_phase == Phase::Failed) {
        PostQuitMessage(0);
    }
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_LBUTTONUP: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        if (hit(g_close, x, y)) {
            if (!g_busy) {
                PostQuitMessage(0);
            }
            return 0;
        }
        if (hit(g_browse, x, y) && !g_busy && g_phase == Phase::Ready) {
            std::wstring picked;
            if (browse_folder(hwnd, &picked)) {
                g_install_dir = picked;
                g_detail = g_install_dir;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        if (hit(g_btn, x, y)) {
            on_primary();
            return 0;
        }
        if (hit(g_secondary, x, y)) {
            ShellExecuteW(nullptr, L"open", g_install_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        return 0;
    }
    case WM_NCHITTEST: {
        LRESULT hit_test = DefWindowProcW(hwnd, msg, wp, lp);
        if (hit_test == HTCLIENT) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            if (!hit(g_btn, pt.x, pt.y) && !hit(g_close, pt.x, pt.y) && !hit(g_browse, pt.x, pt.y) &&
                !hit(g_secondary, pt.x, pt.y)) {
                return HTCAPTION;
            }
        }
        return hit_test;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmd, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::wstring cmd_line = cmd ? cmd : L"";
    for (auto& c : cmd_line) {
        c = static_cast<wchar_t>(towlower(c));
    }

    // Double-clicking Uninstall.exe has no args — detect by filename too.
    std::wstring self_name = exe_path();
    const size_t slash = self_name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        self_name = self_name.substr(slash + 1);
    }
    for (auto& c : self_name) {
        c = static_cast<wchar_t>(towlower(c));
    }

    g_uninstall_mode = cmd_line.find(L"/uninstall") != std::wstring::npos ||
        self_name == L"uninstall.exe";

    g_install_dir = default_install_dir();
    // Prefer the folder that contains this exe when uninstalling.
    if (g_uninstall_mode) {
        g_install_dir = exe_dir();
        HKEY key = nullptr;
        if (RegOpenKeyExW(
                HKEY_LOCAL_MACHINE, kUninstallKey, 0, KEY_READ | KEY_WOW64_64KEY, &key) ==
            ERROR_SUCCESS) {
            wchar_t buf[MAX_PATH]{};
            DWORD n = sizeof(buf);
            DWORD type = 0;
            if (RegQueryValueExW(
                    key,
                    L"InstallLocation",
                    nullptr,
                    &type,
                    reinterpret_cast<LPBYTE>(buf),
                    &n) == ERROR_SUCCESS &&
                type == REG_SZ && buf[0] != L'\0') {
                g_install_dir = buf;
            }
            RegCloseKey(key);
        }
    }

    g_status = g_uninstall_mode ? L"Uninstall" : L"Install";
    g_detail = g_install_dir;

    Gdiplus::GdiplusStartupInput gdi_in;
    if (Gdiplus::GdiplusStartup(&g_gdiplus, &gdi_in, nullptr) != Gdiplus::Ok) {
        return 1;
    }

    const wchar_t* cls = L"UniyaOverSetupWindow";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = cls;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
    RegisterClassExW(&wc);

    const int sx = GetSystemMetrics(SM_CXSCREEN);
    const int sy = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        cls,
        kAppName,
        WS_POPUP | WS_VISIBLE,
        (sx - kWinW) / 2,
        (sy - kWinH) / 2,
        kWinW,
        kWinH,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!hwnd) {
        Gdiplus::GdiplusShutdown(g_gdiplus);
        return 1;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Gdiplus::GdiplusShutdown(g_gdiplus);
    CoUninitialize();
    return 0;
}
