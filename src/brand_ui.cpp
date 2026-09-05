#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <iterator>
#include <string>

namespace {
constexpr wchar_t kMainWindowClass[] = L"TransmissorNDIPortatilV3";
constexpr wchar_t kAboutWindowClass[] = L"ZosmaTransmitterAboutV1";
constexpr UINT_PTR kBrandTimer = 93;
constexpr int kIdAbout = 1501;
constexpr int kIdAboutSite = 1502;
constexpr int kIdAboutLicenses = 1503;
constexpr int kIdAboutClose = 1504;

constexpr COLORREF kBg = RGB(10, 18, 30);
constexpr COLORREF kPanel = RGB(18, 31, 49);
constexpr COLORREF kPanelHover = RGB(25, 43, 67);
constexpr COLORREF kBorder = RGB(56, 82, 112);
constexpr COLORREF kAccent = RGB(37, 150, 255);
constexpr COLORREF kAccentDark = RGB(19, 74, 129);
constexpr COLORREF kText = RGB(242, 247, 255);
constexpr COLORREF kMuted = RGB(164, 185, 214);
constexpr COLORREF kDisabled = RGB(112, 126, 145);

HWND gMain{};
HWND gAbout{};
HHOOK gHook{};

HFONT makeFont(int height, int weight) {
    LOGFONTW lf{};
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    lf.lfHeight = -height;
    lf.lfWeight = weight;
    return CreateFontIndirectW(&lf);
}

void roundWindow(HWND hwnd, int radius = 10) {
    if (!hwnd || !IsWindow(hwnd)) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    HRGN region = CreateRoundRectRgn(0, 0, rc.right + 1, rc.bottom + 1, radius, radius);
    SetWindowRgn(hwnd, region, TRUE);
}

bool cursorInside(HWND hwnd) {
    POINT p{};
    GetCursorPos(&p);
    ScreenToClient(hwnd, &p);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    return PtInRect(&rc, p) != FALSE;
}

void drawRoundedButton(HWND hwnd, HDC dc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int id = GetDlgCtrlID(hwnd);
    const bool enabled = IsWindowEnabled(hwnd) != FALSE;
    const bool hot = cursorInside(hwnd);
    const bool pressed = (SendMessageW(hwnd, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;

    COLORREF fill = kPanel;
    COLORREF border = kBorder;
    COLORREF text = enabled ? kText : kDisabled;

    if (id == 1011) {
        fill = enabled ? (pressed ? RGB(21, 112, 207) : hot ? RGB(45, 163, 255) : kAccent) : RGB(42, 64, 86);
        border = enabled ? RGB(83, 183, 255) : RGB(66, 82, 100);
    } else if (id == 1012) {
        fill = enabled ? (hot ? RGB(48, 65, 86) : RGB(39, 52, 69)) : RGB(48, 53, 61);
        border = enabled ? RGB(106, 130, 160) : RGB(76, 81, 90);
    } else if (hot && enabled) {
        fill = kPanelHover;
        border = RGB(82, 119, 160);
    }

    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, 12, 12);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);

    wchar_t textBuffer[256]{};
    GetWindowTextW(hwnd, textBuffer, static_cast<int>(std::size(textBuffer)));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    HFONT oldFont = font ? static_cast<HFONT>(SelectObject(dc, font)) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text);
    RECT tr = rc;
    DrawTextW(dc, textBuffer, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (oldFont) SelectObject(dc, oldFont);
}

void drawRadioCard(HWND hwnd, HDC dc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const bool checked = SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool hot = cursorInside(hwnd);
    const bool enabled = IsWindowEnabled(hwnd) != FALSE;

    const COLORREF fill = checked ? RGB(16, 47, 78) : hot ? RGB(23, 38, 57) : RGB(19, 29, 43);
    const COLORREF border = checked ? kAccent : hot ? RGB(75, 102, 136) : kBorder;
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, checked ? 2 : 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, 14, 14);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);

    const int cy = rc.top + 23;
    HBRUSH dotBrush = CreateSolidBrush(checked ? kAccent : fill);
    HPEN dotPen = CreatePen(PS_SOLID, 2, checked ? kAccent : RGB(125, 151, 183));
    oldBrush = SelectObject(dc, dotBrush);
    oldPen = SelectObject(dc, dotPen);
    Ellipse(dc, 14, cy - 8, 30, cy + 8);
    if (checked) {
        HBRUSH inner = CreateSolidBrush(RGB(220, 239, 255));
        HGDIOBJ prev = SelectObject(dc, inner);
        Ellipse(dc, 19, cy - 3, 25, cy + 3);
        SelectObject(dc, prev);
        DeleteObject(inner);
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(dotBrush);
    DeleteObject(dotPen);

    wchar_t buffer[256]{};
    GetWindowTextW(hwnd, buffer, static_cast<int>(std::size(buffer)));
    std::wstring label = buffer;
    const size_t split = label.find(L"\r\n");
    const std::wstring title = split == std::wstring::npos ? label : label.substr(0, split);
    const std::wstring body = split == std::wstring::npos ? L"" : label.substr(split + 2);

    HFONT baseFont = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    HFONT titleFont = makeFont(15, FW_SEMIBOLD);
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, titleFont ? titleFont : baseFont));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, enabled ? kText : kDisabled);
    RECT titleRect{42, 8, rc.right - 12, 31};
    DrawTextW(dc, title.c_str(), -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (baseFont) SelectObject(dc, baseFont);
    SetTextColor(dc, enabled ? kMuted : kDisabled);
    RECT bodyRect{42, 31, rc.right - 12, rc.bottom - 7};
    DrawTextW(dc, body.c_str(), -1, &bodyRect, DT_LEFT | DT_TOP | DT_WORDBREAK);
    if (oldFont) SelectObject(dc, oldFont);
    if (titleFont) DeleteObject(titleFont);
}

