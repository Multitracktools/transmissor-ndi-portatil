#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app_types.h"
#include "capture.h"
#include "ndi_sender.h"
#include "settings.h"

namespace {
using namespace std::chrono_literals;

constexpr wchar_t kWindowClass[] = L"TransmissorNDIPortatilV3";
constexpr wchar_t kPreviewClass[] = L"TransmissorNDIPreviewV3";
constexpr UINT kStatusMessage = WM_APP + 1;
constexpr UINT kWorkerEnded = WM_APP + 2;
constexpr UINT kReceiverMessage = WM_APP + 3;
constexpr UINT_PTR kPreviewTimer = 77;

constexpr COLORREF kBg = RGB(12, 18, 28);
constexpr COLORREF kPanel = RGB(24, 32, 45);
constexpr COLORREF kPanel2 = RGB(17, 24, 35);
constexpr COLORREF kBorder = RGB(46, 58, 76);
constexpr COLORREF kText = RGB(240, 244, 250);
constexpr COLORREF kMuted = RGB(158, 171, 191);
constexpr COLORREF kBlue = RGB(70, 130, 255);
constexpr COLORREF kGreen = RGB(49, 201, 125);

HBRUSH gBgBrush{};
HBRUSH gPanelBrush{};
HBRUSH gPanel2Brush{};
HBRUSH gEditBrush{};
HFONT gFont{};
HFONT gFontSmall{};
HFONT gFontBold{};
HFONT gFontTitle{};

enum ControlId {
    IdSourceName = 1001,
    IdCaptureKind,
    IdCaptureSource,
    IdRefresh,
    IdFps,
    IdCursor,
    IdProtected,
    IdQuick,
    IdAllowWhatsApp,
    IdAllowTelegram,
    IdStart,
    IdRelease,
    IdStop,
    IdReceivers,
    IdStatus,
    IdHelp,
    IdPreview
};

struct UiState {
    HWND window{};
    HWND sourceName{};
    HWND captureKind{};
    HWND captureSource{};
    HWND refresh{};
    HWND fps{};
    HWND cursor{};
    HWND protectedMode{};
    HWND quickMode{};
    HWND allowWhatsApp{};
    HWND allowTelegram{};
    HWND start{};
    HWND release{};
    HWND stop{};
    HWND receivers{};
    HWND status{};
    HWND help{};
    HWND preview{};

    std::vector<MonitorSource> monitors;
    std::vector<WindowSource> windows;

    std::atomic_bool running{false};
    std::atomic_bool stopRequested{false};
    std::atomic_bool authorized{false};
    std::atomic_bool allowWa{false};
    std::atomic_bool allowTg{false};
    std::atomic_int receiverCount{0};
    bool activeProtectedMode{true};

    std::thread worker;
    std::mutex workerMutex;
    AppSettings settings;

