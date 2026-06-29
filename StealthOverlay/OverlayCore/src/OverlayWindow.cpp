// ============================================================
//  OverlayCore / OverlayWindow.cpp  (v5 — realtime AI only)
//
//  Simple dark overlay that shows:
//    • Idle:       "Hold Ctrl+Shift+R to ask a question"
//    • Listening:  "Listening..." status in header
//    • Thinking:   "Thinking..." status in header
//    • Result:     Transcribed question + AI answer
//
//  Shortcuts strip at the bottom (always visible).
// ============================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include "OverlayWindow.h"

#ifndef WDA_EXCLUDEFROMCAPTURE
#  define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace OverlayCore {

// ── Colour palette ────────────────────────────────────────
namespace C {
    constexpr COLORREF BG          = RGB( 18,  18,  22);
    constexpr COLORREF PANEL_LINE  = RGB( 36,  38,  46);

    constexpr COLORREF ICON_FILL   = RGB(255, 185,  48);
    constexpr COLORREF ICON_INNER  = RGB( 18,  18,  22);

    constexpr COLORREF Q_BADGE     = RGB(255, 185,  48);
    constexpr COLORREF Q_TEXT      = RGB(248, 248, 252);

    constexpr COLORREF A_BADGE     = RGB( 88, 200, 110);
    constexpr COLORREF A_TEXT      = RGB(190, 193, 196);

    constexpr COLORREF KBD_BG     = RGB( 36,  38,  50);
    constexpr COLORREF KBD_BORDER = RGB( 58,  64,  80);
    constexpr COLORREF KBD_TEXT   = RGB(210, 213, 222);
    constexpr COLORREF SC_LABEL   = RGB( 88,  92, 106);
    constexpr COLORREF SC_PANEL   = RGB( 20,  20,  26);

    constexpr COLORREF IDLE       = RGB( 75,  80,  96);

    // Code block
    constexpr COLORREF CODE_BG    = RGB( 12,  16,  26);   // very dark navy
    constexpr COLORREF CODE_BAR   = RGB( 50, 120, 200);   // left-accent blue
    constexpr COLORREF CODE_TEXT  = RGB(200, 210, 220);   // pale blue-gray
    constexpr COLORREF CODE_LANG  = RGB( 86, 156, 214);   // VS-Code blue label
    constexpr COLORREF CODE_EDGE  = RGB( 35,  50,  75);   // box border

    // Listening pulse
    constexpr COLORREF LISTEN_FILL = RGB(  0, 230, 140);  // bright mint-green
    constexpr COLORREF LISTEN_RING = RGB(  0, 200, 120);  // ring glow
}

