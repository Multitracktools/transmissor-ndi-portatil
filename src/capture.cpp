#include "capture.h"
#include <algorithm>

namespace {
BOOL CALLBACK enumMonitorProc(HMONITOR monitor, HDC, LPRECT bounds, LPARAM data) {
    auto* out = reinterpret_cast<std::vector<MonitorSource>*>(data);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);
    const int index = static_cast<int>(out->size()) + 1;
    const int w = bounds->right - bounds->left;
    const int h = bounds->bottom - bounds->top;
    std::wstring label = L"Monitor " + std::to_wstring(index) + L" — " + std::to_wstring(w) + L" × " + std::to_wstring(h);
    if (info.dwFlags & MONITORINFOF_PRIMARY) label += L" (principal)";
    out->push_back({monitor, *bounds, std::move(label)});
    return TRUE;
}

struct WindowEnumContext { HWND app{}; std::vector<WindowSource>* out{}; };
BOOL CALLBACK enumWindowProc(HWND hwnd, LPARAM data) {
    auto* ctx = reinterpret_cast<WindowEnumContext*>(data);
    if (hwnd == ctx->app || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    const int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return TRUE;
    RECT r{};
    if (!GetWindowRect(hwnd, &r) || r.right <= r.left || r.bottom <= r.top) return TRUE;
    std::wstring title(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(hwnd, title.data(), len + 1);
    title.resize(static_cast<size_t>(len));
    ctx->out->push_back({hwnd, r, std::move(title)});
    return TRUE;
}

void drawCursor(HDC dc, const RECT& bounds) {
    CURSORINFO cursor{};
    cursor.cbSize = sizeof(cursor);
    if (!GetCursorInfo(&cursor) || cursor.flags != CURSOR_SHOWING || !PtInRect(&bounds, cursor.ptScreenPos)) return;
    ICONINFO icon{};
    int x = cursor.ptScreenPos.x - bounds.left;
    int y = cursor.ptScreenPos.y - bounds.top;
    if (GetIconInfo(cursor.hCursor, &icon)) {
        x -= static_cast<int>(icon.xHotspot);
        y -= static_cast<int>(icon.yHotspot);
        if (icon.hbmMask) DeleteObject(icon.hbmMask);
        if (icon.hbmColor) DeleteObject(icon.hbmColor);
    }
    DrawIconEx(dc, x, y, cursor.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
}
}

std::vector<MonitorSource> enumerateMonitors() {
    std::vector<MonitorSource> result;
    EnumDisplayMonitors(nullptr, nullptr, enumMonitorProc, reinterpret_cast<LPARAM>(&result));
    return result;
}

std::vector<WindowSource> enumerateWindows(HWND appWindow) {
    std::vector<WindowSource> result;
    WindowEnumContext ctx{appWindow, &result};
    EnumWindows(enumWindowProc, reinterpret_cast<LPARAM>(&ctx));
    return result;
}

bool captureToBuffer(const CaptureSource& source, bool showCursor, std::vector<unsigned char>& bgra, int& width, int& height) {
    RECT bounds = source.bounds;
    if (source.kind == CaptureKind::Window) {
        if (!IsWindow(source.window) || IsIconic(source.window) || !GetWindowRect(source.window, &bounds)) return false;
    }
    width = bounds.right - bounds.left;
    height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) return false;

    HDC screen = GetDC(nullptr);
    HDC mem = screen ? CreateCompatibleDC(screen) : nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = mem ? CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
    if (!screen || !mem || !bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        if (mem) DeleteDC(mem);
        if (screen) ReleaseDC(nullptr, screen);
        return false;
    }
    HGDIOBJ old = SelectObject(mem, bitmap);
    bool ok = false;
    if (source.kind == CaptureKind::Window) {
        ok = PrintWindow(source.window, mem, 2) != 0;
        if (!ok) ok = BitBlt(mem, 0, 0, width, height, screen, bounds.left, bounds.top, SRCCOPY | CAPTUREBLT) != 0;
    } else {
        ok = BitBlt(mem, 0, 0, width, height, screen, bounds.left, bounds.top, SRCCOPY | CAPTUREBLT) != 0;
        if (ok && showCursor) drawCursor(mem, bounds);
    }
    if (ok) {
        const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
        bgra.resize(bytes);
        std::copy_n(static_cast<unsigned char*>(pixels), bytes, bgra.data());
    }
    SelectObject(mem, old);
    DeleteObject(bitmap);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}
