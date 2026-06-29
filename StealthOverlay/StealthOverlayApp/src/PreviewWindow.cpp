// ============================================================
//  StealthOverlayApp / PreviewWindow.cpp
// ============================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "PreviewWindow.h"
#include <cassert>

namespace App {

// ── Static member ─────────────────────────────────────────
bool PreviewWindow::s_classRegistered = false;

// ── Destructor ────────────────────────────────────────────
PreviewWindow::~PreviewWindow()
{
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

// ── Create ────────────────────────────────────────────────
bool PreviewWindow::Create(HWND hParent, const RECT& windowRect)
{
    // Register window class once per process
    if (!s_classRegistered) {
        WNDCLASSEXW wc   = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = CLASS_NAME;

        s_classRegistered = (RegisterClassExW(&wc) != 0);
        if (!s_classRegistered) return false;
    }

    const int x = windowRect.left;
    const int y = windowRect.top;
    const int w = windowRect.right  - windowRect.left;
    const int h = windowRect.bottom - windowRect.top;

    m_hwnd = CreateWindowExW(
        0,
        CLASS_NAME,
        L"Capture Preview  \x2014  StealthOverlay",
        WS_OVERLAPPEDWINDOW,
        x, y, w, h,
        hParent,
        nullptr,
        GetModuleHandleW(nullptr),
        this    // WM_NCCREATE → lpCreateParams
    );

    return m_hwnd != nullptr;
}

// ── Show / Hide ───────────────────────────────────────────
void PreviewWindow::Show() { if (m_hwnd) ShowWindow(m_hwnd, SW_SHOWNORMAL); }
void PreviewWindow::Hide() { if (m_hwnd) ShowWindow(m_hwnd, SW_HIDE);       }

// ── PostFrame (capture thread) ────────────────────────────
void PreviewWindow::PostFrame(std::shared_ptr<CaptureCore::FrameData> frame)
{
    {
        std::lock_guard<std::mutex> lk(m_frameMtx);
        m_pendingFrame = std::move(frame);
    }
    // PostMessage is thread-safe and does not block
    if (m_hwnd) PostMessageW(m_hwnd, WM_NEW_FRAME, 0, 0);
}

// ── OnPaint (UI thread) ───────────────────────────────────
void PreviewWindow::OnPaint()
{
    // Promote pending frame to display frame
    {
        std::lock_guard<std::mutex> lk(m_frameMtx);
        if (m_pendingFrame) m_displayFrame = std::move(m_pendingFrame);
    }

    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(m_hwnd, &ps);

    RECT clientRect;
    GetClientRect(m_hwnd, &clientRect);
    const int cw = clientRect.right;
    const int ch = clientRect.bottom;

    if (m_displayFrame && !m_displayFrame->pixels.empty())
    {
        // ---- Blit BGRA frame scaled to fit the preview window ----
        const int fw = m_displayFrame->width;
        const int fh = m_displayFrame->height;

        BITMAPINFO bmi           = {};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = fw;
        bmi.bmiHeader.biHeight      = -fh;   // negative = top-down DIB
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        StretchDIBits(
            hdc,
            0, 0, cw, ch,                              // dest
            0, 0, fw, fh,                              // src
            m_displayFrame->pixels.data(),
            &bmi,
            DIB_RGB_COLORS,
            SRCCOPY
        );

        // ---- Overlay label ----
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 80, 80));
        RECT labelRect = { 10, 10, cw - 10, 36 };
        DrawTextW(hdc, L"CAPTURE PREVIEW  (overlay region masked)",
                  -1, &labelRect, DT_LEFT | DT_TOP | DT_SINGLELINE);
    }
    else
    {
        // ---- Placeholder before first frame ----
        HBRUSH bg = CreateSolidBrush(RGB(24, 24, 28));
        FillRect(hdc, &clientRect, bg);
        DeleteObject(bg);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(140, 140, 160));
        DrawTextW(hdc,
            L"Press  Ctrl+Shift+P  to start capture preview",
            -1, &clientRect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    EndPaint(m_hwnd, &ps);
}

// ── WndProc ───────────────────────────────────────────────
LRESULT CALLBACK PreviewWindow::WndProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    PreviewWindow* pThis = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis    = reinterpret_cast<PreviewWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
    }
    else {
        pThis = reinterpret_cast<PreviewWindow*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    switch (msg)
    {
    case WM_NEW_FRAME:
        // Trigger repaint without erasing background (avoids flicker)
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_PAINT:
        if (pThis) pThis->OnPaint();
        return 0;

    case WM_CLOSE:
        // Hide instead of destroy so the window can be re-shown
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        if (pThis) pThis->m_hwnd = nullptr;
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace App
