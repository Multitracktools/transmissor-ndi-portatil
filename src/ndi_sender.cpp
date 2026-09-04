#include "ndi_sender.h"
#include "ip_filter.h"
#include <windows.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "Processing.NDI.Lib.h"

namespace {
using NdiLoadFunction = const NDIlib_v6* (*)();
using namespace std::chrono_literals;

std::string gTrustedIp;

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

std::set<std::string> establishedRemoteIps() {
    std::set<std::string> ips;
    ULONG bytes = 0;
    DWORD result = GetExtendedTcpTable(nullptr, &bytes, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (result != ERROR_INSUFFICIENT_BUFFER || bytes == 0) return ips;
    std::vector<unsigned char> storage(bytes);
    auto* table = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(storage.data());
    result = GetExtendedTcpTable(table, &bytes, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (result != NO_ERROR) return ips;

    const DWORD pid = GetCurrentProcessId();
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if (row.dwOwningPid != pid || row.dwState != MIB_TCP_STATE_ESTAB) continue;
        const std::string ip = ipv4ToString(row.dwRemoteAddr);
        if (!ip.empty() && ip != "?" && ip != "0.0.0.0" && ip != "127.0.0.1") ips.insert(ip);
    }
    return ips;
}

void tryLockToFirstReceiver() {
    if (!gTrustedIp.empty()) return;
    const auto ips = establishedRemoteIps();
    if (ips.size() != 1) return;
    gTrustedIp = *ips.begin();
    std::wstring error;
    if (!installReceiverIpFilter(gTrustedIp, error)) gTrustedIp.clear();
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

    tryLockToFirstReceiver();

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
    snapshot << "Experimental trusted IP: " << (gTrustedIp.empty() ? "(waiting for exactly one receiver)" : gTrustedIp) << "\n";
    snapshot << "Selective IP filter: " << (receiverIpFilterActive() ? "ACTIVE" : "inactive") << "\n";

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
    gTrustedIp.clear();
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
    const int count = api->send_get_no_connections(static_cast<NDIlib_send_instance_t>(sender_), 0);
    logRemoteEndpoints(count);
    return count;
}

bool NdiSender::valid() const { return api_ && sender_; }
