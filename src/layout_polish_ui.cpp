#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>

#include <iterator>

namespace {
constexpr wchar_t kMainClass[] = L"TransmissorNDIPortatilV3";
constexpr UINT_PTR kInstallTimer = 96;
constexpr UINT_PTR kLayoutTimer = 97;

constexpr int kIdSourceName = 1001;
constexpr int kIdCaptureKind = 1002;
constexpr int kIdCaptureSource = 1003;
constexpr int kIdRefresh = 1004;
constexpr int kIdFps = 1005;
constexpr int kIdCursor = 1006;
constexpr int kIdProtected = 1007;
constexpr int kIdQuick = 1008;
constexpr int kIdWhatsApp = 1009;
constexpr int kIdTelegram = 1010;
constexpr int kIdRelease = 1012;
constexpr int kIdHelp = 1015;
constexpr int kIdPreview = 1016;
constexpr int kIdIpEdit = 1201;
constexpr int kIdIpLabel = 1202;
constexpr int kIdIpHint = 1203;
constexpr int kIdAudioCombo = 1210;
constexpr int kIdReferenceTitle = 1301;
constexpr int kIdMonitorMessage = 1302;
constexpr int kIdAbout = 1501;
constexpr int kIdAudioLabel = 1602;
constexpr int kIdAudioHint = 1603;

HWND gMain{};
HWND gAudioLabel{};
HWND gAudioHint{};
HHOOK gHook{};
bool gSubclassInstalled{false};

void moveControl(int id, int x, int y, int w, int h) {
    if (HWND c = gMain ? GetDlgItem(gMain, id) : nullptr)
        SetWindowPos(c, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

void addEditMargins(HWND edit) {
    if (edit) SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
}

void ensureAudioCaption() {
    if (!gMain || gAudioLabel) return;
    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    if (HWND source = GetDlgItem(gMain, kIdSourceName)) {
        if (HFONT candidate = reinterpret_cast<HFONT>(SendMessageW(source, WM_GETFONT, 0, 0))) font = candidate;
    }
    gAudioLabel = CreateWindowExW(0, L"STATIC", L"Saída de áudio", WS_CHILD | WS_VISIBLE | SS_LEFT,
        34, 608, 180, 20, gMain, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAudioLabel)), GetModuleHandleW(nullptr), nullptr);
    gAudioHint = CreateWindowExW(0, L"STATIC", L"A escolha será lembrada na próxima execução.", WS_CHILD | WS_VISIBLE | SS_RIGHT,
        214, 608, 300, 20, gMain, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAudioHint)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(gAudioLabel, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(gAudioHint, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void applyLayout() {
    if (!gMain) return;

    // Mantém tudo dentro do card esquerdo (y <= 678) e reserva respiro real entre blocos.
    moveControl(kIdSourceName, 34, 216, 480, 34);
    moveControl(kIdCaptureKind, 34, 296, 150, 180);
    moveControl(kIdCaptureSource, 194, 296, 270, 220);
    moveControl(kIdRefresh, 474, 296, 40, 34);
    moveControl(kIdFps, 34, 364, 125, 100);
    moveControl(kIdCursor, 178, 366, 170, 28);

    // Cards mais altos para que as duas linhas de descrição nunca sejam cortadas.
    moveControl(kIdProtected, 34, 430, 230, 82);
    moveControl(kIdQuick, 276, 430, 238, 82);

    // Permissões sob os cards, com espaço para o título original do grupo.
    moveControl(kIdWhatsApp, 34, 548, 480, 28);
    moveControl(kIdTelegram, 34, 582, 480, 28);

    // ComboBox: a altura passada ao Win32 inclui a lista suspensa. 120px mantém a caixa
    // fechada normal e ainda dá espaço suficiente ao dropdown, sem invadir o rodapé.
    moveControl(kIdAudioCombo, 34, 632, 480, 120);

    moveControl(kIdIpLabel, 574, 207, 108, 20);
    moveControl(kIdIpEdit, 688, 202, 254, 32);
    moveControl(kIdIpHint, 574, 241, 368, 20);

    // Afasta a referência do banner verde para não haver sobreposição visual.
    moveControl(kIdReferenceTitle, 574, 486, 220, 20);
    moveControl(kIdMonitorMessage, 574, 512, 368, 126);
    moveControl(kIdPreview, 574, 512, 368, 126);

    moveControl(kIdAbout, 708, 18, 120, 34);
    moveControl(kIdHelp, 842, 18, 130, 34);

    addEditMargins(GetDlgItem(gMain, kIdSourceName));
    addEditMargins(GetDlgItem(gMain, kIdIpEdit));
    if (HWND combo = GetDlgItem(gMain, kIdAudioCombo)) {
        SetWindowTheme(combo, L"Explorer", nullptr);
        SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), 22);
        SendMessageW(combo, CB_SETITEMHEIGHT, 0, 22);
    }
    ensureAudioCaption();

    for (HWND child : {GetDlgItem(gMain, kIdAbout), GetDlgItem(gMain, kIdHelp), gAudioLabel, gAudioHint,
                       GetDlgItem(gMain, kIdAudioCombo), GetDlgItem(gMain, kIdReferenceTitle),
                       GetDlgItem(gMain, kIdMonitorMessage), GetDlgItem(gMain, kIdPreview)}) {
        if (child && IsWindowVisible(child)) {
            SetWindowPos(child, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            RedrawWindow(child, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        }
    }
}

LRESULT CALLBACK polishProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_TIMER && wp == kLayoutTimer) { applyLayout(); return 0; }
    if (msg == WM_PAINT) {
        const LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
        for (HWND child : {GetDlgItem(hwnd, kIdAbout), GetDlgItem(hwnd, kIdHelp), gAudioLabel, gAudioHint})
            if (child) RedrawWindow(child, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        return result;
    }
    if (msg == WM_NCDESTROY) {
        KillTimer(hwnd, kLayoutTimer);
        RemoveWindowSubclass(hwnd, polishProc, 79);
        gMain = gAudioLabel = gAudioHint = nullptr;
        gSubclassInstalled = false;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

VOID CALLBACK delayedInstall(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    KillTimer(hwnd, id);
    if (!gMain || hwnd != gMain || gSubclassInstalled) return;
    gSubclassInstalled = SetWindowSubclass(hwnd, polishProc, 79, 0) != FALSE;
    applyLayout();
    SetTimer(hwnd, kLayoutTimer, 1000, nullptr);
    InvalidateRect(hwnd, nullptr, TRUE);
}

void scheduleInstall(HWND hwnd) {
    if (gMain || !hwnd) return;
    gMain = hwnd;
    SetTimer(hwnd, kInstallTimer, 250, delayedInstall);
}

LRESULT CALLBACK hookProc(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && !gMain) {
        const auto* data = reinterpret_cast<CWPSTRUCT*>(lp);
        if (data && data->hwnd && data->message == WM_PAINT) {
            wchar_t className[128]{};
            GetClassNameW(data->hwnd, className, static_cast<int>(std::size(className)));
            if (wcscmp(className, kMainClass) == 0) scheduleInstall(data->hwnd);
        }
    }
    return CallNextHookEx(gHook, code, wp, lp);
}

struct Bootstrap {
    Bootstrap() { gHook = SetWindowsHookExW(WH_CALLWNDPROC, hookProc, nullptr, GetCurrentThreadId()); }
    ~Bootstrap() { if (gHook) UnhookWindowsHookEx(gHook); }
} gBootstrap;
}
