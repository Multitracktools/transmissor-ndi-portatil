#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <wlanapi.h>
#include <iphlpapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Processing.NDI.Lib.h"

namespace {
using namespace std::chrono_literals;

constexpr wchar_t kWindowClass[] = L"TransmissorNDIPortatilWindowV2";
constexpr UINT kStatusMessage = WM_APP + 1;
constexpr UINT kReceiverMessage = WM_APP + 2;
constexpr UINT kTrayMessage = WM_APP + 3;
constexpr UINT kToastMessage = WM_APP + 4;
constexpr UINT_PTR kToastTimer = 42;
constexpr int kTrayId = 1;

enum ControlId {
    IdSourceName = 1001, IdSourceType, IdCaptureSource, IdRefreshSources,
    IdResolution, IdFps, IdCursor,
    IdProtectedMode, IdQuickMode,
    IdAllowWhatsApp, IdAllowTelegram,
    IdStart, IdStop, IdRelease, IdHide,
    IdReceivers, IdStatus, IdQuality, IdToast, IdHelp,
    IdHelpDontShow = 1200,
    IdTrayOpen = 2001, IdTrayHide, IdTrayStop, IdTrayExit
};

enum class CaptureKind { Monitor, Window };
enum class TransmissionMode { Protected, Quick };
enum class BlockReason { None, WaitingAuthorization, ExtraConnection, Privacy, Manual };

struct MonitorInfo { HMONITOR handle{}; RECT bounds{}; std::wstring label; };
struct WindowInfo { HWND handle{}; RECT bounds{}; std::wstring title; std::wstring processName; };
struct CaptureTarget { CaptureKind kind{CaptureKind::Monitor}; RECT bounds{}; HWND window{}; HMONITOR monitor{}; std::wstring label; };
struct Settings {
    std::wstring sourceName{L"Zosma NDI"};
    CaptureKind captureKind{CaptureKind::Monitor};
    int sourceIndex{0};
    int resolutionIndex{0};
    int fps{30};
    bool cursor{true};
    TransmissionMode mode{TransmissionMode::Protected};
    bool showHelpOnStart{true};
};

struct AppState {
    HWND window{}, sourceName{}, sourceType{}, source{}, refresh{}, resolution{}, fps{}, cursor{};
    HWND protectedMode{}, quickMode{}, allowWhatsApp{}, allowTelegram{};
    HWND start{}, stop{}, release{}, hide{}, receivers{}, status{}, quality{}, toast{}, help{};
    std::vector<MonitorInfo> monitors;
    std::vector<WindowInfo> windows;
    std::thread worker;
    std::atomic_bool stopRequested{false}, running{false}, authorized{false}, manualHidden{false};
    std::atomic_bool allowWa{false}, allowTg{false};
    std::atomic_int receiverCount{0};
    std::atomic<BlockReason> blockReason{BlockReason::None};
    TransmissionMode activeMode{TransmissionMode::Protected};
    Settings settings;
    NOTIFYICONDATAW tray{};
    std::mutex workerMutex;
};
AppState g_app;

std::filesystem::path executableDirectory() {
    std::vector<wchar_t> path(32768);
    DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return std::filesystem::path(std::wstring(path.data(), size)).parent_path();
}
std::filesystem::path settingsPath() { return executableDirectory() / L"transmissor-ndi.ini"; }
void logLine(const std::wstring& message) {
    std::wofstream log(executableDirectory() / L"transmissor-ndi.log", std::ios::app);
    if (!log) return;
    SYSTEMTIME t{}; GetLocalTime(&t);
    log << L'[' << t.wYear << L'-' << t.wMonth << L'-' << t.wDay << L' ' << t.wHour << L':' << t.wMinute << L':' << t.wSecond << L"] " << message << L'\n';
}
std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::wstring lower(std::wstring s) { std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c){ return std::towlower(c); }); return s; }
std::wstring getText(HWND h) {
    int n = GetWindowTextLengthW(h); std::wstring s(static_cast<size_t>(n)+1, L'\0');
    GetWindowTextW(h, s.data(), n+1); s.resize(static_cast<size_t>(n)); return s;
}
void setText(HWND h, const std::wstring& s) { SetWindowTextW(h, s.c_str()); }
void postStatus(const std::wstring& s) {
    auto* p = new std::wstring(s); if (!PostMessageW(g_app.window, kStatusMessage, 0, reinterpret_cast<LPARAM>(p))) delete p; logLine(s);
}
void postToast(const std::wstring& s) {
    auto* p = new std::wstring(s); if (!PostMessageW(g_app.window, kToastMessage, 0, reinterpret_cast<LPARAM>(p))) delete p;
}