// ── Font helper ───────────────────────────────────────────
static HFONT MF(HDC hdc, int pts, int weight,
                const wchar_t* face = L"Segoe UI")
{
    int h = -MulDiv(pts, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    return CreateFontW(h, 0, 0, 0, weight,
                       FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_SWISS,
                       face);
}

// ── Draw filled diamond icon ──────────────────────────────
static void DrawDiamond(HDC hdc, int cx, int cy, int r,
                        COLORREF fillColor = C::ICON_FILL)
{
    POINT pts[4] = {
        { cx,     cy - r },
        { cx + r, cy     },
        { cx,     cy + r },
        { cx - r, cy     }
    };
    HBRUSH br  = CreateSolidBrush(fillColor);
    HPEN   pen = CreatePen(PS_NULL, 0, 0);
    HBRUSH ob  = static_cast<HBRUSH>(SelectObject(hdc, br));
    HPEN   op  = static_cast<HPEN>(SelectObject(hdc, pen));
    Polygon(hdc, pts, 4);
    SelectObject(hdc, ob); SelectObject(hdc, op);
    DeleteObject(br); DeleteObject(pen);

    HBRUSH ib = CreateSolidBrush(C::ICON_INNER);
    ob  = static_cast<HBRUSH>(SelectObject(hdc, ib));
    pen = CreatePen(PS_NULL, 0, 0);
    op  = static_cast<HPEN>(SelectObject(hdc, pen));
    int ir = std::max(2, r / 3);
    Ellipse(hdc, cx - ir, cy - ir, cx + ir, cy + ir);
    SelectObject(hdc, ob); SelectObject(hdc, op);
    DeleteObject(ib); DeleteObject(pen);
}

// ── Listening pulse rings around diamond ─────────────────
//  pulseFrame 0,1,2 → ring grows outward to show animation
static void DrawListenPulse(HDC hdc, int cx, int cy, int frame)
{
    HBRUSH nullBr = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));

    // Inner fixed ring (always visible when listening)
    {
        HPEN pen = CreatePen(PS_SOLID, 2, C::LISTEN_RING);
        HPEN op  = static_cast<HPEN>(SelectObject(hdc, pen));
        HBRUSH ob = static_cast<HBRUSH>(SelectObject(hdc, nullBr));
        const int r = 14;
        Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
        SelectObject(hdc, op); SelectObject(hdc, ob);
        DeleteObject(pen);
    }

    // Outer expanding ring (opacity: frame 0 = inner, frame 2 = outer)
    {
        int r = 18 + frame * 5;          // 18, 23, 28
        BYTE alpha = (BYTE)(180 - frame * 55); // 180, 125, 70 — fades out
        COLORREF col = RGB(0, (BYTE)(170 + frame * 20), (BYTE)(100 + frame * 15));
        HPEN pen = CreatePen(PS_SOLID, 2, col);
        HPEN op  = static_cast<HPEN>(SelectObject(hdc, pen));
        HBRUSH ob = static_cast<HBRUSH>(SelectObject(hdc, nullBr));
        Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
        SelectObject(hdc, op); SelectObject(hdc, ob);
        DeleteObject(pen);
        (void)alpha; // GDI pen colour is enough for visual distinction
    }
}

// ── Horizontal rule ───────────────────────────────────────
static void HRule(HDC hdc, int x0, int x1, int y, COLORREF col)
{
    HPEN pen = CreatePen(PS_SOLID, 1, col);
    HPEN old = static_cast<HPEN>(SelectObject(hdc, pen));
    MoveToEx(hdc, x0, y, nullptr);
    LineTo  (hdc, x1, y);
    SelectObject(hdc, old);
    DeleteObject(pen);
}