    std::vector<unsigned char> previewFrame;
    int previewWidth{0};
    int previewHeight{0};
    std::wstring previewLabel;
};

UiState g;

std::filesystem::path executableDirectory() {
    std::vector<wchar_t> buffer(32768);
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return std::filesystem::path(std::wstring(buffer.data(), len)).parent_path();
}

std::filesystem::path iniPath() { return executableDirectory() / L"transmissor-ndi.ini"; }

std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring getText(HWND hwnd) {
    const int len = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(hwnd, text.data(), len + 1);
    text.resize(static_cast<size_t>(len));
    return text;
}

bool isChecked(HWND hwnd) {
    return SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void setChecked(HWND hwnd, bool checked) {
    SendMessageW(hwnd, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
}

void postStatus(const std::wstring& text) {
    auto* copy = new std::wstring(text);
    if (!PostMessageW(g.window, kStatusMessage, 0, reinterpret_cast<LPARAM>(copy))) delete copy;
}

void postReceiverCount(int count) {
    PostMessageW(g.window, kReceiverMessage, static_cast<WPARAM>(count), 0);
}

void applyDarkTheme(HWND hwnd) {
    SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);
}

void setControlFont(HWND hwnd, HFONT font) {
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

CaptureKind selectedKind() {
    return SendMessageW(g.captureKind, CB_GETCURSEL, 0, 0) == 1 ? CaptureKind::Window : CaptureKind::Monitor;
}

void refreshSources(int preferredIndex = -1) {
    SendMessageW(g.captureSource, CB_RESETCONTENT, 0, 0);
    const CaptureKind kind = selectedKind();
    if (kind == CaptureKind::Monitor) {
        g.monitors = enumerateMonitors();
        for (const auto& item : g.monitors)
            SendMessageW(g.captureSource, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.label.c_str()));
    } else {
        g.windows = enumerateWindows(g.window);
        for (const auto& item : g.windows)
            SendMessageW(g.captureSource, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.label.c_str()));
    }
    const LRESULT count = SendMessageW(g.captureSource, CB_GETCOUNT, 0, 0);
    if (count > 0) {
        const int index = preferredIndex >= 0 && preferredIndex < count ? preferredIndex : 0;
        SendMessageW(g.captureSource, CB_SETCURSEL, index, 0);
    }
    InvalidateRect(g.preview, nullptr, TRUE);
}

CaptureSource currentSource() {
    CaptureSource source;
    source.kind = selectedKind();
    const int index = static_cast<int>(SendMessageW(g.captureSource, CB_GETCURSEL, 0, 0));
    if (source.kind == CaptureKind::Monitor && index >= 0 && index < static_cast<int>(g.monitors.size())) {
        const auto& item = g.monitors[index];
        source.monitor = item.handle;
        source.bounds = item.bounds;
        source.label = item.label;
    } else if (source.kind == CaptureKind::Window && index >= 0 && index < static_cast<int>(g.windows.size())) {
        const auto& item = g.windows[index];
        source.window = item.handle;
        source.bounds = item.bounds;
        source.label = item.label;
    }
    return source;
}

void collectSettings() {
    g.settings.sourceName = getText(g.sourceName);
    g.settings.captureKind = selectedKind();
    g.settings.sourceIndex = static_cast<int>(SendMessageW(g.captureSource, CB_GETCURSEL, 0, 0));
    g.settings.fps = SendMessageW(g.fps, CB_GETCURSEL, 0, 0) == 1 ? 60 : 30;
    g.settings.showCursor = isChecked(g.cursor);
    g.settings.mode = isChecked(g.quickMode) ? TransmissionMode::Quick : TransmissionMode::Protected;
    saveSettings(iniPath(), g.settings);
}

void updateControls() {
    const bool running = g.running.load();
    EnableWindow(g.start, !running);
    EnableWindow(g.stop, running);
    EnableWindow(g.sourceName, !running);
    EnableWindow(g.captureKind, !running);
    EnableWindow(g.captureSource, !running);
    EnableWindow(g.refresh, !running);
    EnableWindow(g.fps, !running);
    EnableWindow(g.cursor, !running);
    EnableWindow(g.protectedMode, !running);
    EnableWindow(g.quickMode, !running);

    const bool canRelease = running && g.activeProtectedMode && g.receiverCount.load() == 1 && !g.authorized.load();
    EnableWindow(g.release, canRelease);
}

std::vector<unsigned char> createMessageFrame(int width, int height,
                                               const std::wstring& title,
                                               const std::wstring& body,
                                               const std::wstring& sourceName) {
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* dibPixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &dibPixels, nullptr, 0);
    if (!bitmap || !dibPixels) {
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        if (screen) ReleaseDC(nullptr, screen);
        return pixels;
    }

    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    RECT full{0, 0, width, height};
    HBRUSH background = CreateSolidBrush(RGB(15, 22, 33));
    FillRect(dc, &full, background);
    DeleteObject(background);
    SetBkMode(dc, TRANSPARENT);

