// ============================================================
//  StealthOverlayApp / AgreementDialog.cpp  (v2 — bigger UI + AI keys)
//
//  Window: 860 × 720  (was 580 × 540)
//  Layout (top→bottom, all measurements in client px):
//    0–70   Header bar: ◆ icon + "StealthOverlay" + subtitle
//   70–71   Separator
//   71–80   Spacer
//   80–320  EULA text box (scrollable, read-only)
//  320–330  Spacer
//  330–580  Input fields: Name, OpenAI key, Anthropic key
//  580–620  Section labels for AI note
//  620–630  Separator
//  630–690  Buttons (Decline left, Agree right)
// ============================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <windowsx.h>
#include "AgreementDialog.h"
#include <cassert>

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

namespace App {

// ── Dark palette ─────────────────────────────────────────────
namespace DC {
    constexpr COLORREF BG          = RGB( 14,  14,  20);   // main bg
    constexpr COLORREF CARD        = RGB( 20,  22,  30);   // card/section bg
    constexpr COLORREF EDIT_BG     = RGB( 28,  30,  42);   // input bg
    constexpr COLORREF EDIT_TEXT   = RGB(220, 222, 232);   // input text
    constexpr COLORREF TITLE_TEXT  = RGB(252, 252, 255);   // heading
    constexpr COLORREF SUB_TEXT    = RGB(255, 185,  48);   // amber subtitle
    constexpr COLORREF BODY_TEXT   = RGB(165, 168, 182);   // body / labels
    constexpr COLORREF DIM_TEXT    = RGB( 95, 100, 118);   // dimmed hint
    constexpr COLORREF RULE        = RGB( 38,  42,  58);   // separator

    // Agree button — amber
    constexpr COLORREF AGREE_BG    = RGB(255, 185,  48);
    constexpr COLORREF AGREE_HOV   = RGB(230, 162,  30);
    constexpr COLORREF AGREE_TEXT  = RGB( 14,  14,  20);

    // Decline button
    constexpr COLORREF DECL_BG     = RGB( 32,  34,  48);
    constexpr COLORREF DECL_BORD   = RGB( 62,  66,  90);
    constexpr COLORREF DECL_TEXT   = RGB(155, 158, 175);
}

// ── EULA text ────────────────────────────────────────────────
static const wchar_t* EULA_TEXT =
    L"STEALTHOVERLAY \x2014 END USER LICENSE AGREEMENT\r\n"
    L"\r\n"

    L"1.  PERMITTED USE\r\n"
    L"This software is licensed for personal productivity only. You may use "
    L"it to display private notes, reference material, or live AI assistance "
    L"on your own device during your own sessions.\r\n\r\n"

    L"2.  PROHIBITED USE\r\n"
    L"You agree NOT to use StealthOverlay to gain undisclosed or unfair "
    L"advantage in:\r\n"
    L"    \x2022  Online or in-person examinations and certifications\r\n"
    L"    \x2022  Competitive gaming, esports, or ranked matches\r\n"
    L"    \x2022  Any situation where hidden assistance is prohibited by law or rules\r\n\r\n"

    L"3.  AI FEATURES & THIRD-PARTY APIs\r\n"
    L"The AI answer feature sends audio (via OpenAI Whisper) and text (via "
    L"Anthropic Claude) to third-party cloud APIs. Your API keys and audio "
    L"data are transmitted under each provider\x2019s own privacy policy. "
    L"You accept those policies by entering your keys. Usage costs are "
    L"billed to your own API accounts.\r\n\r\n"

    L"4.  NO WARRANTY\r\n"
    L"Provided \"AS IS\" without warranty of any kind. The developer is not "
    L"liable for damages of any nature arising from use or inability to use "
    L"this software or the third-party API services it connects to.\r\n\r\n"

    L"5.  PRIVACY\r\n"
    L"StealthOverlay does not operate its own server. Your name and API keys "
    L"are saved locally (stealthoverlay.ini next to the .exe). Audio is "
    L"streamed directly to OpenAI and immediately discarded locally.\r\n\r\n"

    L"6.  REDISTRIBUTION\r\n"
    L"You may not sell, sublicense, or redistribute this software without "
    L"explicit written permission from the developer.\r\n\r\n"