void drawModernCheck(HWND hwnd, HDC dc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    HBRUSH bg = CreateSolidBrush(kPanel);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    const bool checked = SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool enabled = IsWindowEnabled(hwnd) != FALSE;
    const bool hot = cursorInside(hwnd);
    RECT box{2, (rc.bottom - 18) / 2, 20, (rc.bottom - 18) / 2 + 18};
    HBRUSH brush = CreateSolidBrush(checked ? kAccent : RGB(20, 29, 42));
    HPEN pen = CreatePen(PS_SOLID, 1, checked ? RGB(91, 190, 255) : hot ? RGB(95, 128, 166) : RGB(91, 111, 139));
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, box.left, box.top, box.right, box.bottom, 6, 6);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);

    if (checked) {
        HPEN checkPen = CreatePen(PS_SOLID, 2, RGB(239, 248, 255));
        HGDIOBJ old = SelectObject(dc, checkPen);
        MoveToEx(dc, 6, box.top + 9, nullptr);
        LineTo(dc, 10, box.top + 13);
        LineTo(dc, 17, box.top + 5);
        SelectObject(dc, old);
        DeleteObject(checkPen);
    }

    wchar_t textBuffer[256]{};
    GetWindowTextW(hwnd, textBuffer, static_cast<int>(std::size(textBuffer)));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    HFONT oldFont = font ? static_cast<HFONT>(SelectObject(dc, font)) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, enabled ? kText : kDisabled);
    RECT tr{28, 0, rc.right, rc.bottom};
    DrawTextW(dc, textBuffer, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (oldFont) SelectObject(dc, oldFont);
}

