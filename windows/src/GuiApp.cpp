#include "GuiApp.h"
#include "resource.h"
#include <wincodec.h>
#include <thread>
#include <algorithm>
#include <cstring>
#include <string>

// ── Shared state ─────────────────────────────────────────────────────────────
GuiState g_gui;

// ── Brand palette ─────────────────────────────────────────────────────────────
static const COLORREF C_ORANGE   = RGB(0xFF, 0x45, 0x00);
static const COLORREF C_DARK     = RGB(0x1A, 0x1A, 0x1A);
static const COLORREF C_SIDEBAR  = RGB(0x22, 0x22, 0x22);
static const COLORREF C_BG       = RGB(0xFA, 0xFA, 0xF8);
static const COLORREF C_CARD     = RGB(0xFF, 0xFF, 0xFF);
static const COLORREF C_TEXT     = RGB(0x1A, 0x1A, 0x1A);
static const COLORREF C_MUTED    = RGB(0x88, 0x88, 0x88);
static const COLORREF C_GREEN    = RGB(0x22, 0xC5, 0x5E);
static const COLORREF C_STATUSBR = RGB(0xF0, 0xF0, 0xEE);
static const COLORREF C_DIVIDER  = RGB(0xE8, 0xE8, 0xE4);

static const int SIDEBAR_W = 200;
static const int STATUSBAR_H = 34;
static const int WIN_W = 860;
static const int WIN_H = 560;

// ── GDI helpers ───────────────────────────────────────────────────────────────
static void FillR(HDC dc, int x, int y, int w, int h, COLORREF c) {
    RECT r{x, y, x+w, y+h};
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &r, br);
    DeleteObject(br);
}
static void DrawText_(HDC dc, const wchar_t* t, int x, int y, int w, int h,
                      COLORREF col, int align = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    SetTextColor(dc, col);
    SetBkMode(dc, TRANSPARENT);
    RECT r{x, y, x+w, y+h};
    DrawTextW(dc, t, -1, &r, align);
}
static HFONT MakeFont(int pts, bool bold, const wchar_t* face = L"Segoe UI") {
    return CreateFontW(-pts, 0, 0, 0,
        bold ? FW_BOLD : FW_NORMAL,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, face);
}
static void RoundRectFill(HDC dc, int x, int y, int w, int h, int r, COLORREF c) {
    HBRUSH br = CreateSolidBrush(c);
    HPEN   pn = CreatePen(PS_NULL, 0, 0);
    auto   ob = SelectObject(dc, br);
    auto   op = SelectObject(dc, pn);
    RoundRect(dc, x, y, x+w, y+h, r, r);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(br); DeleteObject(pn);
}

// Draw the orange speech-bubble P mascot at (x,y) of size s.
static void DrawLogo(HDC dc, int x, int y, int s) {
    HBRUSH orBr = CreateSolidBrush(C_ORANGE);
    HBRUSH wBr  = CreateSolidBrush(RGB(255,255,255));
    HBRUSH dBr  = CreateSolidBrush(C_DARK);
    HPEN   nPen = CreatePen(PS_NULL, 0, 0);

    // Body (rounded rect)
    SelectObject(dc, orBr); SelectObject(dc, nPen);
    int bx=x, by=y, bw=s, bh=int(s*0.75f);
    RoundRect(dc, bx, by, bx+bw, by+bh, s/5, s/5);

    // Tail triangle (bottom-left)
    POINT tri[3] = {{x + s/5, by+bh-2}, {x + s/3, by+bh+s/4}, {x + s/2, by+bh-2}};
    SelectObject(dc, orBr);
    Polygon(dc, tri, 3);

    // Left eye white
    int ew = s/5, ex1 = x+s/5+2, ey = by+bh/3;
    SelectObject(dc, wBr);
    Ellipse(dc, ex1, ey, ex1+ew, ey+ew);
    // Left pupil
    SelectObject(dc, dBr);
    int pw=ew/2;
    Ellipse(dc, ex1+ew/4, ey+ew/4, ex1+ew/4+pw, ey+ew/4+pw);

    // Right eye white
    int ex2 = x + s*3/5;
    SelectObject(dc, wBr);
    Ellipse(dc, ex2, ey, ex2+ew, ey+ew);
    // Right pupil
    SelectObject(dc, dBr);
    Ellipse(dc, ex2+ew/4, ey+ew/4, ex2+ew/4+pw, ey+ew/4+pw);

    DeleteObject(orBr); DeleteObject(wBr); DeleteObject(dBr); DeleteObject(nPen);
}

