#include <windows.h>
#include <commctrl.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <wlanapi.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "protected_ip_ui.h"

namespace {
using namespace std::chrono_literals;

constexpr wchar_t kMainWindowClass[] = L"TransmissorNDIPortatilV3";
constexpr UINT_PTR kNetworkTimer = 91;
constexpr int kIdFps = 1005;
constexpr int kIdStart = 1011;
constexpr int kTopSendId = 1401;
constexpr int kTopPerformanceId = 1402;
constexpr int kFooterPrimaryId = 1403;
constexpr int kFooterSecondaryId = 1404;

HWND gMain{};
HWND gTopSend{};
HWND gTopPerformance{};
HWND gFooterPrimary{};
HWND gFooterSecondary{};
HHOOK gHook{};

struct SampleState {
    NET_LUID luid{};
    bool haveBaseline{false};
    std::uint64_t previousOutOctets{0};
    std::chrono::steady_clock::time_point previousTime{};
};

SampleState gSample;

std::wstring controlText(HWND hwnd) {
    if (!hwnd) return {};
    const int len = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(hwnd, text.data(), len + 1);
    text.resize(static_cast<size_t>(len));
    return text;
}

bool transmissionRunning() {
    HWND start = gMain ? GetDlgItem(gMain, kIdStart) : nullptr;
    return start && controlText(start).find(L"Parar transmissão") != std::wstring::npos;
}

int selectedFps() {
    HWND fps = gMain ? GetDlgItem(gMain, kIdFps) : nullptr;
    if (!fps) return 30;
    return SendMessageW(fps, CB_GETCURSEL, 0, 0) == 1 ? 60 : 30;
}

bool guidEqual(const GUID& a, const GUID& b) {
    return InlineIsEqualGUID(a, b) != FALSE;
}

int wifiSignalQuality(const NET_LUID& luid) {
    GUID targetGuid{};
    if (ConvertInterfaceLuidToGuid(&luid, &targetGuid) != NO_ERROR) return -1;

    HANDLE client = nullptr;
    DWORD negotiated = 0;
    if (WlanOpenHandle(2, nullptr, &negotiated, &client) != ERROR_SUCCESS || !client) return -1;

    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    int quality = -1;
    if (WlanEnumInterfaces(client, nullptr, &interfaces) == ERROR_SUCCESS && interfaces) {
        for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
            const auto& item = interfaces->InterfaceInfo[i];
            if (!guidEqual(item.InterfaceGuid, targetGuid)) continue;

            DWORD size = 0;
            WLAN_OPCODE_VALUE_TYPE opcode{};
            PWLAN_CONNECTION_ATTRIBUTES attrs = nullptr;
            if (WlanQueryInterface(client, &item.InterfaceGuid,
                                   wlan_intf_opcode_current_connection, nullptr,
                                   &size, reinterpret_cast<PVOID*>(&attrs), &opcode) == ERROR_SUCCESS && attrs) {
                quality = static_cast<int>(attrs->wlanAssociationAttributes.wlanSignalQuality);
                WlanFreeMemory(attrs);
            }
            break;
        }
        WlanFreeMemory(interfaces);
    }
    WlanCloseHandle(client, nullptr);
    return quality;
}

bool preferredInterfaceIndex(ULONG& index) {
    SOCKADDR_IN target{};
    target.sin_family = AF_INET;

    const std::string trusted = configuredReceiverIp();
    if (!trusted.empty()) {
        if (InetPtonA(AF_INET, trusted.c_str(), &target.sin_addr) != 1)
            target.sin_addr.s_addr = htonl(0x08080808);
    } else {
        target.sin_addr.s_addr = htonl(0x08080808);
    }

    index = 0;
    return GetBestInterfaceEx(reinterpret_cast<SOCKADDR*>(&target), &index) == NO_ERROR && index != 0;
}

