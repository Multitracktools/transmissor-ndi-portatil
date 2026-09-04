#include <windows.h>
#include <commctrl.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Processing.NDI.Lib.h"

namespace {

constexpr wchar_t kWindowClass[] = L"TransmissorNDIPortatilWindow";
constexpr UINT kStatusMessage = WM_APP + 1;

enum ControlId {
    IdSourceName = 1001,
    IdSourceType,
    IdCaptureSource,
    IdRefreshSources,
    IdCursor,
    IdStart,
    IdStop,
    IdStatus
};

struct MonitorInfo {
    HMONITOR handle{};
    RECT bounds{};
    std::wstring label;
};

struct WindowInfo {
    HWND handle{};
    RECT bounds{};
    std::wstring label;
};

enum class CaptureKind {
    Monitor,
    Window
};

struct CaptureTarget {
    CaptureKind kind{CaptureKind::Monitor};
    RECT bounds{};
    HWND window{};
    std::wstring label;
};

struct AppState {
    HWND window{};
    HWND sourceName{};
    HWND sourceTypeCombo{};
    HWND captureSourceCombo{};
    HWND refreshButton{};
    HWND cursorCheckbox{};
    HWND startButton{};
    HWND stopButton{};
    HWND statusLabel{};
    std::vector<MonitorInfo> monitors;
    std::vector<WindowInfo> windows;
    std::thread worker;
    std::atomic_bool stopRequested{false};
    std::atomic_bool running{false};
    std::mutex workerMutex;
};

AppState g_app;

std::filesystem::path executableDirectory() {
    std::vector<wchar_t> path(32768);
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return std::filesystem::path(std::wstring(path.data(), size)).parent_path();
}

void logLine(const std::wstring& message) {
    std::wofstream log(executableDirectory() / L"transmissor-ndi.log", std::ios::app);
    if (!log) return;

    SYSTEMTIME time{};
    GetLocalTime(&time);
    log << L'[' << time.wYear << L'-' << time.wMonth << L'-' << time.wDay << L' '
        << time.wHour << L':' << time.wMinute << L':' << time.wSecond << L"] "
        << message << L'\n';
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

void postStatus(const std::wstring& status) {
    auto* copy = new std::wstring(status);
    if (!PostMessageW(g_app.window, kStatusMessage, 0, reinterpret_cast<LPARAM>(copy))) {
        delete copy;
    }
    logLine(status);
}

BOOL CALLBACK enumerateMonitor(HMONITOR monitor, HDC, LPRECT bounds, LPARAM data) {
    auto* monitors = reinterpret_cast<std::vector<MonitorInfo>*>(data);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);

    const int number = static_cast<int>(monitors->size()) + 1;
    const int width = bounds->right - bounds->left;
    const int height = bounds->bottom - bounds->top;
    std::wstring label = L"Monitor " + std::to_wstring(number) + L" — " +
                         std::to_wstring(width) + L" × " + std::to_wstring(height);
    if ((info.dwFlags & MONITORINFOF_PRIMARY) != 0) label += L" (principal)";
    monitors->push_back({monitor, *bounds, std::move(label)});
    return TRUE;
}

void enumerateMonitors() {
    g_app.monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, enumerateMonitor,
                        reinterpret_cast<LPARAM>(&g_app.monitors));
}

BOOL CALLBACK enumerateWindow(HWND window, LPARAM data) {
    if (window == g_app.window || !IsWindowVisible(window) || IsIconic(window)) return TRUE;
    const int titleLength = GetWindowTextLengthW(window);
    if (titleLength <= 0) return TRUE;

    std::wstring title(static_cast<size_t>(titleLength) + 1, L'\0');
    GetWindowTextW(window, title.data(), titleLength + 1);
    title.resize(static_cast<size_t>(titleLength));

    RECT bounds{};
    if (!GetWindowRect(window, &bounds) || bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        return TRUE;
    }

    auto* windows = reinterpret_cast<std::vector<WindowInfo>*>(data);
    windows->push_back({window, bounds, std::move(title)});
    return TRUE;
}

void enumerateWindows() {
    g_app.windows.clear();
    EnumWindows(enumerateWindow, reinterpret_cast<LPARAM>(&g_app.windows));
}

CaptureKind selectedCaptureKind() {
    return SendMessageW(g_app.sourceTypeCombo, CB_GETCURSEL, 0, 0) == 1
        ? CaptureKind::Window
        : CaptureKind::Monitor;
}