// ── Nav items ─────────────────────────────────────────────────────────────────
struct NavItem { const wchar_t* label; };
static const NavItem NAV[] = {{L"Dashboard"}, {L"Connection"}, {L"Settings"}, {L"About"}};
static int g_page = 0;  // active page index

// ── Sidebar logo bitmap (loaded once from logo-text.png) ─────────────────────
static HBITMAP g_logoTextBmp = nullptr;
static int     g_logoTextW   = 0;
static int     g_logoTextH   = 0;

// Load a PNG file via WIC, scale to fit within maxW×maxH, return premult-BGRA HBITMAP.
static HBITMAP LoadPngHBitmap(const wchar_t* path, int maxW, int maxH) {
    IWICImagingFactory* fac = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
        reinterpret_cast<void**>(&fac)))) return nullptr;

    IWICBitmapDecoder* dec = nullptr;
    if (FAILED(fac->CreateDecoderFromFilename(path, nullptr,
        GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec))) {
        fac->Release(); return nullptr;
    }

    IWICBitmapFrameDecode* frm = nullptr;
    dec->GetFrame(0, &frm);

    UINT srcW = 1, srcH = 1;
    frm->GetSize(&srcW, &srcH);
    float sc  = std::min(static_cast<float>(maxW) / srcW,
                         static_cast<float>(maxH) / srcH);
    UINT outW = std::max(1u, static_cast<UINT>(srcW * sc));
    UINT outH = std::max(1u, static_cast<UINT>(srcH * sc));

    IWICBitmapScaler* scaler = nullptr;
    fac->CreateBitmapScaler(&scaler);
    scaler->Initialize(frm, outW, outH, WICBitmapInterpolationModeFant);

    IWICFormatConverter* conv = nullptr;
    fac->CreateFormatConverter(&conv);
    conv->Initialize(scaler, GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);

    BITMAPINFOHEADER bih{};
    bih.biSize        = sizeof(bih);
    bih.biWidth       = static_cast<LONG>(outW);
    bih.biHeight      = -static_cast<LONG>(outH);  // top-down
    bih.biPlanes      = 1;
    bih.biBitCount    = 32;
    bih.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC   sdc  = GetDC(nullptr);
    HBITMAP hbmp = CreateDIBSection(sdc,
        reinterpret_cast<BITMAPINFO*>(&bih), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, sdc);

    if (hbmp && bits)
        conv->CopyPixels(nullptr, outW * 4, outW * outH * 4,
                         static_cast<BYTE*>(bits));

    conv->Release(); scaler->Release(); frm->Release();
    dec->Release();  fac->Release();
    return hbmp;
}

// ── Window state ──────────────────────────────────────────────────────────────
static HWND  g_hwnd  = nullptr;
static UINT_PTR g_timer = 0;

// ── Paint ─────────────────────────────────────────────────────────────────────
static void PaintSidebar(HDC dc, int winH) {
    const int h = winH - STATUSBAR_H;
    FillR(dc, 0, 0, SIDEBAR_W, h, C_SIDEBAR);

    // Logo-text image (or fallback)
    if (g_logoTextBmp) {
        HDC mdc2 = CreateCompatibleDC(dc);
        HGDIOBJ obj = SelectObject(mdc2, g_logoTextBmp);
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        AlphaBlend(dc, 8, 6, g_logoTextW, g_logoTextH,
                   mdc2, 0, 0, g_logoTextW, g_logoTextH, bf);
        SelectObject(mdc2, obj);
        DeleteDC(mdc2);
    } else {
        DrawLogo(dc, 18, 14, 36);
        HFONT fBold = MakeFont(13, true);
        SelectObject(dc, fBold);
        DrawText_(dc, L"PocketDisplay", 62, 14, SIDEBAR_W-70, 36, RGB(255,255,255));
        DeleteObject(fBold);
    }

    // Divider
    int divY = g_logoTextBmp ? (6 + g_logoTextH + 6) : 56;
    FillR(dc, 16, divY, SIDEBAR_W-32, 1, RGB(50,50,50));

    // Nav items
    int navStart = g_logoTextBmp ? (6 + g_logoTextH + 14) : 68;
    HFONT fNav = MakeFont(12, false);
    SelectObject(dc, fNav);
    for (int i = 0; i < 4; ++i) {
        int iy = navStart + i*44;
        bool active = (i == g_page);
        if (active) {
            FillR(dc, 0, iy, SIDEBAR_W, 40, RGB(0xFF,0x45,0x00));  // orange highlight
            FillR(dc, 0, iy, 3, 40, C_ORANGE);                     // left bar
        }
        DrawText_(dc, NAV[i].label, 22, iy, SIDEBAR_W-30, 40,
                  active ? RGB(255,255,255) : C_MUTED);
    }
    DeleteObject(fNav);

    // Version at bottom
    HFONT fSm = MakeFont(10, false);
    SelectObject(dc, fSm);
    DrawText_(dc, L"v0.1.0", 16, h-24, SIDEBAR_W-32, 20, RGB(80,80,80));
    DeleteObject(fSm);
}