bool chooseActiveInterface(MIB_IFROW& row, bool& isWifi) {
    PMIB_IFTABLE table = nullptr;
    ULONG size = 0;
    if (GetIfTable(nullptr, &size, FALSE) != ERROR_INSUFFICIENT_BUFFER || size == 0) return false;
    std::vector<unsigned char> storage(size);
    table = reinterpret_cast<PMIB_IFTABLE>(storage.data());
    if (GetIfTable(table, &size, FALSE) != NO_ERROR) return false;

    ULONG preferred = 0;
    preferredInterfaceIndex(preferred);
    const MIB_IFROW* chosen = nullptr;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& candidate = table->table[i];
        if (candidate.dwIndex == preferred && candidate.dwOperStatus == IF_OPER_STATUS_OPERATIONAL) {
            chosen = &candidate;
            break;
        }
    }
    if (!chosen) {
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const auto& candidate = table->table[i];
            if (candidate.dwOperStatus != IF_OPER_STATUS_OPERATIONAL) continue;
            if (candidate.dwType == IF_TYPE_SOFTWARE_LOOPBACK || candidate.dwType == IF_TYPE_TUNNEL) continue;
            chosen = &candidate;
            break;
        }
    }
    if (!chosen) return false;
    row = *chosen;
    isWifi = row.dwType == IF_TYPE_IEEE80211;
    return true;
}

std::wstring formatRate(double mbps) {
    std::wostringstream out;
    if (mbps < 10.0) out << std::fixed << std::setprecision(1);
    else out << std::fixed << std::setprecision(0);
    out << mbps << L" Mbps";
    return out.str();
}

