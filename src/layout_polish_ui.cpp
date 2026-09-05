#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>

#include <iterator>

namespace {
constexpr wchar_t kMainClass[] = L"TransmissorNDIPortatilV3";
constexpr UINT_PTR kInstallTimer = 96;

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

HWND gMain{};
HWND gAudioCover{};
HWND gAudioLabel{};
HHOOK gHook{};
bool gSubclassInstalled{false};

void moveControl(int id, int x, int y, int w, int h) {
    if (HWND c = gMain ? GetDlgItem(gMain, id) : nullptr)
        SetWindowPos(c, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

void addEditMargins(HWND edit) {
    if (edit) SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
}

void normalizeCombo(HWND combo) {
    if (!combo) return;
    // O arredondamento via SetWindowRgn funciona bem em botões, mas em ComboBox pode
    // recortar a caixa e provocar repinturas/piscadas. Restauramos a região nativa.
    SetWindowRgn(combo, nullptr, TRUE);
    SetWindowTheme(combo, L"DarkMode_Explorer", nullptr);
    SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), 22);
    SendMessageW(combo, CB_SETITEMHEIGHT, 0, 22);
}

void ensureAudioArea() {
    if (!gMain || gAudioCover) return;
    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    if (HWND source = GetDlgItem(gMain, kIdSourceName)) {
        if (HFONT candidate = reinterpret_cast<HFONT>(SendMessageW(source, WM_GETFONT, 0, 0))) font = candidate;
    }

    // Cobre apenas o texto antigo pintado em y=620. O combo fica acima deste cover na ordem Z.
    gAudioCover = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
        28, 603, 492, 51, gMain,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAudioCover)), GetModuleHandleW(nullptr), nullptr);
    gAudioLabel = CreateWindowExW(0, L"STATIC", L"Saída de áudio", WS_CHILD | WS_VISIBLE | SS_LEFT,
        34, 604, 180, 20, gMain,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAudioLabel)), GetModuleHandleW(nullptr), nullptr);

    SendMessageW(gAudioCover, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(gAudioLabel, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void bringToFront(HWND child) {
    if (!child || !IsWindowVisible(child)) return;
    SetWindowPos(child, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    RedrawWindow(child, nullptr, nullptr, RDW_INVALIDATE);
}

void applyLayout() {
    if (!gMain) return;

    moveControl(kIdSourceName, 34, 216, 480, 34);
    moveControl(kIdCaptureKind, 34, 296, 150, 180);
    moveControl(kIdCaptureSource, 194, 296, 270, 220);
    moveControl(kIdRefresh, 474, 296, 40, 34);
    moveControl(kIdFps, 34, 364, 125, 100);
    moveControl(kIdCursor, 178, 366, 170, 28);

    // Estes cards já ficaram aprovados visualmente; mantemos as dimensões.
    moveControl(kIdProtected, 34, 430, 230, 82);
    moveControl(kIdQuick, 276, 430, 238, 82);

    // Compacta um pouco as permissões para reservar uma linha limpa ao áudio.
    moveControl(kIdWhatsApp, 34, 544, 480, 27);
    moveControl(kIdTelegram, 34, 575, 480, 27);

    // A parte visível do combo termina antes do fim do card (654px).
    moveControl(kIdAudioCombo, 34, 625, 480, 110);

    moveControl(kIdIpLabel, 574, 207, 108, 20);
    moveControl(kIdIpEdit, 688, 202, 254, 32);
    moveControl(kIdIpHint, 574, 241, 368, 20);

    moveControl(kIdReferenceTitle, 574, 486, 220, 20);
    moveControl(kIdMonitorMessage, 574, 512, 368, 126);
    moveControl(kIdPreview, 574, 512, 368, 126);

    moveControl(kIdAbout, 708, 18, 120, 34);
    moveControl(kIdHelp, 842, 18, 130, 34);

    addEditMargins(GetDlgItem(gMain, kIdSourceName));
    addEditMargins(GetDlgItem(gMain, kIdIpEdit));

    normalizeCombo(GetDlgItem(gMain, kIdCaptureKind));
    normalizeCombo(GetDlgItem(gMain, kIdCaptureSource));
    normalizeCombo(GetDlgItem(gMain, kIdFps));
    normalizeCombo(GetDlgItem(gMain, kIdAudioCombo));

    ensureAudioArea();

    // Ordem Z deliberada: cover -> título -> combo. Assim o texto antigo some sem tampar o seletor.
    bringToFront(gAudioCover);
    bringToFront(gAudioLabel);
    bringToFront(GetDlgItem(gMain, kIdAudioCombo));
    bringToFront(GetDlgItem(gMain, kIdAbout));
    bringToFront(GetDlgItem(gMain, kIdHelp));
    bringToFront(GetDlgItem(gMain, kIdReferenceTitle));
    bringToFront(GetDlgItem(gMain, kIdMonitorMessage));
    bringToFront(GetDlgItem(gMain, kIdPreview));
}

LRESULT CALLBACK polishProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_SIZE && wp != SIZE_MINIMIZED) {
        applyLayout();
    } else if (msg == WM_PAINT) {
        const LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
        // Só mantém os elementos sobre o desenho legado; não reposiciona combos a cada repaint.
        bringToFront(gAudioCover);
        bringToFront(gAudioLabel);
        bringToFront(GetDlgItem(hwnd, kIdAudioCombo));
        bringToFront(GetDlgItem(hwnd, kIdAbout));
        bringToFront(GetDlgItem(hwnd, kIdHelp));
        return result;
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, polishProc, 79);
        gMain = nullptr;
        gAudioCover = nullptr;
        gAudioLabel = nullptr;
        gSubclassInstalled = false;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

VOID CALLBACK delayedInstall(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    KillTimer(hwnd, id);
    if (!gMain || hwnd != gMain || gSubclassInstalled) return;
    gSubclassInstalled = SetWindowSubclass(hwnd, polishProc, 79, 0) != FALSE;
    applyLayout();
    InvalidateRect(hwnd, nullptr, TRUE);
}

void scheduleInstall(HWND hwnd) {
    if (gMain || !hwnd) return;
    gMain = hwnd;
    // Uma única aplicação após os helpers criarem seus controles. Sem timer periódico:
    // isso elimina o reposicionamento/redesenho de ComboBox que causava as piscadas.
    SetTimer(hwnd, kInstallTimer, 350, delayedInstall);
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
