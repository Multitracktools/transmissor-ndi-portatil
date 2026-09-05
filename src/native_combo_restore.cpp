#include <windows.h>
#include <commctrl.h>

#include <iterator>

namespace {
constexpr wchar_t kMainClass[] = L"TransmissorNDIPortatilV3";
constexpr UINT_PTR kRestoreTimer = 98;

HWND gMain{};
HHOOK gHook{};

BOOL CALLBACK restoreChild(HWND child, LPARAM) {
    wchar_t cls[64]{};
    GetClassNameW(child, cls, static_cast<int>(std::size(cls)));

    // ComboBox e Edit voltam ao comportamento nativo do Windows. Mantemos a propriedade
    // usada por brand_ui.cpp para que o timer de estilo não tente arredondá-los novamente.
    if (_wcsicmp(cls, WC_COMBOBOXW) == 0 || _wcsicmp(cls, L"Edit") == 0) {
        SetWindowRgn(child, nullptr, TRUE);
        SetPropW(child, L"ZosmaPremium", reinterpret_cast<HANDLE>(1));
        RedrawWindow(child, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    }
    return TRUE;
}

void restore(HWND hwnd) {
    if (!hwnd) return;
    EnumChildWindows(hwnd, restoreChild, 0);
}

LRESULT CALLBACK mainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_TIMER && wp == kRestoreTimer) {
        KillTimer(hwnd, kRestoreTimer);
        restore(hwnd);
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        KillTimer(hwnd, kRestoreTimer);
        RemoveWindowSubclass(hwnd, mainProc, 81);
        gMain = nullptr;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void install(HWND hwnd) {
    if (gMain || !hwnd) return;
    gMain = hwnd;
    SetWindowSubclass(hwnd, mainProc, 81, 0);
    // brand_ui e protected_ip_ui criam controles via hooks; esperamos todos existirem.
    SetTimer(hwnd, kRestoreTimer, 700, nullptr);
}

LRESULT CALLBACK hookProc(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && !gMain) {
        const auto* data = reinterpret_cast<CWPSTRUCT*>(lp);
        if (data && data->hwnd && data->message == WM_PAINT) {
            wchar_t className[128]{};
            GetClassNameW(data->hwnd, className, static_cast<int>(std::size(className)));
            if (wcscmp(className, kMainClass) == 0) install(data->hwnd);
        }
    }
    return CallNextHookEx(gHook, code, wp, lp);
}

struct Bootstrap {
    Bootstrap() { gHook = SetWindowsHookExW(WH_CALLWNDPROC, hookProc, nullptr, GetCurrentThreadId()); }
    ~Bootstrap() { if (gHook) UnhookWindowsHookEx(gHook); }
} gBootstrap;
}
