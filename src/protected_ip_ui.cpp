#include "protected_ip_ui.h"

#include <windows.h>
#include <commctrl.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <uxtheme.h>
#include <ws2tcpip.h>

#include <atomic>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace {
constexpr int kIdProtected = 1007;
constexpr int kIdQuick = 1008;
constexpr int kIdStart = 1011;
constexpr int kIdRelease = 1012;
constexpr int kIdStatus = 1014;
constexpr int kIpEditId = 1201;
constexpr int kIpLabelId = 1202;
constexpr int kIpHintId = 1203;
constexpr int kAudioComboId = 1210;

HWND gMain{};
HWND gIpEdit{};
HWND gIpLabel{};
HWND gIpHint{};
HWND gAudioCombo{};
std::mutex gIpMutex;
std::string gConfiguredIp;
std::wstring gConfiguredAudioDeviceId;
std::vector<std::wstring> gAudioDeviceIds;
std::atomic_bool gReleased{false};
std::atomic_bool gAudioAllowed{false};
HHOOK gHook{};

std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring controlText(HWND hwnd) {
    if (!hwnd) return {};
    const int len = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(hwnd, text.data(), len + 1);
    text.resize(static_cast<size_t>(len));
    return text;
}

bool validIpv4(const std::wstring& text) {
    IN_ADDR address{};
    return !text.empty() && InetPtonW(AF_INET, text.c_str(), &address) == 1;
}

bool protectedMode() {
    HWND protectedButton = gMain ? GetDlgItem(gMain, kIdProtected) : nullptr;
    return protectedButton && SendMessageW(protectedButton, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

bool readyToStart() {
    HWND start = gMain ? GetDlgItem(gMain, kIdStart) : nullptr;
    if (!start || !IsWindowEnabled(start)) return false;
    return controlText(start).find(L"Iniciar") != std::wstring::npos;
}

bool transmissionRunning() {
    HWND start = gMain ? GetDlgItem(gMain, kIdStart) : nullptr;
    if (!start) return false;
    return controlText(start).find(L"Parar transmissão") != std::wstring::npos;
}

bool privacyActive() {
    HWND status = gMain ? GetDlgItem(gMain, kIdStatus) : nullptr;
    if (!status) return false;
    const std::wstring text = controlText(status);
    return text.find(L"rivacidade") != std::wstring::npos || text.find(L"rotegido") != std::wstring::npos;
}

void refreshAudioState() {
    if (!transmissionRunning()) {
        gAudioAllowed = false;
        return;
    }
    if (privacyActive()) {
        gAudioAllowed = false;
        return;
    }
    if (!protectedMode()) {
        gAudioAllowed = true;
        return;
    }
    gAudioAllowed = gReleased.load();
}

void refreshControls() {
    if (!gIpEdit) return;
    const bool protectedSelected = protectedMode();
    ShowWindow(gIpEdit, protectedSelected ? SW_SHOW : SW_HIDE);
    ShowWindow(gIpLabel, protectedSelected ? SW_SHOW : SW_HIDE);
    ShowWindow(gIpHint, protectedSelected ? SW_SHOW : SW_HIDE);
    EnableWindow(gIpEdit, protectedSelected && readyToStart());
    if (gAudioCombo) EnableWindow(gAudioCombo, readyToStart());
    refreshAudioState();
}

void rememberConfiguredIp() {
    std::lock_guard<std::mutex> lock(gIpMutex);
    gConfiguredIp = protectedMode() ? utf8(controlText(gIpEdit)) : std::string{};
}

void rememberConfiguredAudioDevice() {
    if (!gAudioCombo) return;
    const int index = static_cast<int>(SendMessageW(gAudioCombo, CB_GETCURSEL, 0, 0));
    std::lock_guard<std::mutex> lock(gIpMutex);
    if (index >= 0 && index < static_cast<int>(gAudioDeviceIds.size()))
        gConfiguredAudioDeviceId = gAudioDeviceIds[static_cast<size_t>(index)];
    else
        gConfiguredAudioDeviceId.clear();
}

void populateAudioDevices() {
    if (!gAudioCombo) return;
    SendMessageW(gAudioCombo, CB_RESETCONTENT, 0, 0);
    gAudioDeviceIds.clear();
    SendMessageW(gAudioCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Áudio · Dispositivo padrão do Windows"));
    gAudioDeviceIds.emplace_back();

    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(init);
    if (SUCCEEDED(init) || init == RPC_E_CHANGED_MODE) {
        IMMDeviceEnumerator* enumerator = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))) && enumerator) {
            IMMDeviceCollection* collection = nullptr;
            if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)) && collection) {
                UINT count = 0;
                collection->GetCount(&count);
                for (UINT i = 0; i < count; ++i) {
                    IMMDevice* device = nullptr;
                    if (FAILED(collection->Item(i, &device)) || !device) continue;

                    LPWSTR id = nullptr;
                    IPropertyStore* store = nullptr;
                    std::wstring name;
                    if (SUCCEEDED(device->GetId(&id)) && id &&
                        SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store) {
                        PROPVARIANT value;
                        PropVariantInit(&value);
                        if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) &&
                            value.vt == VT_LPWSTR && value.pwszVal) {
                            name = value.pwszVal;
                        }
                        PropVariantClear(&value);
                    }

                    if (id) {
                        if (name.empty()) name = L"Saída de áudio";
                        const std::wstring label = L"Áudio · " + name;
                        SendMessageW(gAudioCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
                        gAudioDeviceIds.emplace_back(id);
                    }

                    if (store) store->Release();
                    if (id) CoTaskMemFree(id);
                    device->Release();
                }
                collection->Release();
            }
            enumerator->Release();
        }
    }
    if (uninitialize) CoUninitialize();

    SendMessageW(gAudioCombo, CB_SETCURSEL, 0, 0);
    rememberConfiguredAudioDevice();
}