// ── Word-wrapped text block ───────────────────────────────
//  Returns Y after the block.
static int DrawBlock(HDC hdc, const wchar_t* text, COLORREF col,
                     int x, int y, int w, HFONT font, int maxY = 0)
{
    if (!text || !text[0]) return y;
    HFONT old = static_cast<HFONT>(SelectObject(hdc, font));
    SetTextColor(hdc, col);

    RECT r { x, y, x + w, y + 3000 };
    DrawTextW(hdc, text, -1, &r, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    int h = r.bottom - r.top;
    if (maxY > 0 && y + h > maxY) h = maxY - y;

    r = { x, y, x + w, y + h + 4 };
    DrawTextW(hdc, text, -1, &r, DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(hdc, old);
    return y + h + 8;
}

// ── Code block renderer ───────────────────────────────────
//  Draws a styled code box (dark bg, blue left bar, Consolas).
//  fCode = Consolas 12pt, fLang = Segoe UI 9pt bold.
//  Returns Y after the block.
static int DrawCodeBlock(HDC hdc, const wchar_t* code, const wchar_t* lang,
                          int x, int y, int cw,
                          HFONT fCode, HFONT fLang, int maxY)
{
    if (!code || !code[0]) return y;

    const int BAR  = 4;    // left accent bar width
    const int HPAD = 12;   // horizontal text padding inside box
    const int VPAD = 8;    // vertical padding

    // Measure code text height
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, fCode));
    RECT measure { x + BAR + HPAD, 0, x + cw - HPAD, 3000 };
    DrawTextW(hdc, code, -1, &measure,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
    int codeH = measure.bottom - measure.top;
    SelectObject(hdc, oldFont);

    // Optional language-label row
    int langRowH = (lang && lang[0]) ? 18 : 0;

    int boxH = VPAD + langRowH + codeH + VPAD;
    if (maxY > 0 && y + boxH > maxY) boxH = maxY - y;
    if (boxH < 12) return y;

    // Background
    RECT boxR { x, y, x + cw, y + boxH };
    HBRUSH bg = CreateSolidBrush(C::CODE_BG);
    FillRect(hdc, &boxR, bg);
    DeleteObject(bg);

    // Left accent bar
    RECT barR { x, y, x + BAR, y + boxH };
    HBRUSH bar = CreateSolidBrush(C::CODE_BAR);
    FillRect(hdc, &barR, bar);
    DeleteObject(bar);

    // Box border
    HPEN   borderPen = CreatePen(PS_SOLID, 1, C::CODE_EDGE);
    HBRUSH nullBr    = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HPEN   oldPen    = static_cast<HPEN>  (SelectObject(hdc, borderPen));
    HBRUSH oldBr     = static_cast<HBRUSH>(SelectObject(hdc, nullBr));
    Rectangle(hdc, x, y, x + cw, y + boxH);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBr);
    DeleteObject(borderPen);

    SetBkMode(hdc, TRANSPARENT);

    // Language label (top-right corner)
    int textY = y + VPAD;
    if (lang && lang[0] && langRowH > 0) {
        HFONT ol = static_cast<HFONT>(SelectObject(hdc, fLang));
        SetTextColor(hdc, C::CODE_LANG);
        RECT lr { x + BAR + HPAD, y + 4, x + cw - 6, y + 4 + langRowH };
        DrawTextW(hdc, lang, -1, &lr, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
        SelectObject(hdc, ol);
        textY = y + VPAD + langRowH;
    }

    // Code text
    HFONT ol = static_cast<HFONT>(SelectObject(hdc, fCode));
    SetTextColor(hdc, C::CODE_TEXT);
    RECT cr { x + BAR + HPAD, textY,
              x + cw - HPAD,  y + boxH - VPAD };
    DrawTextW(hdc, code, -1, &cr,
              DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
    SelectObject(hdc, ol);

    return y + boxH + 10;
}

// ── Mixed text + code block renderer ─────────────────────
//  Parses ``` fences in `answer`, dispatches text → DrawBlock
//  and code segments → DrawCodeBlock.  Returns final Y.
static int DrawMixedAnswer(HDC hdc, const std::wstring& answer,
                            int x, int y, int cw,
                            HFONT fText, HFONT fCode, HFONT fLang,
                            int maxY)
{
    const std::wstring FENCE = L"```";
    size_t pos = 0;
    const size_t LEN = answer.size();

    while (pos < LEN && (maxY <= 0 || y < maxY)) {
        size_t fs = answer.find(FENCE, pos);

        // Text before the fence (or rest of string)
        size_t textEnd = (fs == std::wstring::npos) ? LEN : fs;
        if (textEnd > pos) {
            std::wstring txt = answer.substr(pos, textEnd - pos);
            // Trim trailing blank lines
            while (!txt.empty() &&
                   (txt.back() == L'\n' || txt.back() == L'\r'))
                txt.pop_back();
            if (!txt.empty())
                y = DrawBlock(hdc, txt.c_str(), C::A_TEXT,
                              x, y, cw, fText, maxY);
        }
        if (fs == std::wstring::npos) break;

        // Skip opening ```
        pos = fs + 3;

        // Optional language tag on same line as opening fence
        std::wstring lang;
        size_t nl = answer.find(L'\n', pos);
        if (nl != std::wstring::npos) {
            lang = answer.substr(pos, nl - pos);
            while (!lang.empty() &&
                   (lang.back() == L'\r' || lang.back() == L' '))
                lang.pop_back();
            pos = nl + 1;
        }

        // Find closing ```
        size_t fe = answer.find(FENCE, pos);
        std::wstring code;
        if (fe != std::wstring::npos) {
            code = answer.substr(pos, fe - pos);
            pos  = fe + 3;
            if (pos < LEN && answer[pos] == L'\n') pos++;
        } else {
            code = answer.substr(pos);
            pos  = LEN;
        }
        // Trim trailing newlines from code
        while (!code.empty() &&
               (code.back() == L'\n' || code.back() == L'\r'))
            code.pop_back();

        if (!code.empty() && (maxY <= 0 || y < maxY)) {
            y += 6;
            y = DrawCodeBlock(hdc, code.c_str(), lang.c_str(),
                               x, y, cw, fCode, fLang, maxY);
        }
    }
    return y;
}

// ── Badge row: "Q ──────" ─────────────────────────────────
//  Returns Y after the badge.
static int DrawBadgeRow(HDC hdc, const wchar_t* label, COLORREF col,
                        int x, int y, int cw, HFONT font)
{
    HFONT old = static_cast<HFONT>(SelectObject(hdc, font));
    SetTextColor(hdc, col);
    SIZE sz {};
    GetTextExtentPoint32W(hdc, label, static_cast<int>(wcslen(label)), &sz);
    TextOutW(hdc, x, y, label, static_cast<int>(wcslen(label)));
    HRule(hdc, x + sz.cx + 10, x + cw, y + sz.cy / 2, C::PANEL_LINE);
    SelectObject(hdc, old);
    return y + sz.cy + 10;
}

// ── Keyboard chip  [CS+R] ────────────────────────────────
//  Returns chip pixel width + gap.
static int DrawKbd(HDC hdc, const wchar_t* key, int x, int y, HFONT font)
{
    HFONT old = static_cast<HFONT>(SelectObject(hdc, font));
    SIZE sz {};
    GetTextExtentPoint32W(hdc, key, static_cast<int>(wcslen(key)), &sz);
    const int pw = sz.cx + 10, ph = sz.cy + 4;

    RECT  chipR { x, y, x + pw, y + ph };
    HBRUSH br  = CreateSolidBrush(C::KBD_BG);
    FillRect(hdc, &chipR, br);
    DeleteObject(br);

    HPEN   pen  = CreatePen(PS_SOLID, 1, C::KBD_BORDER);
    HBRUSH nb   = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HPEN   op   = static_cast<HPEN>  (SelectObject(hdc, pen));
    HBRUSH ob   = static_cast<HBRUSH>(SelectObject(hdc, nb));
    RoundRect(hdc, x, y, x + pw, y + ph, 5, 5);
    SelectObject(hdc, op); SelectObject(hdc, ob);
    DeleteObject(pen);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, C::KBD_TEXT);
    TextOutW(hdc, x + 5, y + 2, key, static_cast<int>(wcslen(key)));
    SelectObject(hdc, old);
    return pw + 6;
}

// ── One shortcut row: two entries side by side ────────────
//  Returns Y after the row.
static int DrawScRow(HDC hdc,
                     const wchar_t* k1, const wchar_t* d1,
                     const wchar_t* k2, const wchar_t* d2,
                     int x, int y, int cw,
                     HFONT fontK, HFONT fontD)
{
    int kw = DrawKbd(hdc, k1, x, y, fontK);
    HFONT old = static_cast<HFONT>(SelectObject(hdc, fontD));
    SetTextColor(hdc, C::SC_LABEL);
    TextOutW(hdc, x + kw, y + 3, d1, static_cast<int>(wcslen(d1)));

    int rx = x + cw / 2;
    kw = DrawKbd(hdc, k2, rx, y, fontK);
    TextOutW(hdc, rx + kw, y + 3, d2, static_cast<int>(wcslen(d2)));

    SelectObject(hdc, old);
    return y + 22;
}

// ── Shortcuts panel (always at bottom) ───────────────────
static void DrawShortcuts(HDC hdc, int W, int H, int PAD,
                          HFONT fK, HFONT fD)
{
    const int PH  = 128;   // taller to fit 4 rows
    const int top = H - PH;
    const int cw  = W - PAD * 2;

    RECT pr { 0, top, W, H };
    HBRUSH pbr = CreateSolidBrush(C::SC_PANEL);
    FillRect(hdc, &pr, pbr);
    DeleteObject(pbr);

    HRule(hdc, PAD, W - PAD, top, C::PANEL_LINE);

    HFONT fl = MF(hdc, 9, FW_BOLD);
    HFONT ol = static_cast<HFONT>(SelectObject(hdc, fl));
    SetTextColor(hdc, RGB(58, 62, 76));
    RECT lr { PAD, top + 8, W - PAD, top + 20 };
    DrawTextW(hdc, L"SHORTCUTS", -1, &lr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, ol);
    DeleteObject(fl);

    int y = top + 22;
    y = DrawScRow(hdc,
        L"Ctrl+Space", L"Hold to record",
        L"CS+Q",       L"Quit silently",
        PAD, y, cw, fK, fD);
    y = DrawScRow(hdc,
        L"CS+F", L"Fix code error (screenshot)",
        L"CS+C", L"Clear answer",
        PAD, y, cw, fK, fD);
    y = DrawScRow(hdc,
        L"CS+O",  L"Show / hide overlay",
        L"CS+=/-", L"Opacity up / down",
        PAD, y, cw, fK, fD);
    y = DrawScRow(hdc,
        L"CA+Right/Left", L"Reposition",
        L"Scroll",        L"Scroll long answers",
        PAD, y, cw, fK, fD);
    (void)y;
}

// ────────────────────────────────────────────────────────────
//  OverlayWindow — lifecycle
// ────────────────────────────────────────────────────────────

bool OverlayWindow::s_classRegistered = false;

OverlayWindow::~OverlayWindow()
{
    if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
}

bool OverlayWindow::RegisterWindowClass()
{
    if (s_classRegistered) return true;
    WNDCLASSEXW wc   = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = CLASS_NAME;
    s_classRegistered = (RegisterClassExW(&wc) != 0);
    return s_classRegistered;
}

static void ApplyRoundRgn(HWND hwnd, int w, int h)
{
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, 14, 14);
    SetWindowRgn(hwnd, rgn, TRUE);
}