LRESULT CALLBACK buttonSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const UINT type = static_cast<UINT>(style & BS_TYPEMASK);
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        if (type == BS_AUTORADIOBUTTON || type == BS_RADIOBUTTON) drawRadioCard(hwnd, dc);
        else if (type == BS_AUTOCHECKBOX || type == BS_CHECKBOX) drawModernCheck(hwnd, dc);
        else drawRoundedButton(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (msg == WM_MOUSELEAVE || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ||
               msg == WM_ENABLE || msg == BM_SETCHECK || msg == WM_SETTEXT) {
        LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
        InvalidateRect(hwnd, nullptr, FALSE);
        return result;
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, buttonSubclass, 71);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void styleControl(HWND child) {
    if (!child || !IsWindow(child)) return;
    if (GetPropW(child, L"ZosmaPremiumStyled")) return;

    wchar_t cls[64]{};
    GetClassNameW(child, cls, static_cast<int>(std::size(cls)));
    if (_wcsicmp(cls, L"Button") == 0) {
        SetWindowSubclass(child, buttonSubclass, 71, 0);
        roundWindow(child, 12);
        SetPropW(child, L"ZosmaPremiumStyled", reinterpret_cast<HANDLE>(1));
        InvalidateRect(child, nullptr, TRUE);
    } else if (_wcsicmp(cls, L"Edit") == 0 || _wcsicmp(cls, WC_COMBOBOXW) == 0) {
        roundWindow(child, 10);
        SetPropW(child, L"ZosmaPremiumStyled", reinterpret_cast<HANDLE>(1));
    }
}

BOOL CALLBACK enumChildrenProc(HWND child, LPARAM) {
    styleControl(child);
    return TRUE;
}

void styleMainControls() {
    if (!gMain) return;
    EnumChildWindows(gMain, enumChildrenProc, 0);
}

void paintBrandHeader(HWND hwnd) {
    HDC dc = GetDC(hwnd);
    if (!dc) return;
    RECT cover{20, 12, 832, 72};
    HBRUSH bg = CreateSolidBrush(kBg);
    FillRect(dc, &cover, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    HFONT titleFont = makeFont(26, FW_SEMIBOLD);
    HFONT subFont = makeFont(14, FW_NORMAL);
    HFONT old = titleFont ? static_cast<HFONT>(SelectObject(dc, titleFont)) : nullptr;
    SetTextColor(dc, kText);
    RECT title{28, 18, 570, 48};
    DrawTextW(dc, L"Zosma Transmitter", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (subFont) SelectObject(dc, subFont);
    SetTextColor(dc, kMuted);
    RECT sub{28, 48, 570, 69};
    DrawTextW(dc, L"Uma solução Zosma Labs", -1, &sub, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (old) SelectObject(dc, old);
    if (titleFont) DeleteObject(titleFont);
    if (subFont) DeleteObject(subFont);
    ReleaseDC(hwnd, dc);
}

void showLicenseNotice(HWND owner) {
    MessageBoxW(owner,
        L"Zosma Transmitter utiliza tecnologia NDI® para transporte de vídeo e áudio em rede.\n\n"
        L"Zosma Transmitter é um software independente e não é afiliado, patrocinado, certificado ou desenvolvido pela Vizrt NDI AB.\n\n"
        L"NDI® é uma marca registrada da Vizrt NDI AB.\n\n"
        L"Os termos aplicáveis aos componentes de terceiros também constam no arquivo AVISOS-DE-TERCEIROS.txt distribuído com o aplicativo.",
        L"Licenças e componentes de terceiros", MB_OK | MB_ICONINFORMATION);
}

LRESULT CALLBACK aboutProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
        const DWORD corner = 2;
        DwmSetWindowAttribute(hwnd, 33, &corner, sizeof(corner));
        HFONT font = makeFont(15, FW_SEMIBOLD);
        HWND site = CreateWindowExW(0, L"BUTTON", L"zosma.com.br  ↗", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            28, 382, 170, 38, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAboutSite)), GetModuleHandleW(nullptr), nullptr);
        HWND licenses = CreateWindowExW(0, L"BUTTON", L"Licenças e componentes", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            210, 382, 210, 38, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAboutLicenses)), GetModuleHandleW(nullptr), nullptr);
        HWND close = CreateWindowExW(0, L"BUTTON", L"Fechar", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            432, 382, 120, 38, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAboutClose)), GetModuleHandleW(nullptr), nullptr);
        for (HWND b : {site, licenses, close}) {
            SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            SetWindowSubclass(b, buttonSubclass, 71, 0);
            roundWindow(b, 12);
        }
        DeleteObject(font);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kIdAboutSite:
            ShellExecuteW(hwnd, L"open", L"https://zosma.com.br", nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        case kIdAboutLicenses:
            showLicenseNotice(hwnd);
            return 0;
        case kIdAboutClose:
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        HBRUSH bg = CreateSolidBrush(kBg);
        FillRect(dc, &client, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);

        HFONT title = makeFont(28, FW_SEMIBOLD);
        HFONT body = makeFont(15, FW_NORMAL);
        HFONT bold = makeFont(15, FW_SEMIBOLD);
        HFONT small = makeFont(13, FW_NORMAL);
        HFONT old = title ? static_cast<HFONT>(SelectObject(dc, title)) : nullptr;
        SetTextColor(dc, kText);
        RECT r{28, 24, 552, 58};
        DrawTextW(dc, L"Zosma Transmitter", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (body) SelectObject(dc, body);
        SetTextColor(dc, kMuted);
        r = {28, 60, 552, 84};
        DrawTextW(dc, L"Uma solução Zosma Labs  ·  Versão 0.3.0 Beta", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        HBRUSH panel = CreateSolidBrush(kPanel);
        HPEN pen = CreatePen(PS_SOLID, 1, kBorder);
        HGDIOBJ oldBrush = SelectObject(dc, panel);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        RoundRect(dc, 28, 102, 552, 220, 16, 16);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(panel);
        DeleteObject(pen);

        if (bold) SelectObject(dc, bold);
        SetTextColor(dc, kText);
        r = {48, 120, 530, 146};
        DrawTextW(dc, L"Zosma Labs", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (body) SelectObject(dc, body);
        SetTextColor(dc, RGB(197, 218, 245));
        r = {48, 146, 530, 174};
        DrawTextW(dc, L"Ideias transformadas em software.", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (small) SelectObject(dc, small);
        SetTextColor(dc, kMuted);
        r = {48, 178, 530, 208};
        DrawTextW(dc, L"zosma.com.br", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        if (bold) SelectObject(dc, bold);
        SetTextColor(dc, kText);
        r = {28, 242, 552, 268};
        DrawTextW(dc, L"Tecnologia NDI®", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (small) SelectObject(dc, small);
        SetTextColor(dc, kMuted);
        r = {28, 270, 552, 352};
        DrawTextW(dc,
            L"Zosma Transmitter utiliza tecnologia NDI® para transmissão de vídeo e áudio em rede.\n\n"
            L"É um software independente e não é afiliado, patrocinado, certificado ou desenvolvido pela Vizrt NDI AB. NDI® é uma marca registrada da Vizrt NDI AB.",
            -1, &r, DT_LEFT | DT_WORDBREAK);

        if (old) SelectObject(dc, old);
        if (title) DeleteObject(title);
        if (body) DeleteObject(body);
        if (bold) DeleteObject(bold);
        if (small) DeleteObject(small);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (hwnd == gAbout) gAbout = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void showAbout() {
    if (gAbout && IsWindow(gAbout)) {
        ShowWindow(gAbout, SW_RESTORE);
        SetForegroundWindow(gAbout);
        return;
    }
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = aboutProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(kBg);
    wc.lpszClassName = kAboutWindowClass;
    RegisterClassExW(&wc);

    RECT mainRect{};
    GetWindowRect(gMain, &mainRect);
    const int width = 596;
    const int height = 486;
    const int x = mainRect.left + ((mainRect.right - mainRect.left) - width) / 2;
    const int y = mainRect.top + ((mainRect.bottom - mainRect.top) - height) / 2;
    gAbout = CreateWindowExW(WS_EX_DLGMODALFRAME, kAboutWindowClass, L"Sobre — Zosma Transmitter",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, width, height,
        gMain, nullptr, instance, nullptr);
    if (gAbout) {
        ShowWindow(gAbout, SW_SHOW);
        UpdateWindow(gAbout);
    }
}

LRESULT CALLBACK mainSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_COMMAND && LOWORD(wp) == kIdAbout) {
        showAbout();
        return 0;
    }
    if (msg == WM_TIMER && wp == kBrandTimer) {
        styleMainControls();
        return 0;
    }
    if (msg == WM_PAINT) {
        const LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
        paintBrandHeader(hwnd);
        return result;
    }
    if (msg == WM_NCDESTROY) {
        KillTimer(hwnd, kBrandTimer);
        RemoveWindowSubclass(hwnd, mainSubclass, 7);
        gMain = nullptr;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void installBrandUi(HWND hwnd) {
    if (gMain || !hwnd) return;
    gMain = hwnd;
    SetWindowTextW(hwnd, L"Zosma Transmitter — V3");

    HFONT font = makeFont(15, FW_SEMIBOLD);
    HWND about = CreateWindowExW(0, L"BUTTON", L"Sobre", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        708, 18, 120, 34, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAbout)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(about, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    DeleteObject(font);

    SetWindowSubclass(hwnd, mainSubclass, 7, 0);
    styleMainControls();
    SetTimer(hwnd, kBrandTimer, 500, nullptr);
    InvalidateRect(hwnd, nullptr, TRUE);
}

LRESULT CALLBACK callWndHook(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && !gMain) {
        const auto* data = reinterpret_cast<CWPSTRUCT*>(lp);
        if (data && data->hwnd && data->message == WM_PAINT) {
            wchar_t className[128]{};
            GetClassNameW(data->hwnd, className, static_cast<int>(std::size(className)));
            if (wcscmp(className, kMainWindowClass) == 0) installBrandUi(data->hwnd);
        }
    }
    return CallNextHookEx(gHook, code, wp, lp);
}

struct BrandBootstrap {
    BrandBootstrap() { gHook = SetWindowsHookExW(WH_CALLWNDPROC, callWndHook, nullptr, GetCurrentThreadId()); }
    ~BrandBootstrap() { if (gHook) UnhookWindowsHookEx(gHook); }
} gBrandBootstrap;
}
