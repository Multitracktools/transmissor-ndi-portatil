#pragma once
#include "app_types.h"
#include <filesystem>

AppSettings loadSettings(const std::filesystem::path& iniPath);
void saveSettings(const std::filesystem::path& iniPath, const AppSettings& settings);