LRESULT CALLBACK subclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                              UINT_PTR, DWORD_PTR) {
    if (msg == WM_COMMAND) {
        const int id = LOWORD(wp);
        if (id == kIdStart) {
            if (readyToStart()) {
                if (protectedMode()) {
                    const std::wstring ip = controlText(gIpEdit);
                    if (!validIpv4(ip)) {
                        MessageBoxW(hwnd,
                            L"Informe um IPv4 válido para a máquina autorizada.\n\nExemplo: 192.168.0.100",
                            L"IP autorizado", MB_OK | MB_ICONWARNING);
                        SetFocus(gIpEdit);
                        return 0;
                    }
                }
                rememberConfiguredIp();
                rememberConfiguredAudioDevice();
                gReleased = !protectedMode();
                gAudioAllowed = !protectedMode();
            } else {
                gReleased = false;
                gAudioAllowed = false;
            }
        } else if (id == kIdRelease && protectedMode()) {
            gReleased = true;
            refreshAudioState();
        } else if (id == kAudioComboId && HIWORD(wp) == CBN_SELCHANGE && readyToStart()) {
            rememberConfiguredAudioDevice();
        }
    } else if (msg == WM_TIMER || msg == WM_ENABLE || msg == WM_SHOWWINDOW) {
        refreshControls();
    } else if (msg == WM_NCDESTROY) {
        gReleased = false;
        gAudioAllowed = false;
        RemoveWindowSubclass(hwnd, subclassProc, 1);
        gMain = nullptr;
        gIpEdit = nullptr;
        gIpLabel = nullptr;
        gIpHint = nullptr;
        gAudioCombo = nullptr;
        gAudioDeviceIds.clear();
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void installUi(HWND hwnd) {
    if (gMain || !hwnd) return;
    gMain = hwnd;

    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HWND cover = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
        568, 202, 376, 58, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(cover, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    gIpLabel = CreateWindowExW(0, L"STATIC", L"IP autorizado", WS_CHILD | WS_VISIBLE,
        572, 207, 112, 22, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIpLabelId)),
        GetModuleHandleW(nullptr), nullptr);
    gIpEdit = CreateWindowExW(0, L"EDIT", L"192.168.0.100",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        686, 202, 254, 30, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIpEditId)),
        GetModuleHandleW(nullptr), nullptr);
    gIpHint = CreateWindowExW(0, L"STATIC", L"Somente esta máquina poderá receber no Modo protegido.",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        572, 236, 368, 20, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIpHintId)),
        GetModuleHandleW(nullptr), nullptr);

    gAudioCombo = CreateWindowExW(0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        32, 618, 480, 220, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAudioComboId)),
        GetModuleHandleW(nullptr), nullptr);

    SendMessageW(gIpLabel, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(gIpEdit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(gIpHint, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(gAudioCombo, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SetWindowTheme(gIpEdit, L"DarkMode_Explorer", nullptr);
    SetWindowTheme(gAudioCombo, L"DarkMode_Explorer", nullptr);
    populateAudioDevices();
    SetWindowSubclass(hwnd, subclassProc, 1, 0);
    refreshControls();
    InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT CALLBACK callWndHook(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && !gMain) {
        const auto* data = reinterpret_cast<CWPSTRUCT*>(lp);
        if (data && data->hwnd && data->message == WM_PAINT) {
            wchar_t className[128]{};
            GetClassNameW(data->hwnd, className, static_cast<int>(std::size(className)));
            if (wcscmp(className, L"TransmissorNDIPortatilV3") == 0) installUi(data->hwnd);
        }
    }
    return CallNextHookEx(gHook, code, wp, lp);
}

struct UiBootstrap {
    UiBootstrap() {
        gHook = SetWindowsHookExW(WH_CALLWNDPROC, callWndHook, nullptr, GetCurrentThreadId());
    }
    ~UiBootstrap() {
        if (gHook) UnhookWindowsHookEx(gHook);
    }
} gBootstrap;
}

std::string configuredReceiverIp() {
    std::lock_guard<std::mutex> lock(gIpMutex);
    return gConfiguredIp;
}

std::wstring configuredAudioDeviceId() {
    std::lock_guard<std::mutex> lock(gIpMutex);
    return gConfiguredAudioDeviceId;
}

bool audioTransmissionAllowed() {
    return gAudioAllowed.load();
}
