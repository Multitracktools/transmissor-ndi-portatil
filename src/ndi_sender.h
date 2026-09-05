#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

class NdiSender {
public:
    NdiSender() = default;
    ~NdiSender();
    NdiSender(const NdiSender&) = delete;
    NdiSender& operator=(const NdiSender&) = delete;

    bool open(const std::filesystem::path& runtimeDll, const std::string& sourceName, std::wstring& error);
    void close();
    bool sendFrame(const std::uint8_t* data, int width, int height, int fps);
    int connections() const;
    bool valid() const;

private:
    void* module_{};
    const void* api_{};
    void* sender_{};
    std::vector<std::uint8_t> asyncVideoBuffers_[2];
    int asyncVideoIndex_{0};
    bool asyncVideoSubmitted_{false};
};