int readInt(const wchar_t* key, int def) { return GetPrivateProfileIntW(L"app", key, def, settingsPath().c_str()); }
std::wstring readString(const wchar_t* key, const wchar_t* def) {
    wchar_t buf[512]{}; GetPrivateProfileStringW(L"app", key, def, buf, 512, settingsPath().c_str()); return buf;
}
void saveSettings() {
    const auto path = settingsPath();
    auto write = [&](const wchar_t* k, const std::wstring& v){ WritePrivateProfileStringW(L"app", k, v.c_str(), path.c_str()); };
    write(L"sourceName", getText(g_app.sourceName));
    write(L"captureKind", std::to_wstring(SendMessageW(g_app.sourceType, CB_GETCURSEL, 0, 0)));
    write(L"sourceIndex", std::to_wstring(SendMessageW(g_app.source, CB_GETCURSEL, 0, 0)));
    write(L"resolutionIndex", std::to_wstring(SendMessageW(g_app.resolution, CB_GETCURSEL, 0, 0)));
    write(L"fps", std::to_wstring(SendMessageW(g_app.fps, CB_GETCURSEL, 0, 0) == 0 ? 30 : 60));
    write(L"cursor", std::to_wstring(Button_GetCheck(g_app.cursor) == BST_CHECKED));
    write(L"mode", std::to_wstring(Button_GetCheck(g_app.quickMode) == BST_CHECKED ? 1 : 0));
    write(L"showHelp", std::to_wstring(g_app.settings.showHelpOnStart));
}
void loadSettings() {
    g_app.settings.sourceName = readString(L"sourceName", L"Zosma NDI");
    g_app.settings.captureKind = readInt(L"captureKind", 0) == 1 ? CaptureKind::Window : CaptureKind::Monitor;
    g_app.settings.sourceIndex = readInt(L"sourceIndex", 0);
    g_app.settings.resolutionIndex = readInt(L"resolutionIndex", 0);
    g_app.settings.fps = readInt(L"fps", 30) == 60 ? 60 : 30;
    g_app.settings.cursor = readInt(L"cursor", 1) != 0;
    g_app.settings.mode = readInt(L"mode", 0) == 1 ? TransmissionMode::Quick : TransmissionMode::Protected;
    g_app.settings.showHelpOnStart = readInt(L"showHelp", 1) != 0;
}

std::wstring processNameForWindow(HWND hwnd) {
    DWORD pid{}; GetWindowThreadProcessId(hwnd, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid); if (!p) return {};
    wchar_t path[32768]{}; DWORD n = 32768; std::wstring result;
    if (QueryFullProcessImageNameW(p, 0, path, &n)) result = std::filesystem::path(std::wstring(path, n)).filename().wstring();
    CloseHandle(p); return lower(result);
}
BOOL CALLBACK enumMonitorProc(HMONITOR monitor, HDC, LPRECT r, LPARAM data) {
    auto* list = reinterpret_cast<std::vector<MonitorInfo>*>(data); MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi); GetMonitorInfoW(monitor, &mi);
    int number = static_cast<int>(list->size()) + 1; int w = r->right-r->left, h = r->bottom-r->top;
    std::wstring label = L"Monitor " + std::to_wstring(number) + L" — " + std::to_wstring(w) + L" × " + std::to_wstring(h);
    if (mi.dwFlags & MONITORINFOF_PRIMARY) label += L" (principal)";
    list->push_back({monitor,*r,label}); return TRUE;
}
BOOL CALLBACK enumWindowProc(HWND hwnd, LPARAM data) {
    if (hwnd == g_app.window || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    BOOL cloaked = FALSE; DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)); if (cloaked) return TRUE;
    int n = GetWindowTextLengthW(hwnd); if (n <= 0) return TRUE;
    std::wstring title(static_cast<size_t>(n)+1,L'\0'); GetWindowTextW(hwnd,title.data(),n+1); title.resize(static_cast<size_t>(n));
    RECT r{}; if (!GetWindowRect(hwnd,&r) || r.right<=r.left || r.bottom<=r.top) return TRUE;
    reinterpret_cast<std::vector<WindowInfo>*>(data)->push_back({hwnd,r,title,processNameForWindow(hwnd)}); return TRUE;
}
void enumerateSources() {
    if (SendMessageW(g_app.sourceType, CB_GETCURSEL,0,0)==1) { g_app.windows.clear(); EnumWindows(enumWindowProc,reinterpret_cast<LPARAM>(&g_app.windows)); }
    else { g_app.monitors.clear(); EnumDisplayMonitors(nullptr,nullptr,enumMonitorProc,reinterpret_cast<LPARAM>(&g_app.monitors)); }
}
void loadSourceCombo(int preferred=-1) {
    SendMessageW(g_app.source, CB_RESETCONTENT,0,0); enumerateSources();
    if (SendMessageW(g_app.sourceType, CB_GETCURSEL,0,0)==1) for (auto& x:g_app.windows) SendMessageW(g_app.source,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(x.title.c_str()));
    else for (auto& x:g_app.monitors) SendMessageW(g_app.source,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(x.label.c_str()));
    LRESULT count=SendMessageW(g_app.source,CB_GETCOUNT,0,0); if(count>0) SendMessageW(g_app.source,CB_SETCURSEL,(preferred>=0&&preferred<count)?preferred:0,0);
}
CaptureTarget selectedTarget() {
    CaptureTarget t; int index=static_cast<int>(SendMessageW(g_app.source,CB_GETCURSEL,0,0));
    if (SendMessageW(g_app.sourceType,CB_GETCURSEL,0,0)==1 && index>=0 && index<static_cast<int>(g_app.windows.size())) { auto& w=g_app.windows[index]; t.kind=CaptureKind::Window; t.bounds=w.bounds; t.window=w.handle; t.label=w.title; }
    else if (index>=0 && index<static_cast<int>(g_app.monitors.size())) { auto&m=g_app.monitors[index]; t.kind=CaptureKind::Monitor; t.bounds=m.bounds; t.monitor=m.handle; t.label=m.label; }
    return t;
}