    L"By clicking \"Agree & Start\" you confirm that you have read, "
    L"understood, and agree to all terms above.";

// ── Static member ─────────────────────────────────────────────
bool AgreementDialog::s_classRegistered = false;

// ── Trim helper ───────────────────────────────────────────────
static std::wstring Trim(const wchar_t* src)
{
    std::wstring s(src);
    auto a = s.find_first_not_of(L" \t\r\n");
    auto z = s.find_last_not_of(L" \t\r\n");
    return (a == std::wstring::npos) ? L"" : s.substr(a, z - a + 1);
}

// ────────────────────────────────────────────────────────────
//  Show
// ────────────────────────────────────────────────────────────
AgreementResult AgreementDialog::Show()
{
    AgreementDialog dlg;
    dlg.RunModal();

    AgreementResult result;
    result.agreed = dlg.m_agreed;
    if (!dlg.m_agreed) return result;

    // Read controls before window is destroyed (RunModal destroys it)
    // Controls are gone after RunModal, so we captured strings in m_agreed path
    // We'll use a different approach: capture in OnCommand
    // Actually controls ARE destroyed after RunModal returns.
    // Fix: stash strings in the dialog object during OnCommand.
    result.userName     = dlg.m_capturedName;
    result.groqKey      = dlg.m_capturedOpenAI;
    result.anthropicKey = dlg.m_capturedAnthropic;
    result.jobRole      = dlg.m_capturedJobRole;
    result.resume       = dlg.m_capturedResume;
    result.jobDesc      = dlg.m_capturedJobDesc;
    return result;
}

// ────────────────────────────────────────────────────────────
//  RunModal
// ────────────────────────────────────────────────────────────
bool AgreementDialog::RunModal()
{
    // Register class once
    if (!s_classRegistered) {
        WNDCLASSEXW wc   = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = CLASS_NAME;
        s_classRegistered = (RegisterClassExW(&wc) != 0);
    }

    // GDI resources
    m_bgBrush    = CreateSolidBrush(DC::BG);
    m_editBrush  = CreateSolidBrush(DC::EDIT_BG);

    HDC ref   = GetDC(nullptr);
    auto pts  = [&](int p) {
        return -MulDiv(p, GetDeviceCaps(ref, LOGPIXELSY), 72);
    };
    m_fontTitle = CreateFontW(pts(22), 0,0,0, FW_BOLD,     0,0,0,
                              DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY, 0, L"Segoe UI");
    m_fontSub   = CreateFontW(pts(11), 0,0,0, FW_NORMAL,   0,0,0,
                              DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY, 0, L"Segoe UI");
    m_fontBody  = CreateFontW(pts(13), 0,0,0, FW_NORMAL,   0,0,0,
                              DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY, 0, L"Segoe UI");
    m_fontLabel = CreateFontW(pts(11), 0,0,0, FW_SEMIBOLD, 0,0,0,
                              DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY, 0, L"Segoe UI");
    m_fontBtn   = CreateFontW(pts(14), 0,0,0, FW_SEMIBOLD, 0,0,0,
                              DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY, 0, L"Segoe UI");
    ReleaseDC(nullptr, ref);

    // Create window (860×880) centred — expanded for resume/JD context fields
    constexpr int W = 860, H = 880;
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);

    m_hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        CLASS_NAME,
        L"StealthOverlay \x2014 Setup & Agreement",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        (sx - W) / 2, (sy - H) / 2, W, H,
        nullptr, nullptr,
        GetModuleHandleW(nullptr), this);

    if (!m_hwnd) return false;

    ShowWindow(m_hwnd, SW_SHOWNORMAL);
    UpdateWindow(m_hwnd);
    SetForegroundWindow(m_hwnd);

    MSG msg = {};
    while (!m_done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessage(m_hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    // Drain leftover messages
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }

    // Cleanup
    if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
    if (m_bgBrush)    { DeleteObject(m_bgBrush);    m_bgBrush    = nullptr; }
    if (m_editBrush)  { DeleteObject(m_editBrush);  m_editBrush  = nullptr; }
    if (m_fontTitle)  { DeleteObject(m_fontTitle);  m_fontTitle  = nullptr; }
    if (m_fontSub)    { DeleteObject(m_fontSub);    m_fontSub    = nullptr; }
    if (m_fontBody)   { DeleteObject(m_fontBody);   m_fontBody   = nullptr; }
    if (m_fontLabel)  { DeleteObject(m_fontLabel);  m_fontLabel  = nullptr; }
    if (m_fontBtn)    { DeleteObject(m_fontBtn);    m_fontBtn    = nullptr; }

    return m_agreed;
}

