#include "ndi_sender.h"
#include "ip_filter.h"
#include "protected_ip_ui.h"

#include <windows.h>
#include <audioclient.h>
#include <iphlpapi.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <objbase.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Processing.NDI.Lib.h"

namespace {
using NdiLoadFunction = const NDIlib_v6* (*)();
using namespace std::chrono_literals;

std::string gTrustedIp;

class WasapiLoopback {
public:
    ~WasapiLoopback() { close(); }

    bool open() {
        close();

        const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(init)) comInitialized_ = true;
        else if (init != RPC_E_CHANGED_MODE) return false;

        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator_));
        if (FAILED(hr)) { close(); return false; }

        hr = enumerator_->GetDefaultAudioEndpoint(eRender, eMultimedia, &device_);
        if (FAILED(hr)) { close(); return false; }

        hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(&audioClient_));
        if (FAILED(hr)) { close(); return false; }

        hr = audioClient_->GetMixFormat(&format_);
        if (FAILED(hr) || !format_) { close(); return false; }

        hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                      0, 0, format_, nullptr);
        if (FAILED(hr)) { close(); return false; }

        hr = audioClient_->GetService(__uuidof(IAudioCaptureClient),
                                      reinterpret_cast<void**>(&captureClient_));
        if (FAILED(hr)) { close(); return false; }

        hr = audioClient_->Start();
        if (FAILED(hr)) { close(); return false; }

        started_ = true;
        return true;
    }

    void close() {
        if (audioClient_ && started_) audioClient_->Stop();
        started_ = false;
        if (captureClient_) { captureClient_->Release(); captureClient_ = nullptr; }
        if (audioClient_) { audioClient_->Release(); audioClient_ = nullptr; }
        if (device_) { device_->Release(); device_ = nullptr; }
        if (enumerator_) { enumerator_->Release(); enumerator_ = nullptr; }
        if (format_) { CoTaskMemFree(format_); format_ = nullptr; }
        if (comInitialized_) { CoUninitialize(); comInitialized_ = false; }
    }

    void drain(const NDIlib_v6* api, NDIlib_send_instance_t sender, bool transmit) {
        if (!started_ || !captureClient_ || !format_ || !api || !sender) return;

        UINT32 packetFrames = 0;
        while (SUCCEEDED(captureClient_->GetNextPacketSize(&packetFrames)) && packetFrames > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(captureClient_->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;

            if (transmit && frames > 0) sendPacket(api, sender, data, frames, flags);
            captureClient_->ReleaseBuffer(frames);
            packetFrames = 0;
        }
    }

private:
    bool formatIsFloat() const {
        if (!format_) return false;
        if (format_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
        if (format_->wFormatTag != WAVE_FORMAT_EXTENSIBLE) return false;
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format_);
        return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE;
    }

    bool formatIsPcm() const {
        if (!format_) return false;
        if (format_->wFormatTag == WAVE_FORMAT_PCM) return true;
        if (format_->wFormatTag != WAVE_FORMAT_EXTENSIBLE) return false;
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format_);
        return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != FALSE;
    }

    float readSample(const BYTE* data, size_t sampleIndex) const {
        if (!format_ || !data) return 0.0f;
        const int bits = format_->wBitsPerSample;
        const size_t bytesPerSample = static_cast<size_t>((bits + 7) / 8);
        const BYTE* p = data + sampleIndex * bytesPerSample;

        if (formatIsFloat() && bits == 32) {
            float value = 0.0f;
            std::memcpy(&value, p, sizeof(value));
            return value;
        }
        if (!formatIsPcm()) return 0.0f;

        if (bits == 8) {
            return (static_cast<int>(*p) - 128) / 128.0f;
        }
        if (bits == 16) {
            std::int16_t value = 0;
            std::memcpy(&value, p, sizeof(value));
            return static_cast<float>(value) / 32768.0f;
        }
        if (bits == 24) {
            std::int32_t value = static_cast<std::int32_t>(p[0]) |
                                 (static_cast<std::int32_t>(p[1]) << 8) |
                                 (static_cast<std::int32_t>(p[2]) << 16);
            if (value & 0x00800000) value |= static_cast<std::int32_t>(0xFF000000);
            return static_cast<float>(value) / 8388608.0f;
        }
        if (bits == 32) {
            std::int32_t value = 0;
            std::memcpy(&value, p, sizeof(value));
            return static_cast<float>(static_cast<double>(value) / 2147483648.0);
        }
        return 0.0f;
    }

    void sendPacket(const NDIlib_v6* api, NDIlib_send_instance_t sender,
                    const BYTE* data, UINT32 frames, DWORD flags) {
        const int channels = static_cast<int>(format_->nChannels);
        if (channels <= 0 || frames == 0) return;

        std::vector<float> planar(static_cast<size_t>(channels) * frames, 0.0f);
        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && data) {
            for (UINT32 frame = 0; frame < frames; ++frame) {
                for (int ch = 0; ch < channels; ++ch) {
                    const size_t interleaved = static_cast<size_t>(frame) * channels + static_cast<size_t>(ch);
                    planar[static_cast<size_t>(ch) * frames + frame] = readSample(data, interleaved);
                }
            }
        }

        NDIlib_audio_frame_v2_t audio{};
        audio.sample_rate = static_cast<int>(format_->nSamplesPerSec);
        audio.no_channels = channels;
        audio.no_samples = static_cast<int>(frames);
        audio.timecode = NDIlib_send_timecode_synthesize;
        audio.p_data = planar.data();
        audio.channel_stride_in_bytes = static_cast<int>(frames * sizeof(float));
        api->send_send_audio_v2(sender, &audio);
    }

    bool comInitialized_{false};
    bool started_{false};
    IMMDeviceEnumerator* enumerator_{};
    IMMDevice* device_{};
    IAudioClient* audioClient_{};
    IAudioCaptureClient* captureClient_{};
    WAVEFORMATEX* format_{};
};

