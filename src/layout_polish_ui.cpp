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

constexpr int kIdAudioCover = 1601;
constexpr int kIdAudioLabel = 1602;
constexpr int kIdAudioHint = 1603;

HWND gMain{};
HWND gAudioCover{};
HWND gAudioLabel{};
HWND gAudioHint{};
HHOOK gHook{};
bool gSubclassInstalled{false};

void moveControl(int id, int x, int y, int w, int h) {
    if (!gMain) return;
    if (HWND control = GetDlgItem(gMain, id)) {
        SetWindowPos(control, nullptr, x, y, w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
}

void addEditMargins(HWND edit) {
    if (!edit) return;
    SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
}

void ensureAudioCaption() {
    if (!gMain || gAudioCover) return;

    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    if (HWND source = GetDlgItem(gMain, kIdSourceName)) {
        HFONT candidate = reinterpret_cast<HFONT>(SendMessageW(source, WM_GETFONT, 0, 0));
        if (candidate) font = candidate;
    }

    // Cobre o aviso antigo desta faixa e cria uma hierarquia própria para a saída de áudio.
    gAudioCover = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
        28, 604, 492, 58, gMain,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAudioCover)), GetModuleHandleW(nullptr), nullptr);
    gAudioLabel = CreateWindowExW(0, L"STATIC", L"Saída de áudio", WS_CHILD | WS_VISIBLE | SS_LEFT,
        34, 604, 200, 20, gMain,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAudioLabel)), GetModuleHandleW(nullptr), nullptr);
    gAudioHint = CreateWindowExW(0, L"STATIC", L"A escolha será lembrada na próxima execução.",
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        224, 604, 288, 20, gMain,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAudioHint)), GetModuleHandleW(nullptr), nullptr);

    SendMessageW(gAudioCover, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(gAudioLabel, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(gAudioHint, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void applyLayout() {
    if (!gMain) return;

    // Mais respiro entre rótulos e controles na coluna de configuração.
    moveControl(kIdSourceName, 34, 216, 480, 34);
    moveControl(kIdCaptureKind, 34, 296, 150, 220);
    moveControl(kIdCaptureSource, 194, 296, 270, 260);
    moveControl(kIdRefresh, 474, 296, 40, 34);
    moveControl(kIdFps, 34, 364, 125, 120);
    moveControl(kIdCursor, 178, 366, 170, 28);

    // Os cards agora começam abaixo do título, sem cortar o texto.
    moveControl(kIdProtected, 34, 430, 230, 70);
    moveControl(kIdQuick, 276, 430, 238, 70);
    moveControl(kIdWhatsApp, 34, 542, 480, 32);
    moveControl(kIdTelegram, 34, 580, 480, 32);

    // Saída de áudio ganha título próprio e não fica espremida no rodapé do card.
    moveControl(kIdAudioCombo, 34, 626, 480, 210);

    // Campo de IP com alinhamento consistente.
    moveControl(kIdIpLabel, 574, 207, 108, 20);
    moveControl(kIdIpEdit, 688, 202, 254, 32);
    moveControl(kIdIpHint, 574, 241, 368, 20);

    // Espaço real entre o bloco verde de privacidade e a referência de captura.
    moveControl(kIdReferenceTitle, 574, 452, 220, 20);
    moveControl(kIdMonitorMessage, 574, 478, 368, 162);
    moveControl(kIdPreview, 574, 478, 368, 162);

    // Mantém Sobre e Como usar alinhados no cabeçalho.
    moveControl(kIdAbout, 708, 18, 120, 34);
    moveControl(kIdHelp, 842, 18, 130, 34);

    addEditMargins(GetDlgItem(gMain, kIdSourceName));
    addEditMargins(GetDlgItem(gMain, kIdIpEdit));

    if (HWND combo = GetDlgItem(gMain, kIdAudioCombo)) SetWindowTheme(combo, L"DarkMode_Explorer", nullptr);
    if (HWND edit = GetDlgItem(gMain, kIdSourceName)) SetWindowTheme(edit, L"DarkMode_Explorer", nullptr);
    if (HWND edit = GetDlgItem(gMain, kIdIpEdit)) SetWindowTheme(edit, L"DarkMode_Explorer", nullptr);

    ensureAudioCaption();

    // O cabeçalho premium é pintado depois dos filhos; força os botões a ficarem por cima dele.
    if (HWND about = GetDlgItem(gMain, kIdAbout)) {
        SetWindowPos(about, HWND_TOP, 708, 18, 120, 34, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        RedrawWindow(about, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME);
    }
    if (HWND help = GetDlgItem(gMain, kIdHelp)) {
        SetWindowPos(help, HWND_TOP, 842, 18, 130, 34, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        RedrawWindow(help, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME);
    }

    for (HWND child : {gAudioCover, gAudioLabel, gAudioHint, GetDlgItem(gMain, kIdAudioCombo),
                       GetDlgItem(gMain, kIdReferenceTitle), GetDlgItem(gMain, kIdMonitorMessage),
                       GetDlgItem(gMain, kIdPreview)}) {
        if (child && IsWindowVisible(child)) {
            SetWindowPos(child, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            RedrawWindow(child, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        }
    }
}

LRESULT CALLBACK polishProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_TIMER && wp == kLayoutTimer) {
        applyLayout();
        return 0;
    }
    if (msg == WM_PAINT) {
        const LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
        // brand_ui.cpp repinta o cabeçalho usando GetDC; redesenha os filhos depois dele.
        if (HWND about = GetDlgItem(hwnd, kIdAbout))
            RedrawWindow(about, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        if (HWND help = GetDlgItem(hwnd, kIdHelp))
            RedrawWindow(help, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        for (HWND child : {gAudioCover, gAudioLabel, gAudioHint})
            if (child) RedrawWindow(child, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        return result;
    }
    if (msg == WM_NCDESTROY) {
        KillTimer(hwnd, kLayoutTimer);
        RemoveWindowSubclass(hwnd, polishProc, 79);
        gMain = nullptr;
        gAudioCover = nullptr;
        gAudioLabel = nullptr;
        gAudioHint = nullptr;
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
    // Instala depois dos outros helpers de UI, garantindo que este acabamento seja o último.
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