// ── Small helper: make a section label (static text) ─────────
static HWND MkLabel(HWND parent, const wchar_t* text,
                    int x, int y, int w, int h, HFONT font)
{
    HWND lbl = CreateWindowExW(0, L"STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, w, h, parent, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(lbl, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return lbl;
}

// ── Small helper: make an edit control ───────────────────────
static HWND MkEdit(HWND parent, WORD id, bool password,
                   int x, int y, int w, int h, HFONT font)
{
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL;
    if (password) style |= ES_PASSWORD;
    HWND edit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"", style,
        x, y, w, h, parent,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return edit;
}

// ── Small helper: multiline edit (scrollable textarea) ────────
static HWND MkArea(HWND parent, WORD id, const wchar_t* placeholder,
                   int x, int y, int w, int h, HFONT font)
{
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                  ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN;
    HWND edit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"", style,
        x, y, w, h, parent,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(edit, EM_SETCUEBANNER, FALSE,
                 reinterpret_cast<LPARAM>(placeholder));
    return edit;
}

// ────────────────────────────────────────────────────────────
//  OnCreate
// ────────────────────────────────────────────────────────────
void AgreementDialog::OnCreate(HWND hwnd)
{
    m_hwnd = hwnd;
    HINSTANCE hi = GetModuleHandleW(nullptr);

    // Layout constants (client coords)
    const int M  = 30;       // left/right margin
    const int IW = 860-M*2;  // inner width = 800

    // ── EULA text box  (y=82 → 300, height=218) ──────────
    m_editTerms = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT", EULA_TEXT,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        M, 82, IW, 218,
        hwnd, reinterpret_cast<HMENU>(ID_EDIT_TERMS), hi, nullptr);
    SendMessageW(m_editTerms, WM_SETFONT,
                 reinterpret_cast<WPARAM>(m_fontBody), TRUE);
    SendMessageW(m_editTerms, EM_SETSEL, 0, 0);
    SendMessageW(m_editTerms, EM_SCROLLCARET, 0, 0);

    // ─────────────────────────────────────────────────────
    //  INPUT FIELDS  (y=320 onward)
    // ─────────────────────────────────────────────────────

    // Column widths: left half | right half (with gap)
    const int COL_W = (IW - 20) / 2;   // ~390
    const int RX    = M + COL_W + 20;  // right column X

    // ── ROW 1: Your Name  |  Job Role ─────────────────────
    MkLabel(hwnd, L"YOUR NAME", M, 320, COL_W, 18, m_fontLabel);
    m_editName = MkEdit(hwnd, ID_EDIT_NAME, false,
                        M, 340, COL_W, 38, m_fontBody);
    SendMessageW(m_editName, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"e.g. Vinay"));

    MkLabel(hwnd, L"JOB ROLE  \x2014  Position you're interviewing for",
            RX, 320, COL_W, 18, m_fontLabel);
    m_editJobRole = MkEdit(hwnd, ID_EDIT_JOBROLE, false,
                           RX, 340, COL_W, 38, m_fontBody);
    SendMessageW(m_editJobRole, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"e.g. Senior SWE at Google"));

    // ── ROW 2: Groq key  |  Anthropic key ────────────────
    MkLabel(hwnd, L"GROQ API KEY  \x2014  Free STT + AI  (console.groq.com)",
            M, 396, COL_W, 18, m_fontLabel);
    m_editOpenAI = MkEdit(hwnd, ID_EDIT_OPENAI, false,
                          M, 416, COL_W, 38, m_fontBody);
    SendMessageW(m_editOpenAI, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"gsk_..."));

    MkLabel(hwnd, L"ANTHROPIC KEY  \x2014  Enables screenshot analysis",
            RX, 396, COL_W, 18, m_fontLabel);
    m_editAnthropic = MkEdit(hwnd, ID_EDIT_ANTHRO, false,
                             RX, 416, COL_W, 38, m_fontBody);
    SendMessageW(m_editAnthropic, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"sk-ant-...  (optional)"));

    // ── Key hint ──────────────────────────────────────────
    MkLabel(hwnd,
        L"Groq is FREE \x2014 get your key at console.groq.com in 60 seconds. "
        L"Keys saved locally (stealthoverlay.ini), never uploaded.",
        M, 462, IW, 28, m_fontSub);

    // ── INTERVIEW CONTEXT section ──────────────────────────
    MkLabel(hwnd, L"YOUR RESUME  \x2014  Paste plain text (AI will tailor every answer to your background)",
            M, 500, IW, 18, m_fontLabel);
    m_editResume = MkArea(hwnd, ID_EDIT_RESUME,
        L"Paste your resume here... (experience, skills, projects)",
        M, 520, IW, 90, m_fontBody);

    MkLabel(hwnd, L"JOB DESCRIPTION  \x2014  Paste the JD (AI matches your answers to the role)",
            M, 622, IW, 18, m_fontLabel);
    m_editJobDesc = MkArea(hwnd, ID_EDIT_JOBDESC,
        L"Paste the job description here... (requirements, responsibilities)",
        M, 642, IW, 90, m_fontBody);

    // ── Context tip ───────────────────────────────────────
    MkLabel(hwnd,
        L"With resume + JD, every answer is personalized to your background and the specific role \x2014 "
        L"just like top-tier interview tools (Cluely, Final Round AI) but running fully on your own keys.",
        M, 742, IW, 28, m_fontSub);

    // ── Buttons ───────────────────────────────────────────
    m_btnDecline = CreateWindowExW(
        0, L"BUTTON", L"Decline",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        M, 800, 170, 52,
        hwnd, reinterpret_cast<HMENU>(ID_BTN_DECLINE), hi, nullptr);
    SendMessageW(m_btnDecline, WM_SETFONT,
                 reinterpret_cast<WPARAM>(m_fontBtn), TRUE);

    m_btnAgree = CreateWindowExW(
        0, L"BUTTON", L"Agree \x26 Start",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        860 - M - 210, 800, 210, 52,
        hwnd, reinterpret_cast<HMENU>(ID_BTN_AGREE), hi, nullptr);
    SendMessageW(m_btnAgree, WM_SETFONT,
                 reinterpret_cast<WPARAM>(m_fontBtn), TRUE);

    SetFocus(m_editName);
}