bool isBrowserProcess(const std::wstring& p) { return p==L"chrome.exe"||p==L"msedge.exe"||p==L"firefox.exe"||p==L"opera.exe"||p==L"opera_gx.exe"||p==L"brave.exe"||p==L"vivaldi.exe"; }
enum class ProtectedApp { None, WhatsApp, Telegram };
ProtectedApp classifyProtected(HWND hwnd) {
    std::wstring p=processNameForWindow(hwnd), title=lower([&]{int n=GetWindowTextLengthW(hwnd); std::wstring s(static_cast<size_t>(n)+1,L'\0'); GetWindowTextW(hwnd,s.data(),n+1); s.resize(static_cast<size_t>(n)); return s;}());
    if (p.find(L"whatsapp")!=std::wstring::npos) return ProtectedApp::WhatsApp;
    if (p==L"telegram.exe" || p==L"telegramdesktop.exe") return ProtectedApp::Telegram;
    if (isBrowserProcess(p)) {
        if (title.find(L"whatsapp")!=std::wstring::npos) return ProtectedApp::WhatsApp;
        if (title.find(L"telegram")!=std::wstring::npos) return ProtectedApp::Telegram;
    }
    return ProtectedApp::None;
}
bool windowActuallyVisible(HWND hwnd, const RECT& captureBounds) {
    if (!IsWindowVisible(hwnd)||IsIconic(hwnd)) return false; BOOL cloaked=FALSE; DwmGetWindowAttribute(hwnd,DWMWA_CLOAKED,&cloaked,sizeof(cloaked)); if(cloaked) return false;
    RECT wr{}, inter{}; if(!GetWindowRect(hwnd,&wr)||!IntersectRect(&inter,&wr,&captureBounds)) return false;
    HRGN visible=CreateRectRgn(inter.left,inter.top,inter.right,inter.bottom);
    for(HWND z=GetWindow(hwnd,GW_HWNDPREV); z; z=GetWindow(z,GW_HWNDPREV)) {
        if(z==g_app.window||!IsWindowVisible(z)||IsIconic(z)) continue; RECT zr{},zi{}; if(GetWindowRect(z,&zr)&&IntersectRect(&zi,&zr,&inter)) { HRGN cover=CreateRectRgn(zi.left,zi.top,zi.right,zi.bottom); CombineRgn(visible,visible,cover,RGN_DIFF); DeleteObject(cover); if(GetRgnBox(visible,&zi)==NULLREGION){DeleteObject(visible);return false;} }
    }
    DeleteObject(visible); return true;
}
int queryWifiQuality() {
    HANDLE client{}; DWORD version{};
    if (WlanOpenHandle(2, nullptr, &version, &client) != ERROR_SUCCESS) return -1;
    PWLAN_INTERFACE_INFO_LIST list{}; int quality = -1;
    if (WlanEnumInterfaces(client, nullptr, &list) == ERROR_SUCCESS && list) {
        for (DWORD i=0; i<list->dwNumberOfItems; ++i) {
            if (list->InterfaceInfo[i].isState != wlan_interface_state_connected) continue;
            DWORD size{}; WLAN_OPCODE_VALUE_TYPE type{}; PWLAN_CONNECTION_ATTRIBUTES attr{};
            if (WlanQueryInterface(client, &list->InterfaceInfo[i].InterfaceGuid, wlan_intf_opcode_current_connection,
                                   nullptr, &size, reinterpret_cast<PVOID*>(&attr), &type) == ERROR_SUCCESS && attr) {
                quality = static_cast<int>(attr->wlanAssociationAttributes.wlanSignalQuality);
                WlanFreeMemory(attr); break;
            }
        }
        WlanFreeMemory(list);
    }
    WlanCloseHandle(client, nullptr); return quality;
}
uint64_t queryOutboundBytes() {
    PMIB_IF_TABLE2 table{}; uint64_t total = 0;
    if (GetIfTable2(&table) == NO_ERROR && table) {
        for (ULONG i=0; i<table->NumEntries; ++i) {
            const auto& row = table->Table[i];
            if (row.OperStatus == IfOperStatusUp && row.Type != IF_TYPE_SOFTWARE_LOOPBACK)
                total += row.OutOctets;
        }
        FreeMibTable(table);
    }
    return total;
}
bool privacyShouldBlock(const CaptureTarget& target) {
    auto permitted=[](ProtectedApp p){ return p==ProtectedApp::WhatsApp ? g_app.allowWa.load() : p==ProtectedApp::Telegram ? g_app.allowTg.load() : true; };
    if(target.kind==CaptureKind::Window) { ProtectedApp p=classifyProtected(target.window); return p!=ProtectedApp::None && !permitted(p); }
    std::vector<WindowInfo> list; EnumWindows(enumWindowProc,reinterpret_cast<LPARAM>(&list));
    for(auto& w:list){ ProtectedApp p=classifyProtected(w.handle); if(p!=ProtectedApp::None && !permitted(p) && windowActuallyVisible(w.handle,target.bounds)) return true; }
    return false;
}