    LOGFONTW lf{};
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    lf.lfHeight = -38;
    lf.lfWeight = FW_SEMIBOLD;
    HFONT titleFont = CreateFontIndirectW(&lf);
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, titleFont));
    SetTextColor(dc, RGB(245, 246, 248));
    RECT r{40, height / 2 - 100, width - 40, height / 2 - 20};
    DrawTextW(dc, title.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    lf.lfHeight = -22;
    lf.lfWeight = FW_NORMAL;
    HFONT bodyFont = CreateFontIndirectW(&lf);
    SelectObject(dc, bodyFont);
    SetTextColor(dc, RGB(185, 196, 212));
    r = {40, height / 2 - 20, width - 40, height / 2 + 60};
    DrawTextW(dc, body.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    lf.lfHeight = -16;
    HFONT sourceFont = CreateFontIndirectW(&lf);
    SelectObject(dc, sourceFont);
    SetTextColor(dc, RGB(120, 136, 158));
    r = {40, height - 70, width - 40, height - 25};
    DrawTextW(dc, sourceName.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    std::memcpy(pixels.data(), dibPixels, pixels.size());
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBitmap);
    DeleteObject(titleFont);
    DeleteObject(bodyFont);
    DeleteObject(sourceFont);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return pixels;
}

void updatePreview() {
    if (!g.preview || !IsWindowVisible(g.window)) return;
    CaptureSource source = currentSource();
    if (source.bounds.right <= source.bounds.left || source.bounds.bottom <= source.bounds.top) return;

    std::vector<unsigned char> frame;
    int width = 0;
    int height = 0;
    if (captureToBuffer(source, isChecked(g.cursor), frame, width, height)) {
        g.previewFrame = std::move(frame);
        g.previewWidth = width;
        g.previewHeight = height;
        g.previewLabel = source.label;
        InvalidateRect(g.preview, nullptr, FALSE);
    }
}

void drawPreview(HDC dc, const RECT& client) {
    HBRUSH bg = CreateSolidBrush(RGB(8, 13, 21));
    FillRect(dc, &client, bg);
    DeleteObject(bg);

    if (g.previewFrame.empty() || g.previewWidth <= 0 || g.previewHeight <= 0) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kMuted);
        HFONT old = static_cast<HFONT>(SelectObject(dc, gFont));
        RECT text = client;
        DrawTextW(dc, L"Prévia indisponível", -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);
        return;
    }

    const int cw = client.right - client.left;
    const int ch = client.bottom - client.top;
    const double scale = min(static_cast<double>(cw) / g.previewWidth,
                             static_cast<double>(ch) / g.previewHeight);
    const int dw = static_cast<int>(g.previewWidth * scale);
    const int dh = static_cast<int>(g.previewHeight * scale);
    const int dx = (cw - dw) / 2;
    const int dy = (ch - dh) / 2;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = g.previewWidth;
    info.bmiHeader.biHeight = -g.previewHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    SetStretchBltMode(dc, HALFTONE);
    StretchDIBits(dc, dx, dy, dw, dh, 0, 0, g.previewWidth, g.previewHeight,
                  g.previewFrame.data(), &info, DIB_RGB_COLORS, SRCCOPY);

    RECT label{12, client.bottom - 38, client.right - 12, client.bottom - 10};
    HBRUSH overlay = CreateSolidBrush(RGB(16, 23, 34));
    FillRect(dc, &label, overlay);
    DeleteObject(overlay);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kText);
    HFONT old = static_cast<HFONT>(SelectObject(dc, gFontSmall));
    std::wstring text = g.previewLabel;
    if (!text.empty()) text += L"  ·  " + std::to_wstring(g.previewWidth) + L" × " + std::to_wstring(g.previewHeight);
    DrawTextW(dc, text.c_str(), -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, old);
}