static void PaintStatusBar(HDC dc, int winW, int winH) {
    int y = winH - STATUSBAR_H;
    FillR(dc, 0, y, winW, STATUSBAR_H, C_STATUSBR);
    FillR(dc, 0, y, winW, 1, C_DIVIDER);  // top border

    HFONT f = MakeFont(11, false);
    SelectObject(dc, f);

    // PC name
    wchar_t pcName[128]{}; DWORD pcNameLen = 127; GetComputerNameW(pcName, &pcNameLen);
    DrawText_(dc, pcName, SIDEBAR_W+16, y, 180, STATUSBAR_H, C_TEXT);

    // Connection status dot
    bool conn = g_gui.connected.load();
    HBRUSH dotBr = CreateSolidBrush(conn ? C_GREEN : C_MUTED);
    HPEN   np    = CreatePen(PS_NULL, 0, 0);
    SelectObject(dc, dotBr); SelectObject(dc, np);
    int dx = SIDEBAR_W + 210, dy = y + STATUSBAR_H/2 - 5;
    Ellipse(dc, dx, dy, dx+10, dy+10);
    DeleteObject(dotBr); DeleteObject(np);

    DrawText_(dc, conn ? L"Connected" : L"Disconnected",
              SIDEBAR_W+226, y, 160, STATUSBAR_H, conn ? C_GREEN : C_MUTED);

    DeleteObject(f);
}