// ────────────────────────────────────────────────────────────
//  OnCommand
// ────────────────────────────────────────────────────────────
void AgreementDialog::OnCommand(WORD id)
{
    if (id == ID_BTN_DECLINE || id == IDCANCEL) {
        m_agreed = false;
        m_done   = true;
        PostQuitMessage(0);
        return;
    }

    if (id == ID_BTN_AGREE || id == IDOK) {
        // Require non-empty name
        wchar_t nameBuf[256] = {};
        GetWindowTextW(m_editName, nameBuf, 256);
        std::wstring name = Trim(nameBuf);
        if (name.empty()) {
            MessageBoxW(m_hwnd,
                L"Please enter your name before agreeing.",
                L"Name Required", MB_ICONINFORMATION | MB_OK);
            SetFocus(m_editName);
            return;
        }

        // Capture all strings NOW — controls destroyed in RunModal
        m_capturedName = name;

        wchar_t keyBuf[512] = {};
        if (m_editJobRole) {
            GetWindowTextW(m_editJobRole, keyBuf, 512);
            m_capturedJobRole = Trim(keyBuf);
        }
        if (m_editOpenAI) {
            GetWindowTextW(m_editOpenAI, keyBuf, 512);
            m_capturedOpenAI = Trim(keyBuf);
        }
        if (m_editAnthropic) {
            GetWindowTextW(m_editAnthropic, keyBuf, 512);
            m_capturedAnthropic = Trim(keyBuf);
        }

        // Resume and JD can be large — use dynamic buffers
        if (m_editResume) {
            int len = GetWindowTextLengthW(m_editResume) + 1;
            std::wstring buf(len, L'\0');
            GetWindowTextW(m_editResume, buf.data(), len);
            m_capturedResume = buf;
        }
        if (m_editJobDesc) {
            int len = GetWindowTextLengthW(m_editJobDesc) + 1;
            std::wstring buf(len, L'\0');
            GetWindowTextW(m_editJobDesc, buf.data(), len);
            m_capturedJobDesc = buf;
        }

        m_agreed = true;
        m_done   = true;
        PostQuitMessage(0);
    }
}

