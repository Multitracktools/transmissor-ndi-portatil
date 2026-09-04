#include "ndi_sender.h"
#include <windows.h>
#include "Processing.NDI.Lib.h"

namespace {
using NdiLoadFunction = const NDIlib_v6* (*)();
}

NdiSender::~NdiSender() { close(); }

bool NdiSender::open(const std::filesystem::path& runtimeDll, const std::string& sourceName, std::wstring& error) {
    close();
    HMODULE module = LoadLibraryW(runtimeDll.c_str());
    if (!module) { error = L"Não foi possível carregar Processing.NDI.Lib.x64.dll."; return false; }
    auto load = reinterpret_cast<NdiLoadFunction>(GetProcAddress(module, "NDIlib_v6_load"));
    const NDIlib_v6* api = load ? load() : nullptr;
    if (!api || !api->initialize()) {
        FreeLibrary(module);
        error = L"Não foi possível inicializar o runtime NDI.";
        return false;
    }
    NDIlib_send_create_t create{};
    create.p_ndi_name = sourceName.c_str();
    create.clock_video = true;
    create.clock_audio = false;
    NDIlib_send_instance_t sender = api->send_create(&create);
    if (!sender) {
        api->destroy();
        FreeLibrary(module);
        error = L"Não foi possível criar a fonte NDI.";
        return false;
    }
    module_ = module;
    api_ = api;
    sender_ = sender;
    return true;
}

void NdiSender::close() {
    auto* api = static_cast<const NDIlib_v6*>(api_);
    if (api && sender_) api->send_destroy(static_cast<NDIlib_send_instance_t>(sender_));
    if (api) api->destroy();
    if (module_) FreeLibrary(static_cast<HMODULE>(module_));
    module_ = nullptr;
    api_ = nullptr;
    sender_ = nullptr;
}

bool NdiSender::sendFrame(const std::uint8_t* data, int width, int height, int fps) {
    auto* api = static_cast<const NDIlib_v6*>(api_);
    if (!api || !sender_ || !data || width <= 0 || height <= 0 || fps <= 0) return false;
    NDIlib_video_frame_v2_t frame{};
    frame.xres = width;
    frame.yres = height;
    frame.FourCC = NDIlib_FourCC_video_type_BGRX;
    frame.frame_rate_N = fps * 1000;
    frame.frame_rate_D = 1000;
    frame.picture_aspect_ratio = static_cast<float>(width) / static_cast<float>(height);
    frame.frame_format_type = NDIlib_frame_format_type_progressive;
    frame.timecode = NDIlib_send_timecode_synthesize;
    frame.p_data = const_cast<std::uint8_t*>(data);
    frame.line_stride_in_bytes = width * 4;
    api->send_send_video_v2(static_cast<NDIlib_send_instance_t>(sender_), &frame);
    return true;
}

int NdiSender::connections() const {
    auto* api = static_cast<const NDIlib_v6*>(api_);
    if (!api || !sender_) return 0;
    return api->send_get_no_connections(static_cast<NDIlib_send_instance_t>(sender_), 0);
}

bool NdiSender::valid() const { return api_ && sender_; }