bool OverlayWindow::Create(HWND hParent, const RECT& targetRect)
{
    if (!RegisterWindowClass()) return false;
    m_bounds = targetRect;
    const int w = targetRect.right  - targetRect.left;
    const int h = targetRect.bottom - targetRect.top;

    constexpr DWORD exStyle =
        WS_EX_LAYERED  | WS_EX_TRANSPARENT |
        WS_EX_TOPMOST  | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    // WS_EX_TRANSPARENT: OS never delivers any mouse messages to this window,
    // so all clicks (including on the taskbar area) pass straight through.
    // Scroll wheel events are forwarded by a WH_MOUSE_LL hook in main.cpp.

    m_hwnd = CreateWindowExW(exStyle, CLASS_NAME, L"", WS_POPUP,
                             targetRect.left, targetRect.top, w, h,
                             hParent, nullptr,
                             GetModuleHandleW(nullptr), this);
    if (!m_hwnd) return false;

    // 60% opacity — see-through but text is readable
    SetLayeredWindowAttributes(m_hwnd, 0, 153, LWA_ALPHA);
    ApplyRoundRgn(m_hwnd, w, h);

    if (!SetWindowDisplayAffinity(m_hwnd, WDA_EXCLUDEFROMCAPTURE))
        OutputDebugStringW(L"[Overlay] WDA_EXCLUDEFROMCAPTURE unavailable\n");

    return true;
}