static void PaintDashboard(HDC dc, int cx, int cy, int cw, int ch) {
    // Section title
    HFONT fTitle = MakeFont(18, true);
    SelectObject(dc, fTitle);
    DrawText_(dc, L"Dashboard", cx+20, cy+18, cw-40, 32, C_TEXT);
    DeleteObject(fTitle);

    HFONT fLabel = MakeFont(10, false);
    HFONT fVal   = MakeFont(13, true);
    HFONT fSm    = MakeFont(11, false);

    // ── Status card ──────────────────────────────────────────────────
    int cardY = cy+62, cardH = 80;
    RoundRectFill(dc, cx+20, cardY, cw-40, cardH, 12, C_CARD);

    SelectObject(dc, fLabel);
    DrawText_(dc, L"STATUS", cx+36, cardY+12, 120, 18, C_MUTED);

    bool conn = g_gui.connected.load();
    bool strm = g_gui.streaming.load();
    const wchar_t* statusStr = strm ? (conn ? L"Streaming" : L"Connecting…")
                                    : L"Idle — waiting for Android";
    SelectObject(dc, fVal);
    DrawText_(dc, statusStr, cx+36, cardY+30, cw-80, 22, C_TEXT);

    // Status dot
    HBRUSH dotBr = CreateSolidBrush(conn ? C_GREEN : C_MUTED);
    HPEN   np    = CreatePen(PS_NULL, 0, 0);
    SelectObject(dc, dotBr); SelectObject(dc, np);
    Ellipse(dc, cx+36, cardY+56, cx+50, cardY+70);
    DeleteObject(dotBr); DeleteObject(np);

    SelectObject(dc, fSm);
    char sm[256]; strncpy_s(sm, g_gui.statusMsg, 255);
    int smLen = MultiByteToWideChar(CP_UTF8, 0, sm, -1, nullptr, 0);
    std::wstring smW(smLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, sm, -1, smW.data(), smLen);
    DrawText_(dc, smW.c_str(), cx+54, cardY+52, cw-100, 20, C_MUTED);

    // ── Stats card ────────────────────────────────────────────────────
    int sc1y = cardY + cardH + 14;
    RoundRectFill(dc, cx+20, sc1y, (cw-50)/2, 70, 12, C_CARD);
    SelectObject(dc, fLabel);
    DrawText_(dc, L"RESOLUTION", cx+36, sc1y+12, 140, 18, C_MUTED);
    SelectObject(dc, fVal);
    int w = g_gui.capW.load(), h = g_gui.capH.load();
    if (w > 0 && h > 0) {
        wchar_t res[32]; swprintf_s(res, L"%d×%d", w, h);
        DrawText_(dc, res, cx+36, sc1y+32, 180, 24, C_TEXT);
    } else {
        DrawText_(dc, L"—", cx+36, sc1y+32, 80, 24, C_TEXT);
    }

    int sc2x = cx+20+(cw-50)/2+10;
    RoundRectFill(dc, sc2x, sc1y, (cw-50)/2, 70, 12, C_CARD);
    SelectObject(dc, fLabel);
    DrawText_(dc, L"FPS", sc2x+16, sc1y+12, 100, 18, C_MUTED);
    SelectObject(dc, fVal);
    int fps = g_gui.fps.load();
    wchar_t fpsStr[16]; swprintf_s(fpsStr, fps > 0 ? L"%d fps" : L"—", fps);
    DrawText_(dc, fpsStr, sc2x+16, sc1y+32, 140, 24, C_TEXT);

    // ── Mode card ─────────────────────────────────────────────────────
    int mcY = sc1y + 84;
    RoundRectFill(dc, cx+20, mcY, cw-40, 52, 12, C_CARD);
    SelectObject(dc, fLabel);
    DrawText_(dc, L"MODE", cx+36, mcY+10, 80, 18, C_MUTED);
    SelectObject(dc, fSm);
    wchar_t modeW[32]{}; MultiByteToWideChar(CP_ACP, 0, g_gui.mode, -1, modeW, 32);
    DrawText_(dc, modeW, cx+110, mcY+10, 200, 18, C_MUTED);
    SelectObject(dc, fVal);
    int bk = g_gui.bitrateKbps.load();
    wchar_t bkStr[32]; swprintf_s(bkStr, bk > 0 ? L"%d kbps" : L"—", bk);
    DrawText_(dc, bkStr, cx+36, mcY+28, cw-80, 20, C_TEXT);

    DeleteObject(fLabel); DeleteObject(fVal); DeleteObject(fSm);
}

static void PaintContent(HDC dc, int winW, int winH) {
    int cx = SIDEBAR_W, cy = 0;
    int cw = winW - SIDEBAR_W, ch = winH - STATUSBAR_H;
    FillR(dc, cx, cy, cw, ch, C_BG);
    // Sidebar shadow strip
    FillR(dc, cx, cy, 1, ch, C_DIVIDER);

    switch (g_page) {
    case 0: PaintDashboard(dc, cx, cy, cw, ch); break;
    case 1: {
        HFONT f = MakeFont(16, true);
        SelectObject(dc, f);
        DrawText_(dc, L"Connection", cx+20, cy+18, cw-40, 32, C_TEXT);
        DeleteObject(f);
        HFONT fs = MakeFont(12, false);
        SelectObject(dc, fs);
        DrawText_(dc, L"USB mode: adb reverse tcp:7777 tcp:7777", cx+20, cy+70, cw-40, 24, C_MUTED);
        DrawText_(dc, L"WiFi mode: auto-discovery via UDP broadcast :7779", cx+20, cy+98, cw-40, 24, C_MUTED);
        DeleteObject(fs);
        break;
    }
    case 2: {
        HFONT f = MakeFont(16, true);
        SelectObject(dc, f);
        DrawText_(dc, L"Settings", cx+20, cy+18, cw-40, 32, C_TEXT);
        DeleteObject(f);
        HFONT fs = MakeFont(12, false);
        SelectObject(dc, fs);
        DrawText_(dc, L"Mirror / Extended mode, bitrate and FPS are set via CLI flags.", cx+20, cy+70, cw-40, 24, C_MUTED);
        DrawText_(dc, L"Example:  PocketDisplay.exe --extend --bitrate=12000 --fps=60", cx+20, cy+98, cw-40, 24, C_MUTED);
        DeleteObject(fs);
        break;
    }
    case 3: {
        HFONT f = MakeFont(16, true);
        SelectObject(dc, f);
        DrawText_(dc, L"About PocketDisplay", cx+20, cy+18, cw-40, 32, C_TEXT);
        DeleteObject(f);
        HFONT fs = MakeFont(12, false);
        SelectObject(dc, fs);
        DrawText_(dc, L"Version 0.1.0  ·  Use your Android phone as a wireless/USB display.", cx+20, cy+70, cw-40, 24, C_MUTED);
        DrawText_(dc, L"github.com/pocketdisplay", cx+20, cy+98, cw-40, 24, C_ORANGE);
        DeleteObject(fs);
        break;
    }
    }
}