// ────────────────────────────────────────────────────────────
//  OnPaint — title section painted by us; controls paint themselves
// ────────────────────────────────────────────────────────────
void AgreementDialog::OnPaint()
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(m_hwnd, &ps);
    RECT rc; GetClientRect(m_hwnd, &rc);
    const int W = rc.right;
    const int M = 30;

    // Background
    FillRect(hdc, &rc, m_bgBrush);
    SetBkMode(hdc, TRANSPARENT);

    // ── Header gradient-like top bar ──────────────────────
    RECT topBar { 0, 0, W, 72 };
    HBRUSH hb = CreateSolidBrush(RGB(18, 18, 26));
    FillRect(hdc, &topBar, hb);
    DeleteObject(hb);

    // ◆ Diamond icon
    auto diamond = [&](int cx, int cy, int r) {
        POINT pts[4] = { {cx,cy-r},{cx+r,cy},{cx,cy+r},{cx-r,cy} };
        HBRUSH b  = CreateSolidBrush(RGB(255,185,48));
        HPEN   p  = CreatePen(PS_NULL,0,0);
        auto   ob = (HBRUSH)SelectObject(hdc,b);
        auto   op = (HPEN)  SelectObject(hdc,p);
        Polygon(hdc, pts, 4);
        SelectObject(hdc,ob); SelectObject(hdc,op);
        DeleteObject(b); DeleteObject(p);
        // Inner dot
        b  = CreateSolidBrush(RGB(18,18,26));
        ob = (HBRUSH)SelectObject(hdc,b);
        p  = CreatePen(PS_NULL,0,0);
        op = (HPEN)SelectObject(hdc,p);
        Ellipse(hdc, cx-3, cy-3, cx+3, cy+3);
        SelectObject(hdc,ob); SelectObject(hdc,op);
        DeleteObject(b); DeleteObject(p);
    };
    diamond(M + 16, 36, 12);

    // App title
    HFONT old = static_cast<HFONT>(SelectObject(hdc, m_fontTitle));
    SetTextColor(hdc, DC::TITLE_TEXT);
    RECT tr { M + 38, 12, W - M, 42 };
    DrawTextW(hdc, L"StealthOverlay", -1, &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, old);

    // Subtitle
    old = static_cast<HFONT>(SelectObject(hdc, m_fontSub));
    SetTextColor(hdc, DC::SUB_TEXT);
    RECT sr { M + 38, 44, W - M, 62 };
    DrawTextW(hdc, L"License Agreement & Setup", -1, &sr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, old);

    // Separator under header
    auto hline = [&](int y, COLORREF col) {
        HPEN pen = CreatePen(PS_SOLID, 1, col);
        HPEN op  = static_cast<HPEN>(SelectObject(hdc, pen));
        MoveToEx(hdc, 0, y, nullptr);
        LineTo  (hdc, W, y);
        SelectObject(hdc, op);
        DeleteObject(pen);
    };
    hline(72, DC::RULE);

    // "TERMS OF SERVICE" micro-label above EULA box
    old = static_cast<HFONT>(SelectObject(hdc, m_fontLabel));
    SetTextColor(hdc, DC::DIM_TEXT);
    RECT lr { M, 78, W - M, 90 };
    DrawTextW(hdc, L"TERMS OF SERVICE", -1, &lr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, old);

    // Separator between context and buttons
    hline(786, DC::RULE);

    // Section labels (painted, not child controls, for clean look)
    old = static_cast<HFONT>(SelectObject(hdc, m_fontLabel));
    SetTextColor(hdc, DC::DIM_TEXT);

    RECT pl { M, 304, W / 2, 318 };
    DrawTextW(hdc, L"YOUR PROFILE",
              -1, &pl, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    RECT al { M, 380, W - M, 394 };
    DrawTextW(hdc, L"AI SETUP  \x2014  Optional but recommended",
              -1, &al, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(hdc, old);

    EndPaint(m_hwnd, &ps);
}

// ────────────────────────────────────────────────────────────
//  OnDrawItem — owner-drawn buttons
// ────────────────────────────────────────────────────────────
void AgreementDialog::OnDrawItem(DRAWITEMSTRUCT* dis)
{
    if (!dis) return;
    HDC  hdc  = dis->hDC;
    RECT rc   = dis->rcItem;
    bool sel  = (dis->itemState & ODS_SELECTED) != 0;

    SetBkMode(hdc, TRANSPARENT);

    if (dis->CtlID == ID_BTN_AGREE) {
        COLORREF bg = sel ? DC::AGREE_HOV : DC::AGREE_BG;
        HBRUSH br  = CreateSolidBrush(bg);
        HPEN   pen = CreatePen(PS_NULL, 0, 0);
        auto   ob  = static_cast<HBRUSH>(SelectObject(hdc, br));
        auto   op  = static_cast<HPEN>  (SelectObject(hdc, pen));
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 10, 10);
        SelectObject(hdc, ob); SelectObject(hdc, op);
        DeleteObject(br); DeleteObject(pen);

        SetTextColor(hdc, DC::AGREE_TEXT);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, m_fontBtn));
        DrawTextW(hdc, L"Agree \x26 Start", -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(hdc, old);
    }
    else if (dis->CtlID == ID_BTN_DECLINE) {
        COLORREF bg = sel ? RGB(44, 46, 62) : DC::DECL_BG;
        HBRUSH br  = CreateSolidBrush(bg);
        HPEN   pen = CreatePen(PS_SOLID, 1, DC::DECL_BORD);
        auto   ob  = static_cast<HBRUSH>(SelectObject(hdc, br));
        auto   op  = static_cast<HPEN>  (SelectObject(hdc, pen));
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 10, 10);
        SelectObject(hdc, ob); SelectObject(hdc, op);
        DeleteObject(br); DeleteObject(pen);

        SetTextColor(hdc, DC::DECL_TEXT);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, m_fontBtn));
        DrawTextW(hdc, L"Decline", -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(hdc, old);
    }

    if (dis->itemState & ODS_FOCUS)
        DrawFocusRect(hdc, &rc);
}

