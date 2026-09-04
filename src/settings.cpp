#include "settings.h"
#include <windows.h>

namespace {
std::wstring readString(const std::filesystem::path& path, const wchar_t* key, const wchar_t* fallback) {
    wchar_t buffer[512]{};
    GetPrivateProfileStringW(L"app", key, fallback, buffer, 512, path.c_str());
    return buffer;
}
int readInt(const std::filesystem::path& path, const wchar_t* key, int fallback) {
    return GetPrivateProfileIntW(L"app", key, fallback, path.c_str());
}
void writeInt(const std::filesystem::path& path, const wchar_t* key, int value) {
    WritePrivateProfileStringW(L"app", key, std::to_wstring(value).c_str(), path.c_str());
}
}

AppSettings loadSettings(const std::filesystem::path& iniPath) {
    AppSettings s;
    s.sourceName = readString(iniPath, L"sourceName", L"Zosma NDI");
    s.captureKind = readInt(iniPath, L"captureKind", 0) == 1 ? CaptureKind::Window : CaptureKind::Monitor;
    s.sourceIndex = readInt(iniPath, L"sourceIndex", 0);
    s.resolutionIndex = readInt(iniPath, L"resolutionIndex", 0);
    s.fps = readInt(iniPath, L"fps", 30) == 60 ? 60 : 30;
    s.showCursor = readInt(iniPath, L"showCursor", 1) != 0;
    s.mode = readInt(iniPath, L"mode", 0) == 1 ? TransmissionMode::Quick : TransmissionMode::Protected;
    s.showHelpOnStart = readInt(iniPath, L"showHelpOnStart", 1) != 0;
    return s;
}

void saveSettings(const std::filesystem::path& iniPath, const AppSettings& s) {
    WritePrivateProfileStringW(L"app", L"sourceName", s.sourceName.c_str(), iniPath.c_str());
    writeInt(iniPath, L"captureKind", s.captureKind == CaptureKind::Window ? 1 : 0);
    writeInt(iniPath, L"sourceIndex", s.sourceIndex);
    writeInt(iniPath, L"resolutionIndex", s.resolutionIndex);
    writeInt(iniPath, L"fps", s.fps);
    writeInt(iniPath, L"showCursor", s.showCursor ? 1 : 0);
    writeInt(iniPath, L"mode", s.mode == TransmissionMode::Quick ? 1 : 0);
    writeInt(iniPath, L"showHelpOnStart", s.showHelpOnStart ? 1 : 0);
}