// ── WndProc ───────────────────────────────────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;  // avoid flicker; we paint everything in WM_PAINT

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT cr; GetClientRect(hwnd, &cr);
        int W = cr.right, H = cr.bottom;

        // Double-buffer
        HDC mdc = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, W, H);
        HGDIOBJ old = SelectObject(mdc, bmp);

        PaintSidebar(mdc, H);
        PaintContent(mdc, W, H);
        PaintStatusBar(mdc, W, H);

        BitBlt(dc, 0, 0, W, H, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, old);
        DeleteObject(bmp);
        DeleteDC(mdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int mx = LOWORD(lp), my = HIWORD(lp);
        // Hit-test nav items
        for (int i = 0; i < 4; ++i) {
            int iy = 68 + i * 44;
            if (mx < SIDEBAR_W && my >= iy && my < iy + 40) {
                g_page = i;
                InvalidateRect(hwnd, nullptr, FALSE);
                break;
            }
        }
        return 0;
    }

    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);  // repaint for live stats
        return 0;

    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize = {WIN_W, WIN_H};
        return 0;
    }

    case WM_CLOSE:
        // Don't destroy — just hide. Streaming keeps running.
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, g_timer);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── GUI thread ────────────────────────────────────────────────────────────────
static void GuiThread() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    HINSTANCE hInst = GetModuleHandleW(nullptr);

    // ── Load logo-text.png from exe directory ─────────────────────────────────
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) { lastSlash[1] = L'\0'; }
    wchar_t imgPath[MAX_PATH]{};
    wcscpy_s(imgPath, exePath);
    wcscat_s(imgPath, L"logo-text.png");

    // Max 184 wide, 52 tall to fit the sidebar header area
    g_logoTextBmp = LoadPngHBitmap(imgPath, SIDEBAR_W - 16, 52);
    if (g_logoTextBmp) {
        BITMAP bm{};
        GetObject(g_logoTextBmp, sizeof(bm), &bm);
        g_logoTextW = bm.bmWidth;
        g_logoTextH = bm.bmHeight;
    }

    // ── Register window class ─────────────────────────────────────────────────
    HICON hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hIcon         = hIcon;
    wc.hIconSm       = hIcon;
    wc.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"PocketDisplayGui";
    RegisterClassExW(&wc);

    // Center on primary monitor
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);

    g_hwnd = CreateWindowExW(
        0, L"PocketDisplayGui", L"PocketDisplay",
        WS_OVERLAPPEDWINDOW,
        (sx - WIN_W) / 2, (sy - WIN_H) / 2, WIN_W, WIN_H,
        nullptr, nullptr, hInst, nullptr);

    if (!g_hwnd) { CoUninitialize(); return; }

    // Ensure taskbar + title bar also get the icon
    SendMessageW(g_hwnd, WM_SETICON, ICON_BIG,   reinterpret_cast<LPARAM>(hIcon));
    SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL,  reinterpret_cast<LPARAM>(hIcon));

    // Refresh every 1 second for live stats
    g_timer = SetTimer(g_hwnd, 1, 1000, nullptr);

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_logoTextBmp) { DeleteObject(g_logoTextBmp); g_logoTextBmp = nullptr; }
    CoUninitialize();
}

void GuiLaunch() {
    std::thread(GuiThread).detach();
}
