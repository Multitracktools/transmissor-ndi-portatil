#include <windows.h>
#include <commctrl.h>

#include <iterator>

namespace {
constexpr wchar_t kMainClass[] = L"TransmissorNDIPortatilV3";
constexpr int kIdProtected = 1007;
constexpr int kIdQuick = 1008;
constexpr int kIdHelp = 1015;
constexpr int kIdAbout = 1501;
constexpr int kIdPermissionHeading = 1510;

HWND gMain{};
HWND gPermissionHeading{};
HFONT gCardBodyFont{};
HFONT gHeadingFont{};
HHOOK gHook{};

void refreshAboutButton() {
    if (!gMain) return;
    if (HWND about = GetDlgItem(gMain, kIdAbout)) {
        SetWindowPos(about, HWND_TOP, 708, 18, 120, 34,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        RedrawWindow(about, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME);
    }
    if (HWND help = GetDlgItem(gMain, kIdHelp)) {
        SetWindowPos(help, HWND_TOP, 842, 18, 130, 34,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

void applyCardTextFix() {
    if (!gMain) return;
    if (!gCardBodyFont) {
        LOGFONTW lf{};
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        lf.lfHeight = -14;
        lf.lfWeight = FW_NORMAL;
        gCardBodyFont = CreateFontIndirectW(&lf);
    }
    for (int id : {kIdProtected, kIdQuick}) {
        if (HWND card = GetDlgItem(gMain, id)) {
            SendMessageW(card, WM_SETFONT, reinterpret_cast<WPARAM>(gCardBodyFont), TRUE);
            InvalidateRect(card, nullptr, TRUE);
        }
    }
}

void createPermissionHeading() {
    if (!gMain || gPermissionHeading) return;
    if (!gHeadingFont) {
        LOGFONTW lf{};
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        lf.lfHeight = -16;
        lf.lfWeight = FW_SEMIBOLD;
        gHeadingFont = CreateFontIndirectW(&lf);
    }
    // Cobre apenas o título desenhado pelo layout original e o reposiciona com
    // espaço visual entre os cards e as permissões. Nenhum ComboBox é tocado.
    gPermissionHeading = CreateWindowExW(0, L"STATIC", L"Permitir envio durante esta execução",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        28, 500, 330, 34, gMain,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdPermissionHeading)),
        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(gPermissionHeading, WM_SETFONT,
                 reinterpret_cast<WPARAM>(gHeadingFont), TRUE);
    SetWindowPos(gPermissionHeading, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

LRESULT CALLBACK mainSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                             UINT_PTR, DWORD_PTR) {
    if (msg == WM_PAINT) {
        const LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
        // brand_ui pinta o cabeçalho depois dos filhos; redesenha apenas os dois
        // botões do cabeçalho e nosso título de permissões por cima.
        refreshAboutButton();
        if (gPermissionHeading)
            RedrawWindow(gPermissionHeading, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_UPDATENOW);
        return result;
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, mainSubclass, 81);
        gMain = nullptr;
        gPermissionHeading = nullptr;
        if (gCardBodyFont) { DeleteObject(gCardBodyFont); gCardBodyFont = nullptr; }
        if (gHeadingFont) { DeleteObject(gHeadingFont); gHeadingFont = nullptr; }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void install(HWND hwnd) {
    if (gMain || !hwnd) return;
    gMain = hwnd;
    applyCardTextFix();
    createPermissionHeading();
    SetWindowSubclass(hwnd, mainSubclass, 81, 0);
    refreshAboutButton();
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
