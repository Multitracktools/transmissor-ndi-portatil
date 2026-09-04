#pragma once
#include "app_types.h"

std::vector<MonitorSource> enumerateMonitors();
std::vector<WindowSource> enumerateWindows(HWND appWindow);
bool captureToBuffer(const CaptureSource& source, bool showCursor, std::vector<unsigned char>& bgra, int& width, int& height);
