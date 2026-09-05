#pragma once
#include <windows.h>
#include <string>
#include <vector>

enum class CaptureKind { Monitor, Window };
enum class TransmissionMode { Protected, Quick };

struct MonitorSource {
    HMONITOR handle{};
    RECT bounds{};
    std::wstring label;
};

struct WindowSource {
    HWND handle{};
    RECT bounds{};
    std::wstring label;
};

struct CaptureSource {
    CaptureKind kind{CaptureKind::Monitor};
    RECT bounds{};
    HWND window{};
    HMONITOR monitor{};
    std::wstring label;
};

struct AppSettings {
    std::wstring sourceName{L"Zosma Transmitter"};
    CaptureKind captureKind{CaptureKind::Monitor};
    int sourceIndex{0};
    int resolutionIndex{0};
    int fps{30};
    bool showCursor{true};
    TransmissionMode mode{TransmissionMode::Protected};
    bool showHelpOnStart{true};
};