void OverlayWindow::Show()
{
    if (m_hwnd) { ShowWindow(m_hwnd, SW_SHOWNOACTIVATE); UpdateWindow(m_hwnd); m_visible = true; }
}
void OverlayWindow::Hide()
{
    if (m_hwnd) { ShowWindow(m_hwnd, SW_HIDE); m_visible = false; }
}

void OverlayWindow::SetPosition(const RECT& r)
{
    m_bounds = r;
    if (!m_hwnd) return;
    int w = r.right - r.left, h = r.bottom - r.top;
    SetWindowPos(m_hwnd, HWND_TOPMOST,
                 r.left, r.top, w, h,
                 SWP_NOACTIVATE);   // no SWP_SHOWWINDOW — keep current visibility
    ApplyRoundRgn(m_hwnd, w, h);
    InvalidateRect(m_hwnd, nullptr, TRUE);
    UpdateWindow(m_hwnd);
}

void OverlayWindow::SetUserName(const std::wstring& name)
{
    m_userName = name;
    Repaint();
}

// ── AI methods ────────────────────────────────────────────

void OverlayWindow::SetAIStatus(const std::wstring& status)
{
    m_aiStatus = status;
    if (m_hwnd) PostMessageW(m_hwnd, WM_USER + 2, 0, 0);
}