void drawCursorInto(HDC dc,const RECT& bounds) {
    CURSORINFO c{}; c.cbSize=sizeof(c); if(!GetCursorInfo(&c)||c.flags!=CURSOR_SHOWING||!PtInRect(&bounds,c.ptScreenPos)) return;
    ICONINFO ii{}; int x=c.ptScreenPos.x-bounds.left,y=c.ptScreenPos.y-bounds.top; if(GetIconInfo(c.hCursor,&ii)){x-=ii.xHotspot;y-=ii.yHotspot;if(ii.hbmMask)DeleteObject(ii.hbmMask);if(ii.hbmColor)DeleteObject(ii.hbmColor);} DrawIconEx(dc,x,y,c.hCursor,0,0,0,nullptr,DI_NORMAL);
}
void drawCenteredText(HDC dc,const RECT& rc,const std::wstring& title,const std::wstring& body,const std::wstring& source) {
    HBRUSH bg=CreateSolidBrush(RGB(18,20,24)); FillRect(dc,&rc,bg); DeleteObject(bg); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(245,246,248));
    LOGFONTW lf{}; lf.lfHeight=-38; lf.lfWeight=FW_SEMIBOLD; wcscpy_s(lf.lfFaceName,L"Segoe UI"); HFONT f1=CreateFontIndirectW(&lf); HFONT old=(HFONT)SelectObject(dc,f1);
    RECT t=rc; t.top=rc.top+(rc.bottom-rc.top)/2-80; DrawTextW(dc,title.c_str(),-1,&t,DT_CENTER|DT_SINGLELINE|DT_VCENTER);
    lf.lfHeight=-22;lf.lfWeight=FW_NORMAL; HFONT f2=CreateFontIndirectW(&lf); SelectObject(dc,f2); SetTextColor(dc,RGB(190,196,205)); t.top+=65; DrawTextW(dc,body.c_str(),-1,&t,DT_CENTER|DT_SINGLELINE|DT_VCENTER);
    lf.lfHeight=-16; HFONT f3=CreateFontIndirectW(&lf); SelectObject(dc,f3); SetTextColor(dc,RGB(130,137,147)); t=rc; t.top=rc.bottom-55; DrawTextW(dc,source.c_str(),-1,&t,DT_CENTER|DT_SINGLELINE|DT_VCENTER);
    SelectObject(dc,old); DeleteObject(f1);DeleteObject(f2);DeleteObject(f3);
}