std::wstring formatLink(std::uint64_t bitsPerSecond) {
    const double mbps = static_cast<double>(bitsPerSecond) / 1'000'000.0;
    if (mbps >= 1000.0) {
        std::wostringstream out;
        out << std::fixed << std::setprecision(mbps >= 10'000.0 ? 0 : 1) << (mbps / 1000.0) << L" Gbps";
        return out.str();
    }
    std::wostringstream out;
    out << std::fixed << std::setprecision(0) << mbps << L" Mbps";
    return out.str();
}

std::wstring stabilityState(bool isWifi, int signal, double linkMbps, double outgoingMbps) {
    if (linkMbps <= 0.0) return L"Sem rede";
    const double utilization = linkMbps > 0.0 ? outgoingMbps / linkMbps : 0.0;
    if ((isWifi && signal >= 0 && signal < 35) || linkMbps < 40.0 || utilization > 0.90) return L"Instável";
    if ((isWifi && signal >= 0 && signal < 60) || linkMbps < 100.0 || utilization > 0.70) return L"Atenção";
    return L"Estável";
}

void setLabel(HWND label, const std::wstring& text) {
    if (label) SetWindowTextW(label, text.c_str());
}

void updateNetworkStatus() {
    if (!gMain) return;

    MIB_IFROW row{};
    bool isWifi = false;
    if (!chooseActiveInterface(row, isWifi)) {
        gSample = {};
        setLabel(gTopSend, L"— Mbps");
        setLabel(gTopPerformance, L"Sem rede");
        setLabel(gFooterPrimary, L"Sem rede · " + std::to_wstring(selectedFps()) + L" fps · Sem rede");
        setLabel(gFooterSecondary, L"Nenhuma interface de rede ativa foi encontrada.");
        return;
    }

    MIB_IF_ROW2 row2{};
    row2.InterfaceIndex = row.dwIndex;
    const bool haveRow2 = GetIfEntry2(&row2) == NO_ERROR;
    const std::uint64_t outOctets = haveRow2 ? row2.OutOctets : static_cast<std::uint64_t>(row.dwOutOctets);
    const std::uint64_t linkSpeed = haveRow2 ? row2.TransmitLinkSpeed : static_cast<std::uint64_t>(row.dwSpeed);
    NET_LUID luid{};
    if (haveRow2) luid = row2.InterfaceLuid;
    else ConvertInterfaceIndexToLuid(row.dwIndex, &luid);

    const auto now = std::chrono::steady_clock::now();
    double outgoingMbps = 0.0;
    const bool sameInterface = gSample.haveBaseline && gSample.luid.Value == luid.Value;
    if (sameInterface) {
        const double seconds = std::chrono::duration<double>(now - gSample.previousTime).count();
        if (seconds > 0.05 && outOctets >= gSample.previousOutOctets)
            outgoingMbps = static_cast<double>(outOctets - gSample.previousOutOctets) * 8.0 / seconds / 1'000'000.0;
    }
    gSample.luid = luid;
    gSample.previousOutOctets = outOctets;
    gSample.previousTime = now;
    gSample.haveBaseline = true;

    const double linkMbps = static_cast<double>(linkSpeed) / 1'000'000.0;
    const int signal = isWifi ? wifiSignalQuality(luid) : -1;
    const std::wstring state = stabilityState(isWifi, signal, linkMbps, outgoingMbps);
    const int fps = selectedFps();

    std::wstring networkName = isWifi ? L"Wi-Fi" : L"Ethernet";
    if (isWifi && signal >= 0) networkName += L" " + std::to_wstring(signal) + L"%";
    std::wstring primary = networkName + L" · " + formatLink(linkSpeed) + L" · " + std::to_wstring(fps) + L" fps · " + state;
    std::wstring secondary = transmissionRunning()
        ? L"Envio pela interface ativa: " + formatRate(outgoingMbps)
        : L"Rede pronta · a taxa de envio aparece durante a transmissão.";

    setLabel(gTopSend, transmissionRunning() ? formatRate(outgoingMbps) : L"— Mbps");
    setLabel(gTopPerformance, transmissionRunning() ? state : L"Aguardando");
    setLabel(gFooterPrimary, primary);
    setLabel(gFooterSecondary, secondary);
}

HWND addStatic(HWND parent, int id, int x, int y, int w, int h, HFONT font) {
    HWND label = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (font) SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return label;
}

LRESULT CALLBACK subclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_TIMER && wp == kNetworkTimer) {
        updateNetworkStatus();
        return 0;
    }
    if (msg == WM_COMMAND) {
        const int id = LOWORD(wp);
        if (id == kIdFps || id == kIdStart) updateNetworkStatus();
    } else if (msg == WM_NCDESTROY) {
        KillTimer(hwnd, kNetworkTimer);
        RemoveWindowSubclass(hwnd, subclassProc, 3);
        gMain = nullptr;
        gTopSend = nullptr;
        gTopPerformance = nullptr;
        gFooterPrimary = nullptr;
        gFooterSecondary = nullptr;
        gSample = {};
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void installUi(HWND hwnd) {
    if (gMain || !hwnd) return;
    gMain = hwnd;
    HFONT normal = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HWND fps = GetDlgItem(hwnd, kIdFps);
    if (fps) {
        HFONT candidate = reinterpret_cast<HFONT>(SendMessageW(fps, WM_GETFONT, 0, 0));
        if (candidate) normal = candidate;
    }
    gTopSend = addStatic(hwnd, kTopSendId, 510, 103, 190, 27, normal);
    gTopPerformance = addStatic(hwnd, kTopPerformanceId, 748, 103, 190, 27, normal);
    gFooterPrimary = addStatic(hwnd, kFooterPrimaryId, 32, 678, 690, 26, normal);
    gFooterSecondary = addStatic(hwnd, kFooterSecondaryId, 32, 704, 690, 22, normal);
    SetWindowSubclass(hwnd, subclassProc, 3, 0);
    SetTimer(hwnd, kNetworkTimer, 1000, nullptr);
    updateNetworkStatus();
}

LRESULT CALLBACK callWndHook(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && !gMain) {
        const auto* data = reinterpret_cast<CWPSTRUCT*>(lp);
        if (data && data->hwnd && data->message == WM_PAINT) {
            wchar_t className[128]{};
            GetClassNameW(data->hwnd, className, static_cast<int>(std::size(className)));
            if (wcscmp(className, kMainWindowClass) == 0) installUi(data->hwnd);
        }
    }
    return CallNextHookEx(gHook, code, wp, lp);
}

struct NetworkBootstrap {
    NetworkBootstrap() { gHook = SetWindowsHookExW(WH_CALLWNDPROC, callWndHook, nullptr, GetCurrentThreadId()); }
    ~NetworkBootstrap() { if (gHook) UnhookWindowsHookEx(gHook); }
} gBootstrap;
}