WasapiLoopback gAudioCapture;

std::filesystem::path executableDirectory() {
    std::vector<wchar_t> buffer(32768);
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return std::filesystem::path(std::wstring(buffer.data(), len)).parent_path();
}

std::string ipv4ToString(DWORD address) {
    IN_ADDR addr{};
    addr.S_un.S_addr = address;
    char text[INET_ADDRSTRLEN]{};
    if (!InetNtopA(AF_INET, &addr, text, static_cast<DWORD>(sizeof(text)))) return "?";
    return text;
}

std::string tcpStateName(DWORD state) {
    switch (state) {
    case MIB_TCP_STATE_CLOSED: return "CLOSED";
    case MIB_TCP_STATE_LISTEN: return "LISTEN";
    case MIB_TCP_STATE_SYN_SENT: return "SYN_SENT";
    case MIB_TCP_STATE_SYN_RCVD: return "SYN_RCVD";
    case MIB_TCP_STATE_ESTAB: return "ESTABLISHED";
    case MIB_TCP_STATE_FIN_WAIT1: return "FIN_WAIT1";
    case MIB_TCP_STATE_FIN_WAIT2: return "FIN_WAIT2";
    case MIB_TCP_STATE_CLOSE_WAIT: return "CLOSE_WAIT";
    case MIB_TCP_STATE_CLOSING: return "CLOSING";
    case MIB_TCP_STATE_LAST_ACK: return "LAST_ACK";
    case MIB_TCP_STATE_TIME_WAIT: return "TIME_WAIT";
    case MIB_TCP_STATE_DELETE_TCB: return "DELETE_TCB";
    default: return "UNKNOWN";
    }
}