void OverlayWindow::SetAIAnswer(const std::wstring& question,
                                 const std::wstring& answer)
{
    m_aiQuestion = question;
    m_aiAnswer   = answer;
    m_aiStatus   = L"";
    m_scrollY    = 0;    // reset scroll on every new answer
    m_contentH   = 0;
    if (m_hwnd) PostMessageW(m_hwnd, WM_USER + 2, 0, 0);
}

void OverlayWindow::ClearAI()
{
    m_aiQuestion.clear();
    m_aiAnswer.clear();
    m_aiStatus.clear();
    Repaint();
}

void OverlayWindow::ResetScroll()
{
    m_scrollY  = 0;
    m_contentH = 0;
}

void OverlayWindow::StepOpacity(int direction)
{
    // Cycle: 40% → 60% → 80% → 95% → 40% ...
    static const BYTE levels[] = { 102, 153, 204, 242 };
    constexpr int N = 4;
    // Find current bracket
    int idx = 1; // default 60%
    for (int i = 0; i < N; ++i)
        if (m_opacity <= levels[i]) { idx = i; break; }

    idx = (idx + direction + N) % N;
    m_opacity = levels[idx];

    if (m_hwnd)
        SetLayeredWindowAttributes(m_hwnd, 0, m_opacity, LWA_ALPHA);
}

void OverlayWindow::SetListening(bool listening)
{
    if (m_listening == listening) return;
    m_listening = listening;
    if (listening) {
        m_pulseFrame = 0;
        if (m_hwnd)
            m_pulseTimer = SetTimer(m_hwnd, 1, 350, nullptr); // ~3 fps pulse
    } else {
        if (m_hwnd && m_pulseTimer) {
            KillTimer(m_hwnd, m_pulseTimer);
            m_pulseTimer = 0;
        }
        m_pulseFrame = 0;
    }
    Repaint();
}

// ────────────────────────────────────────────────────────────
//  OnPaint
// ────────────────────────────────────────────────────────────