using NdiLoadFunction=const NDIlib_v6* (*)();
struct CaptureSurface {
    HDC screen{}, mem{}; HBITMAP bitmap{}; HGDIOBJ original{}; void* pixels{}; int width{},height{};
    bool resize(int w,int h){ if(w<=0||h<=0)return false; if(bitmap&&w==width&&h==height)return true; if(bitmap){SelectObject(mem,original);DeleteObject(bitmap);bitmap=nullptr;pixels=nullptr;} BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=w;bi.bmiHeader.biHeight=-h;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;bitmap=CreateDIBSection(screen,&bi,DIB_RGB_COLORS,&pixels,nullptr,0);if(!bitmap||!pixels)return false;HGDIOBJ replaced=SelectObject(mem,bitmap);if(!original)original=replaced;width=w;height=h;return true; }
    CaptureSurface(){screen=GetDC(nullptr);mem=screen?CreateCompatibleDC(screen):nullptr;} ~CaptureSurface(){if(bitmap){SelectObject(mem,original);DeleteObject(bitmap);}if(mem)DeleteDC(mem);if(screen)ReleaseDC(nullptr,screen);}
};
void updateUiForReceiverCount(int count) { PostMessageW(g_app.window,kReceiverMessage,static_cast<WPARAM>(count),0); }
void transmitWorker(CaptureTarget target,std::string sourceName,bool showCursor,int fps,int resolutionIndex,TransmissionMode mode) {
    auto dllPath=executableDirectory()/L"Processing.NDI.Lib.x64.dll"; HMODULE module=LoadLibraryW(dllPath.c_str()); if(!module){postStatus(L"Erro: Processing.NDI.Lib.x64.dll não encontrada.");g_app.running=false;return;}
    auto load=reinterpret_cast<NdiLoadFunction>(GetProcAddress(module,"NDIlib_v6_load")); const NDIlib_v6* ndi=load?load():nullptr; if(!ndi||!ndi->initialize()){postStatus(L"Erro ao inicializar o runtime NDI.");FreeLibrary(module);g_app.running=false;return;}
    NDIlib_send_create_t create{};create.p_ndi_name=sourceName.c_str();create.clock_video=true;create.clock_audio=false;auto sender=ndi->send_create(&create);if(!sender){postStatus(L"Erro ao criar a fonte NDI.");ndi->destroy();FreeLibrary(module);g_app.running=false;return;}
    g_app.authorized=(mode==TransmissionMode::Quick);g_app.manualHidden=false;g_app.blockReason=(mode==TransmissionMode::Quick?BlockReason::None:BlockReason::WaitingAuthorization);
    CaptureSurface surface; int baseW=target.bounds.right-target.bounds.left,baseH=target.bounds.bottom-target.bounds.top; int outW=baseW,outH=baseH;
    if(resolutionIndex==1){outW=1920;outH=1080;} else if(resolutionIndex==2){outW=1280;outH=720;} if(!surface.resize(outW,outH)){postStatus(L"Erro ao preparar a superfície de captura.");}
    HDC captureDc=CreateCompatibleDC(surface.screen); HBITMAP captureBmp=nullptr;HGDIOBJ capOld=nullptr;void* capPixels=nullptr;int capW=0,capH=0;
    auto resizeCapture=[&](int w,int h){if(captureBmp&&w==capW&&h==capH)return true;if(captureBmp){SelectObject(captureDc,capOld);DeleteObject(captureBmp);}BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=w;bi.bmiHeader.biHeight=-h;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;captureBmp=CreateDIBSection(surface.screen,&bi,DIB_RGB_COLORS,&capPixels,nullptr,0);if(!captureBmp)return false;HGDIOBJ x=SelectObject(captureDc,captureBmp);if(!capOld)capOld=x;capW=w;capH=h;return true;};
    NDIlib_video_frame_v2_t frame{};frame.xres=outW;frame.yres=outH;frame.FourCC=NDIlib_FourCC_video_type_BGRX;frame.frame_rate_N=fps*1000;frame.frame_rate_D=1000;frame.picture_aspect_ratio=static_cast<float>(outW)/outH;frame.frame_format_type=NDIlib_frame_format_type_progressive;frame.timecode=NDIlib_send_timecode_synthesize;frame.p_data=static_cast<uint8_t*>(surface.pixels);frame.line_stride_in_bytes=outW*4;
    postStatus(mode==TransmissionMode::Protected?L"Fonte NDI ativa — aguardando receptor.":L"Transmissão rápida iniciada.");
    int lastConnections=-1; auto nextFrame=std::chrono::steady_clock::now(); double measuredFps=0;int frames=0;auto fpsStart=nextFrame; uint64_t lastBytes=queryOutboundBytes(); auto netStart=nextFrame;
    while(!g_app.stopRequested.load()) {
        int connections=ndi->send_get_no_connections(sender,0); if(connections!=lastConnections){lastConnections=connections;g_app.receiverCount=connections;updateUiForReceiverCount(connections);if(mode==TransmissionMode::Protected){if(connections>1){g_app.authorized=false;g_app.blockReason=BlockReason::ExtraConnection;postStatus(L"Transmissão bloqueada — conexão adicional detectada.");}else if(connections==1&&!g_app.authorized.load()&&g_app.blockReason.load()==BlockReason::WaitingAuthorization){postStatus(L"Receptor conectado — libere a transmissão quando estiver pronto.");}}else if(connections>1)postToast(L"Aviso: há mais de um receptor conectado.");}
        BlockReason reason=BlockReason::None;
        bool privacy=privacyShouldBlock(target);
        if(privacy) reason=BlockReason::Privacy; else if(g_app.manualHidden.load()) reason=BlockReason::Manual; else if(mode==TransmissionMode::Protected&&!g_app.authorized.load()) reason=g_app.blockReason.load()==BlockReason::ExtraConnection?BlockReason::ExtraConnection:BlockReason::WaitingAuthorization;
        if(reason==BlockReason::None){
            RECT r=target.bounds;if(target.kind==CaptureKind::Window){if(!IsWindow(target.window)){postStatus(L"A janela selecionada foi fechada. Transmissão encerrada por segurança.");break;}if(IsIconic(target.window)){reason=BlockReason::Manual;}else GetWindowRect(target.window,&r);} int w=r.right-r.left,h=r.bottom-r.top;
            if(reason==BlockReason::None&&resizeCapture(w,h)){
                bool ok=false;if(target.kind==CaptureKind::Monitor)ok=BitBlt(captureDc,0,0,w,h,surface.screen,r.left,r.top,SRCCOPY|CAPTUREBLT)!=0;else {ok=PrintWindow(target.window,captureDc,PW_RENDERFULLCONTENT)!=0;if(!ok)ok=BitBlt(captureDc,0,0,w,h,surface.screen,r.left,r.top,SRCCOPY|CAPTUREBLT)!=0;}
                if(ok){if(showCursor&&target.kind==CaptureKind::Monitor)drawCursorInto(captureDc,r);SetStretchBltMode(surface.mem,HALFTONE);StretchBlt(surface.mem,0,0,outW,outH,captureDc,0,0,w,h,SRCCOPY);} else reason=BlockReason::Manual;
            }
        }
        if(reason!=BlockReason::None){RECT rc{0,0,outW,outH};std::wstring src=std::wstring(sourceName.begin(),sourceName.end());if(reason==BlockReason::Privacy)drawCenteredText(surface.mem,rc,L"Conteúdo protegido",L"Um aplicativo privado está aberto no monitor compartilhado.",src);else if(reason==BlockReason::ExtraConnection)drawCenteredText(surface.mem,rc,L"Transmissão temporariamente bloqueada",L"Foi detectada uma conexão adicional.",src);else if(reason==BlockReason::Manual)drawCenteredText(surface.mem,rc,L"Imagem ocultada",L"A transmissão foi ocultada no computador transmissor.",src);else drawCenteredText(surface.mem,rc,L"Transmissão protegida",L"Aguardando autorização no computador transmissor.",src);}
        ndi->send_send_video_v2(sender,&frame);frames++;auto now=std::chrono::steady_clock::now();if(now-fpsStart>=2s){measuredFps=frames/std::chrono::duration<double>(now-fpsStart).count();frames=0;fpsStart=now; uint64_t bytes=queryOutboundBytes(); double secs=std::chrono::duration<double>(now-netStart).count(); double mbps=secs>0?((bytes>=lastBytes?bytes-lastBytes:0)*8.0/1000000.0/secs):0.0; lastBytes=bytes; netStart=now; int wifi=queryWifiQuality(); std::wstring state=measuredFps<fps*0.75?L"Instável":measuredFps<fps*0.92?L"Atenção":L"Estável"; std::wstring wifiText=wifi>=0?std::to_wstring(wifi)+L"%":L"—"; postStatus(L"__QUALITY__Wi-Fi "+wifiText+L" · "+std::to_wstring(static_cast<int>(mbps+0.5))+L" Mbps · "+std::to_wstring(static_cast<int>(measuredFps+0.5))+L" fps · "+state);}
        nextFrame+=std::chrono::microseconds(1000000/fps);std::this_thread::sleep_until(nextFrame);if(std::chrono::steady_clock::now()-nextFrame>1s)nextFrame=std::chrono::steady_clock::now();
    }
    if(captureBmp){SelectObject(captureDc,capOld);DeleteObject(captureBmp);}if(captureDc)DeleteDC(captureDc);ndi->send_destroy(sender);ndi->destroy();FreeLibrary(module);g_app.running=false;g_app.receiverCount=0;g_app.authorized=false;g_app.blockReason=BlockReason::None;updateUiForReceiverCount(0);postStatus(L"Transmissão encerrada.");
}