void transmissionWorker(CaptureSource source, std::wstring sourceDisplayName,
                        std::string sourceName, int fps, bool showCursor,
                        TransmissionMode mode) {
    NdiSender sender;
    std::wstring error;
    if (!sender.open(executableDirectory() / L"Processing.NDI.Lib.x64.dll", sourceName, error)) {
        postStatus(L"Erro: " + error);
        g.running = false;
        PostMessageW(g.window, kWorkerEnded, 0, 0);
        return;
    }

    const int placeholderWidth = 1280;
    const int placeholderHeight = 720;
    const auto waitingFrame = createMessageFrame(
        placeholderWidth, placeholderHeight,
        L"Transmissão protegida",
        L"Aguardando autorização no computador transmissor.",
        sourceDisplayName);
    const auto extraConnectionFrame = createMessageFrame(
        placeholderWidth, placeholderHeight,
        L"Transmissão temporariamente bloqueada",
        L"Foi detectada uma conexão adicional.",
        sourceDisplayName);

    g.authorized = (mode == TransmissionMode::Quick);
    g.receiverCount = 0;
    postReceiverCount(0);
    postStatus(mode == TransmissionMode::Protected
        ? L"Fonte NDI ativa — aguardando receptor."
        : L"Modo rápido — transmissão iniciada.");

    std::vector<unsigned char> frame;
    int width = 0;
    int height = 0;
    int previousConnections = -1;
    bool extraConnectionLock = false;
    auto nextFrame = std::chrono::steady_clock::now();

    while (!g.stopRequested.load()) {
        const int connections = sender.connections();
        if (connections != previousConnections) {
            previousConnections = connections;
            g.receiverCount = connections;
            postReceiverCount(connections);

            if (mode == TransmissionMode::Protected) {
                if (connections > 1) {
                    extraConnectionLock = true;
                    g.authorized = false;
                    postStatus(L"Transmissão bloqueada — foi detectado mais de um receptor.");
                } else if (connections == 1 && !g.authorized.load()) {
                    postStatus(extraConnectionLock
                        ? L"Restou um receptor. Clique em Liberar novamente para continuar."
                        : L"Receptor conectado — clique em Liberar transmissão quando estiver pronto.");
                } else if (connections == 0) {
                    postStatus(L"Fonte NDI ativa — aguardando receptor.");
                }
            } else if (connections > 1) {
                postStatus(L"Modo rápido — aviso: há mais de um receptor conectado.");
            }
        }

        if (mode == TransmissionMode::Protected && !g.authorized.load()) {
            const auto& protectedFrame = extraConnectionLock ? extraConnectionFrame : waitingFrame;
            if (!sender.sendFrame(protectedFrame.data(), placeholderWidth, placeholderHeight, fps)) {
                postStatus(L"Falha ao enviar tela protegida NDI.");
                break;
            }
        } else {
            if (!captureToBuffer(source, showCursor, frame, width, height)) {
                postStatus(L"Falha de captura. A transmissão foi encerrada.");
                break;
            }
            if (!sender.sendFrame(frame.data(), width, height, fps)) {
                postStatus(L"Falha ao enviar frame NDI.");
                break;
            }
        }

        nextFrame += std::chrono::microseconds(1000000 / fps);
        std::this_thread::sleep_until(nextFrame);
        if (std::chrono::steady_clock::now() - nextFrame > 1s) nextFrame = std::chrono::steady_clock::now();
    }

    sender.close();
    g.running = false;
    g.stopRequested = false;
    g.authorized = false;
    g.receiverCount = 0;
    postReceiverCount(0);
    postStatus(L"Transmissão encerrada.");
    PostMessageW(g.window, kWorkerEnded, 0, 0);
}

void startTransmission() {
    if (g.running.load()) return;
    const std::wstring sourceName = getText(g.sourceName);
    if (sourceName.empty()) {
        MessageBoxW(g.window, L"Informe um nome para a fonte NDI.", L"Transmissor NDI", MB_OK | MB_ICONWARNING);
        return;
    }
    CaptureSource source = currentSource();
    if (source.bounds.right <= source.bounds.left || source.bounds.bottom <= source.bounds.top) {
        MessageBoxW(g.window, L"Selecione uma fonte de captura válida.", L"Transmissor NDI", MB_OK | MB_ICONWARNING);
        return;
    }

    collectSettings();
    const int fps = g.settings.fps;
    const bool showCursor = g.settings.showCursor;
    const TransmissionMode mode = g.settings.mode;
    g.activeProtectedMode = mode == TransmissionMode::Protected;
    g.authorized = mode == TransmissionMode::Quick;
    g.receiverCount = 0;
    g.stopRequested = false;
    g.running = true;
    updateControls();

    std::lock_guard<std::mutex> lock(g.workerMutex);
    if (g.worker.joinable()) g.worker.join();
    g.worker = std::thread(transmissionWorker, source, sourceName, utf8(sourceName), fps, showCursor, mode);
}

void releaseTransmission() {
    if (!g.running.load() || !g.activeProtectedMode) return;
    if (g.receiverCount.load() != 1) return;
    g.authorized = true;
    postStatus(L"Conteúdo liberado para o receptor conectado.");
    updateControls();
}

void stopTransmission() {
    if (!g.running.load()) return;
    g.stopRequested = true;
    std::lock_guard<std::mutex> lock(g.workerMutex);
    if (g.worker.joinable()) g.worker.join();
    updateControls();
}

void showHelp() {
    MessageBoxW(g.window,
        L"1. Escolha Monitor ou Janela e confirme a fonte na prévia.\n"
        L"2. No Modo protegido, a fonte NDI começa com uma tela de espera.\n"
        L"3. Quando exatamente um receptor conectar, clique em Liberar transmissão.\n"
        L"4. Se entrar outro receptor, a imagem é bloqueada imediatamente.\n"
        L"5. As permissões de WhatsApp e Telegram valem apenas durante esta execução.\n"
        L"6. No Modo rápido, a imagem começa imediatamente e conexões extras apenas geram aviso.\n\n"
        L"Nenhuma transmissão começa automaticamente ao abrir o aplicativo.",
        L"Como usar — V3", MB_OK | MB_ICONINFORMATION);
}