void OverlayWindow::OnPaint()
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(m_hwnd, &ps);

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    const int W   = rc.right;
    const int H   = rc.bottom;
    const int PAD = 22;
    const int CW  = W - PAD * 2;

    // ── 1. Background ──────────────────────────────────
    HBRUSH bgBr = CreateSolidBrush(C::BG);
    FillRect(hdc, &rc, bgBr);
    DeleteObject(bgBr);
    SetBkMode(hdc, TRANSPARENT);

    // ── 2. Fonts ───────────────────────────────────────
    HFONT fBadge  = MF(hdc, 12, FW_BOLD);
    HFONT fQ      = MF(hdc, 17, FW_SEMIBOLD);
    HFONT fA      = MF(hdc, 15, FW_NORMAL);
    HFONT fKbd    = MF(hdc, 10, FW_NORMAL);
    HFONT fSc     = MF(hdc, 10, FW_NORMAL);
    HFONT fStatus = MF(hdc, 13, FW_SEMIBOLD);
    HFONT fCode   = MF(hdc, 12, FW_NORMAL, L"Consolas");
    HFONT fCodeLang = MF(hdc, 9, FW_BOLD);

    const int SC_H          = 130;
    const int CONTENT_BOTTOM= H - SC_H - 8;

    // ── 3. Header: ◆ icon + username + status ─────────
    const int ICON_CY = 28;

    // Listening pulse rings drawn BEFORE the diamond (behind it)
    if (m_listening)
        DrawListenPulse(hdc, PAD + 10, ICON_CY, m_pulseFrame);

    // Diamond changes color to bright green while recording
    DrawDiamond(hdc, PAD + 10, ICON_CY, 10,
                m_listening ? C::LISTEN_FILL : C::ICON_FILL);

    int ruleStartX = PAD + 26;
    if (!m_userName.empty()) {
        std::wstring greeting = L"  " + m_userName;
        HFONT old = static_cast<HFONT>(SelectObject(hdc, fBadge));
        SetTextColor(hdc, RGB(185, 188, 200));
        SIZE sz {};
        GetTextExtentPoint32W(hdc, greeting.c_str(),
                              static_cast<int>(greeting.size()), &sz);
        TextOutW(hdc, PAD + 24, ICON_CY - sz.cy / 2,
                 greeting.c_str(), static_cast<int>(greeting.size()));
        SelectObject(hdc, old);
        ruleStartX = PAD + 24 + sz.cx + 12;
    }

    if (!m_aiStatus.empty()) {
        HFONT old = static_cast<HFONT>(SelectObject(hdc, fStatus));
        SetTextColor(hdc, RGB(255, 185, 48));
        SIZE sz {};
        GetTextExtentPoint32W(hdc, m_aiStatus.c_str(),
                              static_cast<int>(m_aiStatus.size()), &sz);
        TextOutW(hdc, W - PAD - sz.cx, ICON_CY - sz.cy / 2,
                 m_aiStatus.c_str(), static_cast<int>(m_aiStatus.size()));
        SelectObject(hdc, old);
        HRule(hdc, ruleStartX, W - PAD - sz.cx - 12, ICON_CY, C::PANEL_LINE);
    } else {
        HRule(hdc, ruleStartX, W - PAD, ICON_CY, C::PANEL_LINE);
    }

    int y = ICON_CY + 16;

    // ── 4. Content (scrollable) ────────────────────────
    bool hasContent = !m_aiQuestion.empty() || !m_aiAnswer.empty();

    if (hasContent) {
        // Set a clip region so content can't paint over header or shortcuts
        HRGN clipRgn = CreateRectRgn(0, ICON_CY + 10, W, CONTENT_BOTTOM);
        SelectClipRgn(hdc, clipRgn);
        DeleteObject(clipRgn);

        // Shift logical origin up by scroll amount so drawing code
        // uses unmodified Y coordinates and the OS handles the offset.
        POINT prevOrg;
        SetWindowOrgEx(hdc, 0, m_scrollY, &prevOrg);

        int drawY = y;
        if (!m_aiQuestion.empty()) {
            drawY = DrawBadgeRow(hdc, L"Q  \x2014  Heard", C::Q_BADGE,
                                  PAD, drawY, CW, fBadge);
            drawY += 4;
            drawY = DrawBlock(hdc, m_aiQuestion.c_str(), C::Q_TEXT,
                               PAD, drawY, CW, fQ, drawY + 130);
            drawY += 10;
        }
        if (!m_aiAnswer.empty()) {
            drawY = DrawBadgeRow(hdc, L"A  \x2014  AI Answer", C::A_BADGE,
                                  PAD, drawY, CW, fBadge);
            drawY += 4;
            drawY = DrawMixedAnswer(hdc, m_aiAnswer,
                                     PAD, drawY, CW, fA, fCode, fCodeLang,
                                     drawY + 4000); // large limit — clip handles cutoff
        }
        // Remember content height so WM_MOUSEWHEEL can cap scrolling
        m_contentH = drawY + m_scrollY;

        // Restore origin and clear clip
        SetWindowOrgEx(hdc, prevOrg.x, prevOrg.y, nullptr);
        SelectClipRgn(hdc, nullptr);

        // Draw thin scroll bar indicator on the right if content overflows
        int visH = CONTENT_BOTTOM - (ICON_CY + 10);
        if (m_contentH > visH + 10) {
            float ratio  = (float)visH / (float)m_contentH;
            float top    = (float)m_scrollY / (float)m_contentH * visH;
            int   barTop = (ICON_CY + 10) + (int)top;
            int   barH   = std::max(20, (int)(ratio * visH));
            HBRUSH sb = CreateSolidBrush(RGB(80, 84, 104));
            RECT   sr { W - 6, barTop, W - 2, barTop + barH };
            FillRect(hdc, &sr, sb);
            DeleteObject(sb);
        }
    } else {
        // Idle: simple prompt
        RECT cr { PAD, H / 3, PAD + CW, H / 2 };
        HFONT ol = static_cast<HFONT>(SelectObject(hdc, fA));
        SetTextColor(hdc, C::IDLE);
        DrawTextW(hdc,
            m_aiStatus.empty()
                ? L"Hold  Ctrl+Space  to ask a question\nRelease to get the AI answer"
                : L"",
            -1, &cr, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(hdc, ol);
    }

    // ── 5. Shortcuts panel ─────────────────────────────
    DrawShortcuts(hdc, W, H, PAD, fKbd, fSc);

    // ── 6. Cleanup ─────────────────────────────────────
    DeleteObject(fBadge);
    DeleteObject(fQ);
    DeleteObject(fA);
    DeleteObject(fKbd);
    DeleteObject(fSc);
    DeleteObject(fStatus);
    DeleteObject(fCode);
    DeleteObject(fCodeLang);

    EndPaint(m_hwnd, &ps);
}