void stopTransmission() { if(!g_app.running.load())return;g_app.stopRequested=true;std::lock_guard<std::mutex> lock(g_app.workerMutex);if(g_app.worker.joinable())g_app.worker.join();g_app.stopRequested=false; }
void startTransmission() {
    if(g_app.running.load())return;std::wstring name=getText(g_app.sourceName);if(name.empty()){MessageBoxW(g_app.window,L"Defina um nome para a fonte NDI.",L"Transmissor NDI",MB_OK|MB_ICONWARNING);return;}
    CaptureTarget target=selectedTarget(); if(target.bounds.right<=target.bounds.left){MessageBoxW(g_app.window,L"Selecione uma fonte de captura válida.",L"Transmissor NDI",MB_OK|MB_ICONWARNING);return;}
    int fps=SendMessageW(g_app.fps,CB_GETCURSEL,0,0)==1?60:30;int res=static_cast<int>(SendMessageW(g_app.resolution,CB_GETCURSEL,0,0));bool cursor=Button_GetCheck(g_app.cursor)==BST_CHECKED;TransmissionMode mode=Button_GetCheck(g_app.quickMode)==BST_CHECKED?TransmissionMode::Quick:TransmissionMode::Protected;g_app.activeMode=mode;
    g_app.allowWa=false;g_app.allowTg=false;Button_SetCheck(g_app.allowWhatsApp,BST_UNCHECKED);Button_SetCheck(g_app.allowTelegram,BST_UNCHECKED);saveSettings();g_app.running=true;g_app.stopRequested=false;
    std::lock_guard<std::mutex> lock(g_app.workerMutex);if(g_app.worker.joinable())g_app.worker.join();g_app.worker=std::thread(transmitWorker,target,utf8(name),cursor,fps,res,mode);
}

void showHelpDialog() {
    const wchar_t* text=L"COMO USAR\n\n1. Escolha Monitor ou Janela e selecione a origem.\n2. No Modo protegido, a fonte NDI aparece com uma tela de espera.\n3. Quando um receptor conectar, clique em Liberar transmissão.\n4. Se uma conexão adicional entrar, a imagem é bloqueada e exige nova liberação.\n5. No Modo rápido, a imagem começa imediatamente e conexões extras apenas geram aviso.\n6. WhatsApp e Telegram começam protegidos em toda execução. As permissões são temporárias.\n7. Minimizar este aplicativo mantém a transmissão. O menu próximo ao relógio permite ocultar a imagem ou parar.\n\nA fonte NDI nunca é encerrada quando a imagem é bloqueada.";
    int result=MessageBoxW(g_app.window,text,L"Como usar — Transmissor NDI Portátil",MB_OK|MB_ICONINFORMATION);(void)result;
    int dont=MessageBoxW(g_app.window,L"Deseja abrir automaticamente o Como usar nas próximas vezes?",L"Como usar",MB_YESNO|MB_ICONQUESTION);g_app.settings.showHelpOnStart=(dont==IDYES);saveSettings();
}
void addTrayIcon() { g_app.tray={};g_app.tray.cbSize=sizeof(g_app.tray);g_app.tray.hWnd=g_app.window;g_app.tray.uID=kTrayId;g_app.tray.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;g_app.tray.uCallbackMessage=kTrayMessage;g_app.tray.hIcon=LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(1));wcscpy_s(g_app.tray.szTip,L"Transmissor NDI Portátil");Shell_NotifyIconW(NIM_ADD,&g_app.tray); }
void removeTrayIcon(){if(g_app.tray.cbSize)Shell_NotifyIconW(NIM_DELETE,&g_app.tray);}
void showTrayMenu(){POINT pt{};GetCursorPos(&pt);HMENU m=CreatePopupMenu();AppendMenuW(m,MF_STRING,IdTrayOpen,L"Abrir transmissor");AppendMenuW(m,MF_STRING,IdTrayHide,g_app.manualHidden?L"Mostrar imagem":L"Ocultar imagem");AppendMenuW(m,MF_STRING,IdTrayStop,L"Parar transmissão");AppendMenuW(m,MF_SEPARATOR,0,nullptr);AppendMenuW(m,MF_STRING,IdTrayExit,L"Sair");SetForegroundWindow(g_app.window);TrackPopupMenu(m,TPM_RIGHTBUTTON,pt.x,pt.y,0,g_app.window,nullptr);DestroyMenu(m);}
void updateControls(){bool run=g_app.running.load();EnableWindow(g_app.start,!run);EnableWindow(g_app.stop,run);EnableWindow(g_app.sourceName,!run);EnableWindow(g_app.sourceType,!run);EnableWindow(g_app.source,!run);EnableWindow(g_app.refresh,!run);EnableWindow(g_app.resolution,!run);EnableWindow(g_app.fps,!run);EnableWindow(g_app.protectedMode,!run);EnableWindow(g_app.quickMode,!run);EnableWindow(g_app.release,run&&g_app.activeMode==TransmissionMode::Protected&&g_app.receiverCount.load()==1&&!g_app.authorized.load()&&!privacyShouldBlock(selectedTarget()));EnableWindow(g_app.hide,run);SetWindowTextW(g_app.hide,g_app.manualHidden?L"Mostrar imagem":L"Ocultar imagem");}