HWND addControl(const wchar_t* cls, const wchar_t* text, DWORD style,
                int x, int y, int w, int h, int id, HFONT font = nullptr) {
    HWND hwnd = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (font) setControlFont(hwnd, font);
    applyDarkTheme(hwnd);
    return hwnd;
}

void drawPanel(HDC dc, RECT r) {
    HBRUSH brush = CreateSolidBrush(kPanel);
    HPEN pen = CreatePen(PS_SOLID, 1, kBorder);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, 14, 14);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void drawSectionTitle(HDC dc, int x, int y, const wchar_t* text) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kText);
    HFONT old = static_cast<HFONT>(SelectObject(dc, gFontBold));
    RECT r{x, y, x + 330, y + 26};
    DrawTextW(dc, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old);
}

void drawSmallText(HDC dc, int x, int y, const wchar_t* text, int width = 300) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kMuted);
    HFONT old = static_cast<HFONT>(SelectObject(dc, gFontSmall));
    RECT r{x, y, x + width, y + 22};
    DrawTextW(dc, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old);
}

void createUi() {
    g.help = addControl(L"BUTTON", L"?  Como usar", BS_PUSHBUTTON, 842, 18, 130, 34, IdHelp, gFont);

    g.sourceName = addControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 32, 206, 480, 34, IdSourceName, gFont);

    g.captureKind = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 32, 286, 150, 220, IdCaptureKind, gFont);
    SendMessageW(g.captureKind, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Monitor"));
    SendMessageW(g.captureKind, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Janela"));

    g.captureSource = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 192, 286, 270, 260, IdCaptureSource, gFont);
    g.refresh = addControl(L"BUTTON", L"↻", BS_PUSHBUTTON, 472, 286, 40, 34, IdRefresh, gFontBold);

    g.fps = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST, 32, 354, 125, 120, IdFps, gFont);
    SendMessageW(g.fps, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"30 FPS"));
    SendMessageW(g.fps, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"60 FPS"));
    g.cursor = addControl(L"BUTTON", L"Mostrar cursor", BS_AUTOCHECKBOX, 174, 356, 160, 28, IdCursor, gFont);

    g.protectedMode = addControl(L"BUTTON", L"Modo protegido\r\nConfirma o receptor antes de liberar", BS_AUTORADIOBUTTON | BS_MULTILINE | WS_GROUP,
                                 32, 420, 230, 72, IdProtected, gFont);
    g.quickMode = addControl(L"BUTTON", L"Modo rápido\r\nComeça a mostrar imediatamente", BS_AUTORADIOBUTTON | BS_MULTILINE,
                             274, 420, 238, 72, IdQuick, gFont);

    g.allowWhatsApp = addControl(L"BUTTON", L"Permitir envio do WhatsApp", BS_AUTOCHECKBOX, 32, 538, 480, 36, IdAllowWhatsApp, gFont);
    g.allowTelegram = addControl(L"BUTTON", L"Permitir envio do Telegram", BS_AUTOCHECKBOX, 32, 580, 480, 36, IdAllowTelegram, gFont);

    g.release = addControl(L"BUTTON", L"Liberar transmissão", BS_PUSHBUTTON, 572, 332, 368, 40, IdRelease, gFontBold);
    g.preview = CreateWindowExW(0, kPreviewClass, L"", WS_CHILD | WS_VISIBLE, 572, 460, 368, 186,
                                g.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdPreview)), GetModuleHandleW(nullptr), nullptr);

    g.start = addControl(L"BUTTON", L"Iniciar transmissão", BS_DEFPUSHBUTTON, 770, 680, 170, 42, IdStart, gFontBold);
    g.stop = addControl(L"BUTTON", L"Parar", BS_PUSHBUTTON, 650, 680, 108, 42, IdStop, gFont);

    g.receivers = addControl(L"STATIC", L"0 conectados", SS_LEFT, 274, 98, 190, 26, IdReceivers, gFontBold);
    g.status = addControl(L"STATIC", L"Pronta para iniciar", SS_LEFT | SS_END_ELLIPSIS, 32, 98, 205, 26, IdStatus, gFontBold);
}