// ────────────────────────────────────────────────────────────
//  WndProc
// ────────────────────────────────────────────────────────────

LRESULT CALLBACK OverlayWindow::WndProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    OverlayWindow* pThis = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = reinterpret_cast<OverlayWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
    } else {
        pThis = reinterpret_cast<OverlayWindow*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    switch (msg) {
    case WM_PAINT:
        if (pThis) pThis->OnPaint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_USER + 2:
        if (pThis) { InvalidateRect(hwnd, nullptr, TRUE); UpdateWindow(hwnd); }
        return 0;
    case WM_TIMER:
        if (pThis && wParam == 1 && pThis->m_listening) {
            pThis->m_pulseFrame = (pThis->m_pulseFrame + 1) % 3;
            InvalidateRect(hwnd, nullptr, TRUE);
            UpdateWindow(hwnd);
        }
        return 0;

    case WM_MOUSEWHEEL: {
        if (!pThis) break;
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        // Positive = scroll up (decrease scrollY), negative = scroll down
        pThis->m_scrollY -= (delta / WHEEL_DELTA) * 40;
        if (pThis->m_scrollY < 0) pThis->m_scrollY = 0;
        // Cap at content height - visible area (generous ceiling)
        int maxScroll = std::max(0, pThis->m_contentH - 200);
        if (pThis->m_scrollY > maxScroll) pThis->m_scrollY = maxScroll;
        InvalidateRect(hwnd, nullptr, TRUE);
        UpdateWindow(hwnd);
        return 0;
    }
    case WM_DESTROY:
        if (pThis) {
            if (pThis->m_pulseTimer) {
                KillTimer(hwnd, pThis->m_pulseTimer);
                pThis->m_pulseTimer = 0;
            }
            pThis->m_hwnd = nullptr;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace OverlayCore