HWND makeControl(const wchar_t* cls,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id){return CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,g_app.window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);}
void createUi() {
    HFONT font=(HFONT)GetStockObject(DEFAULT_GUI_FONT);
    makeControl(L"STATIC",L"Nome da fonte NDI",0,24,20,160,22,0);g_app.sourceName=makeControl(L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL,24,44,330,28,IdSourceName);
    g_app.help=makeControl(L"BUTTON",L"Como usar",BS_PUSHBUTTON,570,42,150,30,IdHelp);
    makeControl(L"STATIC",L"Captura",0,24,88,100,22,0);g_app.sourceType=makeControl(WC_COMBOBOXW,L"",CBS_DROPDOWNLIST|WS_VSCROLL,24,112,160,200,IdSourceType);SendMessageW(g_app.sourceType,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Monitor"));SendMessageW(g_app.sourceType,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Janela"));
    g_app.source=makeControl(WC_COMBOBOXW,L"",CBS_DROPDOWNLIST|WS_VSCROLL,194,112,400,260,IdCaptureSource);g_app.refresh=makeControl(L"BUTTON",L"Atualizar",BS_PUSHBUTTON,604,111,116,30,IdRefreshSources);
    makeControl(L"STATIC",L"Qualidade",0,24,160,100,22,0);g_app.resolution=makeControl(WC_COMBOBOXW,L"",CBS_DROPDOWNLIST,24,184,190,150,IdResolution);for(auto*s:{L"Original",L"1920 × 1080",L"1280 × 720"})SendMessageW(g_app.resolution,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(s));g_app.fps=makeControl(WC_COMBOBOXW,L"",CBS_DROPDOWNLIST,224,184,120,120,IdFps);SendMessageW(g_app.fps,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"30 FPS"));SendMessageW(g_app.fps,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"60 FPS"));g_app.cursor=makeControl(L"BUTTON",L"Mostrar cursor",BS_AUTOCHECKBOX,365,184,160,26,IdCursor);
    makeControl(L"STATIC",L"Modo de transmissão",0,24,232,180,22,0);g_app.protectedMode=makeControl(L"BUTTON",L"Modo protegido",BS_AUTORADIOBUTTON|WS_GROUP,24,258,170,26,IdProtectedMode);g_app.quickMode=makeControl(L"BUTTON",L"Modo rápido",BS_AUTORADIOBUTTON,205,258,150,26,IdQuickMode);
    makeControl(L"STATIC",L"Privacidade — permissões válidas somente nesta execução",0,24,306,420,22,0);g_app.allowWhatsApp=makeControl(L"BUTTON",L"Permitir WhatsApp",BS_AUTOCHECKBOX,24,332,180,26,IdAllowWhatsApp);g_app.allowTelegram=makeControl(L"BUTTON",L"Permitir Telegram",BS_AUTOCHECKBOX,220,332,180,26,IdAllowTelegram);
    g_app.receivers=makeControl(L"STATIC",L"Receptores: 0",0,24,382,200,24,IdReceivers);g_app.status=makeControl(L"STATIC",L"Pronto para transmitir.",0,24,410,696,24,IdStatus);g_app.quality=makeControl(L"STATIC",L"Rede · — Mbps · — fps · Parado",0,24,438,696,24,IdQuality);
    g_app.start=makeControl(L"BUTTON",L"Iniciar transmissão",BS_DEFPUSHBUTTON,24,486,200,40,IdStart);g_app.release=makeControl(L"BUTTON",L"Liberar transmissão",BS_PUSHBUTTON,236,486,180,40,IdRelease);g_app.hide=makeControl(L"BUTTON",L"Ocultar imagem",BS_PUSHBUTTON,428,486,140,40,IdHide);g_app.stop=makeControl(L"BUTTON",L"Parar",BS_PUSHBUTTON,580,486,140,40,IdStop);g_app.toast=makeControl(L"STATIC",L"",0,24,542,696,28,IdToast);
    EnumChildWindows(g_app.window,[](HWND h,LPARAM f){SendMessageW(h,WM_SETFONT,f,TRUE);return TRUE;},reinterpret_cast<LPARAM>(font));
}