void logRemoteEndpoints(int ndiConnections) {
    static auto lastScan = std::chrono::steady_clock::now() - 10s;
    static std::string lastSnapshot;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastScan < 1s) return;
    lastScan = now;

    ULONG bytes = 0;
    DWORD result = GetExtendedTcpTable(nullptr, &bytes, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (result != ERROR_INSUFFICIENT_BUFFER || bytes == 0) return;

    std::vector<unsigned char> storage(bytes);
    auto* table = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(storage.data());
    result = GetExtendedTcpTable(table, &bytes, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (result != NO_ERROR) return;

    const DWORD pid = GetCurrentProcessId();
    std::ostringstream snapshot;
    snapshot << "NDI technical connections: " << ndiConnections << "\n";
    snapshot << "Configured trusted IP: " << (gTrustedIp.empty() ? "(none - quick mode)" : gTrustedIp) << "\n";
    snapshot << "Selective IP filter: " << (receiverIpFilterActive() ? "ACTIVE" : "inactive") << "\n";
    snapshot << "System audio: " << (audioTransmissionAllowed() ? "sending" : "muted") << "\n";

    int ownedRows = 0;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if (row.dwOwningPid != pid) continue;
        if (row.dwState == MIB_TCP_STATE_LISTEN) continue;
        ++ownedRows;
        snapshot << "  "
                 << ipv4ToString(row.dwLocalAddr) << ':' << ntohs(static_cast<u_short>(row.dwLocalPort))
                 << " -> "
                 << ipv4ToString(row.dwRemoteAddr) << ':' << ntohs(static_cast<u_short>(row.dwRemotePort))
                 << "  [" << tcpStateName(row.dwState) << "]\n";
    }
    if (ownedRows == 0) snapshot << "  (no non-listening TCP endpoints owned by this process)\n";

    const std::string current = snapshot.str();
    if (current == lastSnapshot) return;
    lastSnapshot = current;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::ofstream out(executableDirectory() / L"ndi-ip-debug.txt", std::ios::app | std::ios::binary);
    if (!out) return;
    out << "\r\n=== "
        << st.wYear << '-';
    if (st.wMonth < 10) out << '0';
    out << st.wMonth << '-';
    if (st.wDay < 10) out << '0';
    out << st.wDay << ' ';
    if (st.wHour < 10) out << '0';
    out << st.wHour << ':';
    if (st.wMinute < 10) out << '0';
    out << st.wMinute << ':';
    if (st.wSecond < 10) out << '0';
    out << st.wSecond << " ===\r\n";
    std::string crlf = current;
    for (size_t pos = 0; (pos = crlf.find('\n', pos)) != std::string::npos; pos += 2) crlf.replace(pos, 1, "\r\n");
    out << crlf;
}
}

NdiSender::~NdiSender() { close(); }

bool NdiSender::open(const std::filesystem::path& runtimeDll, const std::string& sourceName, std::wstring& error) {
    close();
    gTrustedIp = configuredReceiverIp();
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

    if (!gTrustedIp.empty()) {
        if (!installReceiverIpFilter(gTrustedIp, error)) {
            close();
            return false;
        }
    }

    // O áudio é capturado por loopback WASAPI da saída padrão do Windows.
    // Falha de áudio não impede a transmissão de vídeo.
    gAudioCapture.open();
    return true;
}

void NdiSender::close() {
    gAudioCapture.close();
    removeReceiverIpFilter();
    gTrustedIp.clear();
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
    auto sender = static_cast<NDIlib_send_instance_t>(sender_);

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
    api->send_send_video_v2(sender, &frame);

    // Mantém o loopback drenado o tempo todo, mas só envia áudio quando a imagem
    // também está autorizada. No modo protegido e durante privacidade o áudio fica mudo.
    gAudioCapture.drain(api, sender, audioTransmissionAllowed());
    return true;
}

int NdiSender::connections() const {
    auto* api = static_cast<const NDIlib_v6*>(api_);
    if (!api || !sender_) return 0;
    const int technicalCount = api->send_get_no_connections(static_cast<NDIlib_send_instance_t>(sender_), 0);
    logRemoteEndpoints(technicalCount);
    // O modo por IP trata todas as conexões técnicas da máquina permitida como um único receptor lógico.
    return technicalCount > 0 ? 1 : 0;
}

bool NdiSender::valid() const { return api_ && sender_; }