void loadCaptureSources() {
    const CaptureKind kind = selectedCaptureKind();
    SendMessageW(g_app.captureSourceCombo, CB_RESETCONTENT, 0, 0);

    if (kind == CaptureKind::Monitor) {
        enumerateMonitors();
        for (const auto& monitor : g_app.monitors) {
            SendMessageW(g_app.captureSourceCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(monitor.label.c_str()));
        }
    } else {
        enumerateWindows();
        for (const auto& window : g_app.windows) {
            SendMessageW(g_app.captureSourceCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(window.label.c_str()));
        }
    }

    const LRESULT count = SendMessageW(g_app.captureSourceCombo, CB_GETCOUNT, 0, 0);
    if (count > 0) SendMessageW(g_app.captureSourceCombo, CB_SETCURSEL, 0, 0);
}

void drawCursorInto(HDC target, const RECT& monitorBounds) {
    CURSORINFO cursor{};
    cursor.cbSize = sizeof(cursor);
    if (!GetCursorInfo(&cursor) || cursor.flags != CURSOR_SHOWING) return;
    if (!PtInRect(&monitorBounds, cursor.ptScreenPos)) return;

    ICONINFO icon{};
    int x = cursor.ptScreenPos.x - monitorBounds.left;
    int y = cursor.ptScreenPos.y - monitorBounds.top;
    if (GetIconInfo(cursor.hCursor, &icon)) {
        x -= static_cast<int>(icon.xHotspot);
        y -= static_cast<int>(icon.yHotspot);
        if (icon.hbmMask) DeleteObject(icon.hbmMask);
        if (icon.hbmColor) DeleteObject(icon.hbmColor);
    }
    DrawIconEx(target, x, y, cursor.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
}

using NdiLoadFunction = const NDIlib_v6* (*)();

void transmit(CaptureTarget target, std::string sourceName, bool showCursor) {
    const auto dllPath = executableDirectory() / L"Processing.NDI.Lib.x64.dll";
    HMODULE ndiModule = LoadLibraryW(dllPath.c_str());
    if (!ndiModule) {
        postStatus(L"Erro: biblioteca NDI ausente ou bloqueada. Consulte o log.");
        g_app.running = false;
        return;
    }

    auto loadNdi = reinterpret_cast<NdiLoadFunction>(GetProcAddress(ndiModule, "NDIlib_v6_load"));
    const NDIlib_v6* ndi = loadNdi ? loadNdi() : nullptr;
    if (!ndi || !ndi->initialize()) {
        postStatus(L"Erro: não foi possível inicializar o NDI.");
        FreeLibrary(ndiModule);
        g_app.running = false;
        return;
    }

    NDIlib_send_create_t create{};
    create.p_ndi_name = sourceName.c_str();
    create.clock_video = true;
    create.clock_audio = false;
    NDIlib_send_instance_t sender = ndi->send_create(&create);
    if (!sender) {
        postStatus(L"Erro: o NDI não conseguiu criar a transmissão.");
        ndi->destroy();
        FreeLibrary(ndiModule);
        g_app.running = false;
        return;
    }

    if (target.kind == CaptureKind::Window) {
        if (!IsWindow(target.window) || IsIconic(target.window) || !GetWindowRect(target.window, &target.bounds)) {
            postStatus(L"Erro: a janela escolhida não está disponível.");
            ndi->send_destroy(sender);
            ndi->destroy();
            FreeLibrary(ndiModule);
            g_app.running = false;
            return;
        }
    }

    const int width = target.bounds.right - target.bounds.left;
    const int height = target.bounds.bottom - target.bounds.top;
    HDC screenDc = GetDC(nullptr);
    HDC memoryDc = CreateCompatibleDC(screenDc);
    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HGDIOBJ previous = bitmap ? SelectObject(memoryDc, bitmap) : nullptr;

    if (!screenDc || !memoryDc || !bitmap || !pixels) {
        postStatus(L"Erro: não foi possível preparar a captura da tela.");
    } else {
        NDIlib_video_frame_v2_t frame{};
        frame.xres = width;
        frame.yres = height;
        frame.FourCC = NDIlib_FourCC_video_type_BGRX;
        frame.frame_rate_N = 30000;
        frame.frame_rate_D = 1000;
        frame.picture_aspect_ratio = static_cast<float>(width) / static_cast<float>(height);
        frame.frame_format_type = NDIlib_frame_format_type_progressive;
        frame.timecode = NDIlib_send_timecode_synthesize;
        frame.p_data = static_cast<uint8_t*>(pixels);
        frame.line_stride_in_bytes = width * 4;

        postStatus(target.kind == CaptureKind::Window
            ? L"Transmitindo janela via NDI — 30 FPS"
            : L"Transmitindo monitor via NDI — 30 FPS");
        while (!g_app.stopRequested.load()) {
            RECT currentBounds = target.bounds;
            if (target.kind == CaptureKind::Window) {
                if (!IsWindow(target.window) || IsIconic(target.window)) {
                    postStatus(L"A janela foi fechada ou minimizada. Transmissão encerrada.");
                    break;
                }
                GetWindowRect(target.window, &currentBounds);
                const int currentWidth = currentBounds.right - currentBounds.left;
                const int currentHeight = currentBounds.bottom - currentBounds.top;
                if (currentWidth != width || currentHeight != height) {
                    postStatus(L"A janela mudou de tamanho. Clique em Iniciar para reconectar.");
                    break;
                }
            }

            bool captured = false;
            if (target.kind == CaptureKind::Window) {
                captured = PrintWindow(target.window, memoryDc, 0x00000002) != FALSE;
                if (!captured) {
                    captured = BitBlt(memoryDc, 0, 0, width, height, screenDc,
                                      currentBounds.left, currentBounds.top, SRCCOPY | CAPTUREBLT) != FALSE;
                }
            } else {
                captured = BitBlt(memoryDc, 0, 0, width, height, screenDc,
                                  target.bounds.left, target.bounds.top, SRCCOPY | CAPTUREBLT) != FALSE;
            }

            if (!captured) {
                postStatus(L"Erro durante a captura da tela.");
                break;
            }
            if (showCursor) drawCursorInto(memoryDc, currentBounds);
            ndi->send_send_video_v2(sender, &frame);
        }
    }

    if (previous) SelectObject(memoryDc, previous);
    if (bitmap) DeleteObject(bitmap);
    if (memoryDc) DeleteDC(memoryDc);
    if (screenDc) ReleaseDC(nullptr, screenDc);
    ndi->send_destroy(sender);
    ndi->destroy();
    FreeLibrary(ndiModule);
    g_app.running = false;
    postStatus(L"Transmissão encerrada.");
}

void setControlsForTransmission(bool transmitting) {
    EnableWindow(g_app.sourceName, !transmitting);
    EnableWindow(g_app.sourceTypeCombo, !transmitting);
    EnableWindow(g_app.captureSourceCombo, !transmitting);
    EnableWindow(g_app.refreshButton, !transmitting);
    EnableWindow(g_app.cursorCheckbox, !transmitting);
    EnableWindow(g_app.startButton, !transmitting);
    EnableWindow(g_app.stopButton, transmitting);
}

void stopTransmission() {
    g_app.stopRequested = true;
    std::scoped_lock lock(g_app.workerMutex);
    if (g_app.worker.joinable()) g_app.worker.join();
    setControlsForTransmission(false);
}

void startTransmission() {
    if (g_app.running.load()) return;
    const int sourceIndex = static_cast<int>(SendMessageW(g_app.captureSourceCombo, CB_GETCURSEL, 0, 0));
    const CaptureKind kind = selectedCaptureKind();
    CaptureTarget target{};
    target.kind = kind;

    if (kind == CaptureKind::Monitor) {
        if (sourceIndex < 0 || sourceIndex >= static_cast<int>(g_app.monitors.size())) {
            MessageBoxW(g_app.window, L"Nenhum monitor foi encontrado.", L"Transmissor NDI", MB_ICONWARNING);
            return;
        }
        target.bounds = g_app.monitors[sourceIndex].bounds;
        target.label = g_app.monitors[sourceIndex].label;
    } else {
        if (sourceIndex < 0 || sourceIndex >= static_cast<int>(g_app.windows.size())) {
            MessageBoxW(g_app.window, L"Nenhuma janela foi selecionada. Clique em Atualizar lista.",
                        L"Transmissor NDI", MB_ICONWARNING);
            return;
        }
        target.window = g_app.windows[sourceIndex].handle;
        target.bounds = g_app.windows[sourceIndex].bounds;
        target.label = g_app.windows[sourceIndex].label;
    }

    if (target.bounds.right <= target.bounds.left || target.bounds.bottom <= target.bounds.top) {
        MessageBoxW(g_app.window, L"A origem selecionada possui tamanho inválido.",
                    L"Transmissor NDI", MB_ICONWARNING);
        return;
    }

    wchar_t nameBuffer[256]{};
    GetWindowTextW(g_app.sourceName, nameBuffer, static_cast<int>(std::size(nameBuffer)));
    std::wstring wideName(nameBuffer);
    if (wideName.empty()) wideName = L"Tela compartilhada";

    const bool showCursor = SendMessageW(g_app.cursorCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_app.stopRequested = false;
    g_app.running = true;
    setControlsForTransmission(true);
    postStatus(L"Iniciando transmissão…");

    std::scoped_lock lock(g_app.workerMutex);
    if (g_app.worker.joinable()) g_app.worker.join();
    g_app.worker = std::thread(transmit, target, utf8(wideName), showCursor);
}

HFONT createUiFont() {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    return CreateFontIndirectW(&metrics.lfMessageFont);
}

void createControls(HWND window) {
    const auto create = [window](const wchar_t* cls, const wchar_t* text, DWORD style,
                                 int x, int y, int width, int height, int id) {
        return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                               x, y, width, height, window,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               GetModuleHandleW(nullptr), nullptr);
    };

    create(L"STATIC", L"Nome da transmissão", 0, 24, 22, 470, 22, 0);
    g_app.sourceName = create(L"EDIT", L"Tela compartilhada", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                              24, 47, 470, 30, IdSourceName);
    create(L"STATIC", L"O que deseja transmitir?", 0, 24, 92, 470, 22, 0);
    g_app.sourceTypeCombo = create(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST,
                                   24, 117, 470, 100, IdSourceType);
    SendMessageW(g_app.sourceTypeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Monitor inteiro"));
    SendMessageW(g_app.sourceTypeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Janela específica"));
    SendMessageW(g_app.sourceTypeCombo, CB_SETCURSEL, 0, 0);

    create(L"STATIC", L"Monitor ou janela", 0, 24, 158, 470, 22, 0);
    g_app.captureSourceCombo = create(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                      24, 183, 350, 220, IdCaptureSource);
    g_app.refreshButton = create(L"BUTTON", L"Atualizar lista", WS_TABSTOP,
                                 386, 182, 108, 31, IdRefreshSources);
    g_app.cursorCheckbox = create(L"BUTTON", L"Mostrar cursor do mouse",
                                  WS_TABSTOP | BS_AUTOCHECKBOX, 24, 225, 260, 28, IdCursor);
    SendMessageW(g_app.cursorCheckbox, BM_SETCHECK, BST_CHECKED, 0);
    g_app.startButton = create(L"BUTTON", L"Iniciar transmissão", WS_TABSTOP | BS_DEFPUSHBUTTON,
                               24, 272, 226, 42, IdStart);
    g_app.stopButton = create(L"BUTTON", L"Parar", WS_TABSTOP,
                              268, 272, 226, 42, IdStop);
    g_app.statusLabel = create(L"STATIC", L"Pronto para transmitir.", SS_LEFT,
                               24, 334, 470, 42, IdStatus);

    HFONT font = createUiFont();
    EnumChildWindows(window, [](HWND child, LPARAM fontHandle) -> BOOL {
        SendMessageW(child, WM_SETFONT, fontHandle, TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(font));
    SetPropW(window, L"UiFont", font);

    loadCaptureSources();
    setControlsForTransmission(false);
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            g_app.window = window;
            createControls(window);
            logLine(L"Aplicativo iniciado.");
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IdStart: startTransmission(); return 0;
                case IdStop: stopTransmission(); return 0;
                case IdRefreshSources: loadCaptureSources(); return 0;
                case IdSourceType:
                    if (HIWORD(wParam) == CBN_SELCHANGE) loadCaptureSources();
                    return 0;
                default: break;
            }
            break;
        case WM_DISPLAYCHANGE:
            if (!g_app.running.load() && selectedCaptureKind() == CaptureKind::Monitor) {
                loadCaptureSources();
            }
            return 0;
        case kStatusMessage: {
            auto* status = reinterpret_cast<std::wstring*>(lParam);
            SetWindowTextW(g_app.statusLabel, status->c_str());
            if (!g_app.running.load()) setControlsForTransmission(false);
            delete status;
            return 0;
        }
        case WM_CLOSE:
            stopTransmission();
            DestroyWindow(window);
            return 0;
        case WM_DESTROY: {
            if (auto font = reinterpret_cast<HFONT>(GetPropW(window, L"UiFont"))) {
                DeleteObject(font);
                RemovePropW(window, L"UiFont");
            }
            PostQuitMessage(0);
            return 0;
        }
        default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) return 1;

    HWND window = CreateWindowExW(0, kWindowClass, L"Transmissor NDI Portátil — Protótipo",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 540, 430,
                                  nullptr, nullptr, instance, nullptr);
    if (!window) return 1;

    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
