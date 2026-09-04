#include <windows.h>
#include <commctrl.h>

#include <atomic>
#include <chrono>
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
constexpr UINT kStatusMessage = WM_APP + 1;
constexpr UINT kWorkerEnded = WM_APP + 2;

enum ControlId {
    IdSourceName = 1001,
    IdCaptureKind,
    IdCaptureSource,
    IdRefresh,
    IdFps,
    IdCursor,
    IdProtected,
    IdQuick,
    IdStart,
    IdStop,
    IdStatus,
    IdHelp
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
    HWND start{};
    HWND stop{};
    HWND status{};
    HWND help{};
    std::vector<MonitorSource> monitors;
    std::vector<WindowSource> windows;
    std::atomic_bool running{false};
    std::atomic_bool stopRequested{false};
    std::thread worker;
    std::mutex workerMutex;
    AppSettings settings;
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

void postStatus(const std::wstring& text) {
    auto* copy = new std::wstring(text);
    if (!PostMessageW(g.window, kStatusMessage, 0, reinterpret_cast<LPARAM>(copy))) delete copy;
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
    g.settings.showCursor = Button_GetCheck(g.cursor) == BST_CHECKED;
    g.settings.mode = Button_GetCheck(g.quickMode) == BST_CHECKED ? TransmissionMode::Quick : TransmissionMode::Protected;
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
}

void transmissionWorker(CaptureSource source, std::string sourceName, int fps, bool showCursor) {
    NdiSender sender;
    std::wstring error;
    if (!sender.open(executableDirectory() / L"Processing.NDI.Lib.x64.dll", sourceName, error)) {
        postStatus(L"Erro: " + error);
        g.running = false;
        PostMessageW(g.window, kWorkerEnded, 0, 0);
        return;
    }

    postStatus(L"Fonte NDI ativa. Base V3 em teste.");
    std::vector<unsigned char> frame;
    int width = 0;
    int height = 0;
    auto nextFrame = std::chrono::steady_clock::now();

    while (!g.stopRequested.load()) {
        if (!captureToBuffer(source, showCursor, frame, width, height)) {
            postStatus(L"Falha de captura. A transmissão foi encerrada.");
            break;
        }
        if (!sender.sendFrame(frame.data(), width, height, fps)) {
            postStatus(L"Falha ao enviar frame NDI.");
            break;
        }
        nextFrame += std::chrono::microseconds(1000000 / fps);
        std::this_thread::sleep_until(nextFrame);
        if (std::chrono::steady_clock::now() - nextFrame > 1s) nextFrame = std::chrono::steady_clock::now();
    }

    sender.close();
    g.running = false;
    g.stopRequested = false;
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
    g.stopRequested = false;
    g.running = true;
    updateControls();
    std::lock_guard<std::mutex> lock(g.workerMutex);
    if (g.worker.joinable()) g.worker.join();
    g.worker = std::thread(transmissionWorker, source, utf8(sourceName), fps, showCursor);
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
        L"Esta é a nova base V3 do Transmissor NDI Portátil.\n\n"
        L"1. Escolha Monitor ou Janela.\n"
        L"2. Selecione a origem.\n"
        L"3. Defina 30 ou 60 FPS.\n"
        L"4. Clique em Iniciar transmissão.\n\n"
        L"Nesta etapa a arquitetura foi reconstruída e a transmissão básica está isolada em módulos. "
        L"Os modos protegido/rápido e as proteções de privacidade serão adicionados sobre esta base após a compilação ficar estável.",
        L"Como usar — V3", MB_OK | MB_ICONINFORMATION);
}

HWND addControl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}

void createUi() {
    addControl(L"STATIC", L"Nome da fonte NDI", 0, 24, 18, 180, 20, 0);
    g.sourceName = addControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 24, 42, 330, 28, IdSourceName);
    g.help = addControl(L"BUTTON", L"Como usar", BS_PUSHBUTTON, 560, 40, 150, 30, IdHelp);

    addControl(L"STATIC", L"Captura", 0, 24, 92, 100, 20, 0);
    g.captureKind = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 24, 116, 150, 180, IdCaptureKind);
    SendMessageW(g.captureKind, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Monitor"));
    SendMessageW(g.captureKind, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Janela"));
    g.captureSource = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 184, 116, 390, 260, IdCaptureSource);
    g.refresh = addControl(L"BUTTON", L"Atualizar", BS_PUSHBUTTON, 584, 115, 126, 30, IdRefresh);

    addControl(L"STATIC", L"Qualidade", 0, 24, 164, 100, 20, 0);
    g.fps = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST, 24, 188, 120, 120, IdFps);
    SendMessageW(g.fps, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"30 FPS"));
    SendMessageW(g.fps, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"60 FPS"));
    g.cursor = addControl(L"BUTTON", L"Mostrar cursor", BS_AUTOCHECKBOX, 164, 188, 160, 26, IdCursor);

    addControl(L"STATIC", L"Modo", 0, 24, 238, 100, 20, 0);
    g.protectedMode = addControl(L"BUTTON", L"Modo protegido", BS_AUTORADIOBUTTON | WS_GROUP, 24, 262, 170, 26, IdProtected);
    g.quickMode = addControl(L"BUTTON", L"Modo rápido", BS_AUTORADIOBUTTON, 204, 262, 150, 26, IdQuick);

    g.status = addControl(L"STATIC", L"Pronto. Nenhuma transmissão iniciada.", 0, 24, 322, 686, 26, IdStatus);
    g.start = addControl(L"BUTTON", L"Iniciar transmissão", BS_DEFPUSHBUTTON, 24, 372, 220, 40, IdStart);
    g.stop = addControl(L"BUTTON", L"Parar", BS_PUSHBUTTON, 256, 372, 140, 40, IdStop);

    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    EnumChildWindows(g.window, [](HWND child, LPARAM value) -> BOOL {
        SendMessageW(child, WM_SETFONT, value, TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(font));
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g.window = hwnd;
        g.settings = loadSettings(iniPath());
        createUi();
        SetWindowTextW(g.sourceName, g.settings.sourceName.c_str());
        SendMessageW(g.captureKind, CB_SETCURSEL, g.settings.captureKind == CaptureKind::Window ? 1 : 0, 0);
        refreshSources(g.settings.sourceIndex);
        SendMessageW(g.fps, CB_SETCURSEL, g.settings.fps == 60 ? 1 : 0, 0);
        Button_SetCheck(g.cursor, g.settings.showCursor ? BST_CHECKED : BST_UNCHECKED);
        Button_SetCheck(g.settings.mode == TransmissionMode::Quick ? g.quickMode : g.protectedMode, BST_CHECKED);
        updateControls();
        if (g.settings.showHelpOnStart) PostMessageW(hwnd, WM_COMMAND, IdHelp, 0);
        return 0;

    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id == IdCaptureKind && HIWORD(wp) == CBN_SELCHANGE) refreshSources();
        else if (id == IdRefresh) refreshSources();
        else if (id == IdStart) startTransmission();
        else if (id == IdStop) stopTransmission();
        else if (id == IdHelp) showHelp();
        return 0;
    }

    case kStatusMessage: {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
        SetWindowTextW(g.status, text->c_str());
        return 0;
    }

    case kWorkerEnded:
        if (g.worker.joinable()) {
            std::lock_guard<std::mutex> lock(g.workerMutex);
            if (g.worker.joinable()) g.worker.join();
        }
        updateControls();
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
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, kWindowClass, L"Transmissor NDI Portátil — V3",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 750, 500, nullptr, nullptr, instance, nullptr);
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
    return static_cast<int>(msg.wParam);
}