LRESULT CALLBACK wndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg){
    case WM_CREATE:{g_app.window=hwnd;loadSettings();createUi();setText(g_app.sourceName,g_app.settings.sourceName);SendMessageW(g_app.sourceType,CB_SETCURSEL,g_app.settings.captureKind==CaptureKind::Window?1:0,0);loadSourceCombo(g_app.settings.sourceIndex);SendMessageW(g_app.resolution,CB_SETCURSEL,std::clamp(g_app.settings.resolutionIndex,0,2),0);SendMessageW(g_app.fps,CB_SETCURSEL,g_app.settings.fps==60?1:0,0);Button_SetCheck(g_app.cursor,g_app.settings.cursor?BST_CHECKED:BST_UNCHECKED);Button_SetCheck(g_app.settings.mode==TransmissionMode::Quick?g_app.quickMode:g_app.protectedMode,BST_CHECKED);Button_SetCheck(g_app.allowWhatsApp,BST_UNCHECKED);Button_SetCheck(g_app.allowTelegram,BST_UNCHECKED);addTrayIcon();updateControls();if(g_app.settings.showHelpOnStart)PostMessageW(hwnd,WM_COMMAND,IdHelp,0);return 0;}
    case WM_COMMAND:{int id=LOWORD(wp);if(id==IdSourceType&&HIWORD(wp)==CBN_SELCHANGE){loadSourceCombo();saveSettings();}else if(id==IdRefreshSources)loadSourceCombo();else if(id==IdStart){startTransmission();updateControls();}else if(id==IdStop||id==IdTrayStop){stopTransmission();updateControls();}else if(id==IdRelease){if(g_app.receiverCount.load()==1){g_app.authorized=true;g_app.manualHidden=false;g_app.blockReason=BlockReason::None;postToast(L"Transmissão liberada para o receptor conectado.");postStatus(L"Conteúdo liberado.");}updateControls();}else if(id==IdHide||id==IdTrayHide){g_app.manualHidden=!g_app.manualHidden.load();postToast(g_app.manualHidden?L"Imagem ocultada sem encerrar a fonte NDI.":L"Imagem liberada novamente.");updateControls();}else if(id==IdAllowWhatsApp){g_app.allowWa=Button_GetCheck(g_app.allowWhatsApp)==BST_CHECKED;postToast(g_app.allowWa?L"WhatsApp permitido durante esta execução.":L"Proteção do WhatsApp ativada novamente.");}else if(id==IdAllowTelegram){g_app.allowTg=Button_GetCheck(g_app.allowTelegram)==BST_CHECKED;postToast(g_app.allowTg?L"Telegram permitido durante esta execução.":L"Proteção do Telegram ativada novamente.");}else if(id==IdHelp)showHelpDialog();else if(id==IdTrayOpen){ShowWindow(hwnd,SW_RESTORE);SetForegroundWindow(hwnd);}else if(id==IdTrayExit){if(g_app.running.load()&&MessageBoxW(hwnd,L"Há uma transmissão ativa. Deseja parar e sair?",L"Sair",MB_YESNO|MB_ICONQUESTION)!=IDYES)return 0;stopTransmission();DestroyWindow(hwnd);}return 0;}
    case kStatusMessage:{std::unique_ptr<std::wstring> s(reinterpret_cast<std::wstring*>(lp));if(s->rfind(L"__QUALITY__",0)==0)setText(g_app.quality,s->substr(11));else setText(g_app.status,*s);updateControls();return 0;}
    case kReceiverMessage:{int c=static_cast<int>(wp);setText(g_app.receivers,L"Receptores: "+std::to_wstring(c));if(g_app.activeMode==TransmissionMode::Protected&&c>1){FLASHWINFO f{sizeof(f),hwnd,FLASHW_TRAY,1,0};FlashWindowEx(&f);}updateControls();return 0;}
    case kToastMessage:{std::unique_ptr<std::wstring> s(reinterpret_cast<std::wstring*>(lp));setText(g_app.toast,*s);SetTimer(hwnd,kToastTimer,4500,nullptr);return 0;}
    case WM_TIMER:if(wp==kToastTimer){KillTimer(hwnd,kToastTimer);setText(g_app.toast,L"");}return 0;
    case kTrayMessage:if(lp==WM_RBUTTONUP||lp==WM_CONTEXTMENU)showTrayMenu();else if(lp==WM_LBUTTONDBLCLK){ShowWindow(hwnd,SW_RESTORE);SetForegroundWindow(hwnd);}return 0;
    case WM_SIZE:if(wp==SIZE_MINIMIZED)ShowWindow(hwnd,SW_HIDE);return 0;
    case WM_CLOSE:if(g_app.running.load()){if(MessageBoxW(hwnd,L"A transmissão está ativa. Deseja realmente parar e sair?",L"Transmissão ativa",MB_YESNO|MB_ICONQUESTION)!=IDYES)return 0;stopTransmission();}saveSettings();DestroyWindow(hwnd);return 0;
    case WM_DESTROY:removeTrayIcon();saveSettings();PostQuitMessage(0);return 0;
    }return DefWindowProcW(hwnd,msg,wp,lp);
}
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show) {
    INITCOMMONCONTROLSEX icc{sizeof(icc),ICC_STANDARD_CLASSES};InitCommonControlsEx(&icc);
    WNDCLASSEXW wc{sizeof(wc)};wc.lpfnWndProc=wndProc;wc.hInstance=instance;wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1));wc.hIconSm=wc.hIcon;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);wc.lpszClassName=kWindowClass;if(!RegisterClassExW(&wc))return 1;
    HWND hwnd=CreateWindowExW(0,kWindowClass,L"Transmissor NDI Portátil",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,760,630,nullptr,nullptr,instance,nullptr);if(!hwnd)return 1;ShowWindow(hwnd,show);UpdateWindow(hwnd);
    MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}if(g_app.worker.joinable()){g_app.stopRequested=true;g_app.worker.join();}return static_cast<int>(msg.wParam);
}