LRESULT CALLBACK previewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{}; GetClientRect(hwnd, &rc);
        drawPreview(dc, rc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g.window = hwnd;
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
        g.settings = loadSettings(iniPath());
        createUi();
        SetWindowTextW(g.sourceName, g.settings.sourceName.c_str());
        SendMessageW(g.captureKind, CB_SETCURSEL, g.settings.captureKind == CaptureKind::Window ? 1 : 0, 0);
        refreshSources(g.settings.sourceIndex);
        SendMessageW(g.fps, CB_SETCURSEL, g.settings.fps == 60 ? 1 : 0, 0);
        setChecked(g.cursor, g.settings.showCursor);
        setChecked(g.settings.mode == TransmissionMode::Quick ? g.quickMode : g.protectedMode, true);
        setChecked(g.allowWhatsApp, false);
        setChecked(g.allowTelegram, false);
        g.allowWa = false;
        g.allowTg = false;
        updateControls();
        SetTimer(hwnd, kPreviewTimer, 200, nullptr);
        if (g.settings.showHelpOnStart) PostMessageW(hwnd, WM_COMMAND, IdHelp, 0);
        return 0;
    }

    case WM_TIMER:
        if (wp == kPreviewTimer) updatePreview();
        return 0;

    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id == IdCaptureKind && HIWORD(wp) == CBN_SELCHANGE) refreshSources();
        else if (id == IdCaptureSource && HIWORD(wp) == CBN_SELCHANGE) updatePreview();
        else if (id == IdRefresh) refreshSources();
        else if (id == IdStart) startTransmission();
        else if (id == IdRelease) releaseTransmission();
        else if (id == IdStop) stopTransmission();
        else if (id == IdAllowWhatsApp) g.allowWa = isChecked(g.allowWhatsApp);
        else if (id == IdAllowTelegram) g.allowTg = isChecked(g.allowTelegram);
        else if (id == IdHelp) showHelp();
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, kText);
        SetBkColor(dc, kPanel);
        return reinterpret_cast<INT_PTR>(gPanelBrush);
    }

    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, kText);
        SetBkColor(dc, kPanel2);
        return reinterpret_cast<INT_PTR>(gEditBrush);
    }

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{}; GetClientRect(hwnd, &client);
        FillRect(dc, &client, gBgBrush);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kText);
        HFONT old = static_cast<HFONT>(SelectObject(dc, gFontTitle));
        RECT title{32, 18, 520, 48};
        DrawTextW(dc, L"Transmissor NDI Portátil", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, gFontSmall);
        SetTextColor(dc, kMuted);
        RECT sub{32, 48, 420, 68};
        DrawTextW(dc, L"Windows · versão de testes", -1, &sub, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        drawPanel(dc, {20, 80, 980, 146});
        drawSmallText(dc, 32, 84, L"Transmissão", 180);
        drawSmallText(dc, 274, 84, L"Receptores", 170);
        drawSmallText(dc, 510, 84, L"Envio", 150);
        drawSmallText(dc, 748, 84, L"Desempenho", 180);
        SetTextColor(dc, kText);
        SelectObject(dc, gFontBold);
        RECT envio{510, 103, 690, 130}; DrawTextW(dc, L"— Mbps", -1, &envio, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT perf{748, 103, 930, 130}; DrawTextW(dc, g.running ? L"Em transmissão" : L"Aguardando", -1, &perf, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        drawPanel(dc, {20, 158, 536, 654});
        drawPanel(dc, {552, 158, 960, 654});
        drawSectionTitle(dc, 32, 172, L"Configuração da transmissão");
        drawSmallText(dc, 32, 190, L"Nome da fonte NDI", 250);
        drawSmallText(dc, 32, 266, L"O que deseja transmitir", 260);
        drawSmallText(dc, 32, 334, L"Qualidade", 150);
        drawSmallText(dc, 32, 400, L"Modo de transmissão", 220);
        drawSectionTitle(dc, 32, 506, L"Permitir envio durante esta execução");
        drawSmallText(dc, 32, 620, L"As permissões não são salvas e reiniciam protegidas.", 470);

        drawSectionTitle(dc, 572, 172, L"Controle de acesso");
        SetTextColor(dc, kText);
        SelectObject(dc, gFontTitle);
        RECT number{572, 216, 940, 264};
        const std::wstring count = std::to_wstring(g.receiverCount.load());
        DrawTextW(dc, count.c_str(), -1, &number, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, gFontSmall);
        SetTextColor(dc, kMuted);
        RECT access{582, 266, 930, 318};
        const wchar_t* accessText = g.receiverCount.load() == 0
            ? L"Nenhum receptor conectado. A imagem ficará em espera até sua confirmação."
            : g.receiverCount.load() == 1
                ? L"Um receptor conectado. Libere quando estiver pronto."
                : L"Mais de um receptor conectado. A transmissão está protegida.";
        DrawTextW(dc, accessText, -1, &access, DT_CENTER | DT_WORDBREAK);

        RECT privacy{572, 384, 940, 442};
        HBRUSH privacyBrush = CreateSolidBrush(RGB(20, 70, 54));
        FillRect(dc, &privacy, privacyBrush);
        DeleteObject(privacyBrush);
        SetTextColor(dc, RGB(197, 246, 218));
        SelectObject(dc, gFontBold);
        RECT p1{588, 390, 925, 416}; DrawTextW(dc, L"Modo Privacidade ativo", -1, &p1, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, gFontSmall);
        RECT p2{588, 414, 925, 438}; DrawTextW(dc, L"WhatsApp e Telegram começam protegidos.", -1, &p2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        drawSectionTitle(dc, 572, 444, L"Prévia da captura");

        drawPanel(dc, {20, 666, 960, 736});
        SetTextColor(dc, RGB(199, 238, 215));
        SelectObject(dc, gFontBold);
        RECT wifi{32, 678, 420, 704}; DrawTextW(dc, L"Rede · aguardando medição", -1, &wifi, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, gFontSmall);
        SetTextColor(dc, kMuted);
        RECT q{32, 704, 480, 726}; DrawTextW(dc, L"30 fps · Prévia em baixa taxa para economizar recursos", -1, &q, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SelectObject(dc, old);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case kStatusMessage: {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
        SetWindowTextW(g.status, text->c_str());
        updateControls();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case kReceiverMessage: {
        const int count = static_cast<int>(wp);
        SetWindowTextW(g.receivers, (std::to_wstring(count) + L" conectados").c_str());
        if (g.activeProtectedMode && count > 1) {
            FLASHWINFO flash{sizeof(flash), hwnd, FLASHW_TRAY, 1, 0};
            FlashWindowEx(&flash);
        }
        updateControls();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case kWorkerEnded:
        if (g.worker.joinable()) {
            std::lock_guard<std::mutex> lock(g.workerMutex);
            if (g.worker.joinable()) g.worker.join();
        }
        updateControls();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_CLOSE:
        if (g.running.load()) {
            if (MessageBoxW(hwnd, L"Há uma transmissão ativa. Deseja parar e sair?", L"Transmissão ativa", MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
            stopTransmission();
        }
        collectSettings();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kPreviewTimer);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    gBgBrush = CreateSolidBrush(kBg);
    gPanelBrush = CreateSolidBrush(kPanel);
    gPanel2Brush = CreateSolidBrush(kPanel2);
    gEditBrush = CreateSolidBrush(kPanel2);

    LOGFONTW lf{};
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    lf.lfHeight = -16; gFont = CreateFontIndirectW(&lf);
    lf.lfHeight = -14; gFontSmall = CreateFontIndirectW(&lf);
    lf.lfHeight = -16; lf.lfWeight = FW_SEMIBOLD; gFontBold = CreateFontIndirectW(&lf);
    lf.lfHeight = -23; lf.lfWeight = FW_SEMIBOLD; gFontTitle = CreateFontIndirectW(&lf);

    WNDCLASSEXW previewClass{sizeof(previewClass)};
    previewClass.lpfnWndProc = previewProc;
    previewClass.hInstance = instance;
    previewClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    previewClass.hbrBackground = gPanel2Brush;
    previewClass.lpszClassName = kPreviewClass;
    if (!RegisterClassExW(&previewClass)) return 1;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = gBgBrush;
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, kWindowClass, L"Transmissor NDI Portátil — V3",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 1000, 790, nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g.worker.joinable()) {
        g.stopRequested = true;
        g.worker.join();
    }

    DeleteObject(gFont);
    DeleteObject(gFontSmall);
    DeleteObject(gFontBold);
    DeleteObject(gFontTitle);
    DeleteObject(gBgBrush);
    DeleteObject(gPanelBrush);
    DeleteObject(gPanel2Brush);
    DeleteObject(gEditBrush);
    return static_cast<int>(msg.wParam);
}