// ────────────────────────────────────────────────────────────
//  WndProc
// ────────────────────────────────────────────────────────────
LRESULT CALLBACK AgreementDialog::WndProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    AgreementDialog* pThis = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis    = reinterpret_cast<AgreementDialog*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(pThis));
    } else {
        pThis = reinterpret_cast<AgreementDialog*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    switch (msg)
    {
    case WM_CREATE:
        if (pThis) pThis->OnCreate(hwnd);
        return 0;

    case WM_PAINT:
        if (pThis) pThis->OnPaint();
        return 0;

    case WM_ERASEBKGND:
        if (pThis && pThis->m_bgBrush) {
            RECT rc; GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, pThis->m_bgBrush);
            return 1;
        }
        break;

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        if (pThis) {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, DC::EDIT_TEXT);
            SetBkColor  (hdc, DC::EDIT_BG);
            return reinterpret_cast<LRESULT>(pThis->m_editBrush);
        }
        break;

    case WM_CTLCOLORSTATIC:
        if (pThis) {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, DC::BODY_TEXT);
            SetBkColor  (hdc, DC::BG);
            return reinterpret_cast<LRESULT>(pThis->m_bgBrush);
        }
        break;

    case WM_DRAWITEM:
        if (pThis) {
            pThis->OnDrawItem(reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        }
        break;

    case WM_COMMAND:
        if (pThis) pThis->OnCommand(LOWORD(wParam));
        return 0;

    case WM_KEYDOWN:
        if (pThis) {
            if (wParam == VK_RETURN) pThis->OnCommand(ID_BTN_AGREE);
            if (wParam == VK_ESCAPE) pThis->OnCommand(ID_BTN_DECLINE);
        }
        return 0;

    case WM_CLOSE:
        if (pThis) pThis->OnCommand(ID_BTN_DECLINE);
        return 0;

    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace App
