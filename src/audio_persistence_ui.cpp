#include <windows.h>
#include <commctrl.h>

#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

namespace {
constexpr wchar_t kMainWindowClass[] = L"TransmissorNDIPortatilV3";
constexpr int kAudioComboId = 1210;
constexpr UINT_PTR kAudioPersistenceTimer = 94;

HWND gMain{};
HHOOK gHook{};
bool gRestored{false};

std::filesystem::path iniPath() {
    std::vector<wchar_t> buffer(32768);
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return std::filesystem::path(std::wstring(buffer.data(), len)).parent_path() / L"transmissor-ndi.ini";
}

std::wstring comboItemText(HWND combo, int index) {
    const LRESULT len = SendMessageW(combo, CB_GETLBTEXTLEN, index, 0);
    if (len == CB_ERR || len < 0) return {};
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    SendMessageW(combo, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text.data()));
    text.resize(static_cast<size_t>(len));
    return text;
}

std::wstring savedAudioLabel() {
    wchar_t value[1024]{};
    GetPrivateProfileStringW(L"app", L"audioOutputLabel", L"", value,
                             static_cast<DWORD>(std::size(value)), iniPath().c_str());
    return value;
}

void saveAudioLabel(HWND combo) {
    if (!combo) return;
    const int selected = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    const std::wstring label = selected >= 0 ? comboItemText(combo, selected) : L"";
    WritePrivateProfileStringW(L"app", L"audioOutputLabel", label.c_str(), iniPath().c_str());
}

bool restoreAudioSelection() {
    if (!gMain) return false;
    HWND combo = GetDlgItem(gMain, kAudioComboId);
    if (!combo) return false;
    const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    if (count <= 0) return false;

    const std::wstring wanted = savedAudioLabel();
    int selected = 0;
    if (!wanted.empty()) {
        for (int i = 0; i < count; ++i) {
            if (comboItemText(combo, i) == wanted) {
                selected = i;
                break;
            }
        }
    }

    SendMessageW(combo, CB_SETCURSEL, selected, 0);
    SendMessageW(gMain, WM_COMMAND, MAKEWPARAM(kAudioComboId, CBN_SELCHANGE), reinterpret_cast<LPARAM>(combo));
    saveAudioLabel(combo);
    return true;
}

LRESULT CALLBACK subclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_TIMER && wp == kAudioPersistenceTimer) {
        if (!gRestored) gRestored = restoreAudioSelection();
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == kAudioComboId && HIWORD(wp) == CBN_SELCHANGE) {
        const LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
        HWND combo = GetDlgItem(hwnd, kAudioComboId);
        saveAudioLabel(combo);
        return result;
    }
    if (msg == WM_NCDESTROY) {
        KillTimer(hwnd, kAudioPersistenceTimer);
        RemoveWindowSubclass(hwnd, subclassProc, 8);
        gMain = nullptr;
        gRestored = false;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void install(HWND hwnd) {
    if (gMain || !hwnd) return;
    gMain = hwnd;
    SetWindowSubclass(hwnd, subclassProc, 8, 0);
    SetTimer(hwnd, kAudioPersistenceTimer, 250, nullptr);
}

LRESULT CALLBACK callWndHook(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && !gMain) {
        const auto* data = reinterpret_cast<CWPSTRUCT*>(lp);
        if (data && data->hwnd && data->message == WM_PAINT) {
            wchar_t className[128]{};
            GetClassNameW(data->hwnd, className, static_cast<int>(std::size(className)));
            if (wcscmp(className, kMainWindowClass) == 0) install(data->hwnd);
        }
    }
    return CallNextHookEx(gHook, code, wp, lp);
}

struct Bootstrap {
    Bootstrap() { gHook = SetWindowsHookExW(WH_CALLWNDPROC, callWndHook, nullptr, GetCurrentThreadId()); }
    ~Bootstrap() { if (gHook) UnhookWindowsHookEx(gHook); }
} gBootstrap;
}
