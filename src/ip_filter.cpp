#include "ip_filter.h"

#include <windows.h>
#include <fwpmu.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <string>

namespace {
HANDLE gEngine = nullptr;

bool parseIpv4(const std::string& text, UINT32& hostOrder) {
    IN_ADDR address{};
    if (InetPtonA(AF_INET, text.c_str(), &address) != 1) return false;
    hostOrder = ntohl(address.S_un.S_addr);
    return true;
}

bool addFilter(const FWPM_FILTER_CONDITION0* conditions, UINT32 conditionCount,
               const GUID& layer, FWP_ACTION_TYPE action, UINT64 weight,
               const wchar_t* name, std::wstring& error) {
    FWPM_FILTER0 filter{};
    filter.displayData.name = const_cast<wchar_t*>(name);
    filter.layerKey = layer;
    filter.subLayerKey = FWPM_SUBLAYER_UNIVERSAL;
    filter.action.type = action;
    filter.weight.type = FWP_UINT64;
    filter.weight.uint64 = &weight;
    filter.numFilterConditions = conditionCount;
    filter.filterCondition = const_cast<FWPM_FILTER_CONDITION0*>(conditions);
    const DWORD status = FwpmFilterAdd0(gEngine, &filter, nullptr, nullptr);
    if (status != ERROR_SUCCESS) {
        error = L"FwpmFilterAdd falhou: " + std::to_wstring(status);
        return false;
    }
    return true;
}
}

bool installReceiverIpFilter(const std::string& allowedIpv4, std::wstring& error) {
    removeReceiverIpFilter();

    UINT32 allowed = 0;
    if (!parseIpv4(allowedIpv4, allowed)) {
        error = L"IP autorizado inválido.";
        return false;
    }

    wchar_t exePath[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
    if (!length) {
        error = L"Não foi possível obter o caminho do aplicativo.";
        return false;
    }

    FWP_BYTE_BLOB* appId = nullptr;
    DWORD status = FwpmGetAppIdFromFileName0(exePath, &appId);
    if (status != ERROR_SUCCESS || !appId) {
        error = L"Não foi possível identificar o aplicativo no Windows Filtering Platform.";
        return false;
    }

    FWPM_SESSION0 session{};
    session.flags = FWPM_SESSION_FLAG_DYNAMIC;
    session.displayData.name = const_cast<wchar_t*>(L"Transmissor NDI - filtro IP temporário");
    status = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, &session, &gEngine);
    if (status != ERROR_SUCCESS) {
        FwpmFreeMemory0(reinterpret_cast<void**>(&appId));
        gEngine = nullptr;
        error = L"Não foi possível abrir o Windows Filtering Platform. Execute como administrador.";
        return false;
    }

    // O NDI usa tráfego UDP/multicast para descoberta. Se bloquearmos todo o tráfego
    // do processo para outros IPs, a fonte deixa de aparecer nos receptores.
    // Portanto o filtro seletivo atua somente em TCP, que é onde observamos as
    // conexões de mídia/controle dos receptores no teste real.
    FWPM_FILTER_CONDITION0 allowConditions[3]{};
    allowConditions[0].fieldKey = FWPM_CONDITION_ALE_APP_ID;
    allowConditions[0].matchType = FWP_MATCH_EQUAL;
    allowConditions[0].conditionValue.type = FWP_BYTE_BLOB_TYPE;
    allowConditions[0].conditionValue.byteBlob = appId;
    allowConditions[1].fieldKey = FWPM_CONDITION_IP_PROTOCOL;
    allowConditions[1].matchType = FWP_MATCH_EQUAL;
    allowConditions[1].conditionValue.type = FWP_UINT8;
    allowConditions[1].conditionValue.uint8 = IPPROTO_TCP;
    allowConditions[2].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
    allowConditions[2].matchType = FWP_MATCH_EQUAL;
    allowConditions[2].conditionValue.type = FWP_UINT32;
    allowConditions[2].conditionValue.uint32 = allowed;

    FWPM_FILTER_CONDITION0 blockConditions[2]{};
    blockConditions[0].fieldKey = FWPM_CONDITION_ALE_APP_ID;
    blockConditions[0].matchType = FWP_MATCH_EQUAL;
    blockConditions[0].conditionValue.type = FWP_BYTE_BLOB_TYPE;
    blockConditions[0].conditionValue.byteBlob = appId;
    blockConditions[1].fieldKey = FWPM_CONDITION_IP_PROTOCOL;
    blockConditions[1].matchType = FWP_MATCH_EQUAL;
    blockConditions[1].conditionValue.type = FWP_UINT8;
    blockConditions[1].conditionValue.uint8 = IPPROTO_TCP;

    bool ok = true;
    ok = ok && addFilter(allowConditions, 3, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4,
                         FWP_ACTION_PERMIT, 0xF000000000000000ULL, L"NDI permitir TCP do IP autorizado (entrada)", error);
    ok = ok && addFilter(blockConditions, 2, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4,
                         FWP_ACTION_BLOCK, 0x1000000000000000ULL, L"NDI bloquear TCP de outros IPs (entrada)", error);
    ok = ok && addFilter(allowConditions, 3, FWPM_LAYER_ALE_AUTH_CONNECT_V4,
                         FWP_ACTION_PERMIT, 0xF000000000000000ULL, L"NDI permitir TCP do IP autorizado (saída)", error);
    ok = ok && addFilter(blockConditions, 2, FWPM_LAYER_ALE_AUTH_CONNECT_V4,
                         FWP_ACTION_BLOCK, 0x1000000000000000ULL, L"NDI bloquear TCP de outros IPs (saída)", error);

    FwpmFreeMemory0(reinterpret_cast<void**>(&appId));
    if (!ok) removeReceiverIpFilter();
    return ok;
}

void removeReceiverIpFilter() {
    if (!gEngine) return;
    FwpmEngineClose0(gEngine);
    gEngine = nullptr;
}

bool receiverIpFilterActive() { return gEngine != nullptr; }
