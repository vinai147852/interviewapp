// ============================================================
//  StealthOverlayApp / main.cpp  (v5 — realtime AI only)
//
//  Flow:
//    1. AgreementDialog — collects name + Groq key
//    2. Overlay created on right half of screen
//    3. Hold Ctrl+Shift+R → record mic
//    4. Release → Whisper transcribes → LLaMA answers
//    5. Overlay shows Q + A
//
//  Hotkeys:
//    Ctrl+Space   Hold to record, release to send  ← MAIN KEY
//    CS+C         Clear current answer
//    CS+O         Toggle overlay visibility
//    CS+Q         Silent quit
//    CA+→         Right half  (default)
//    CA+←         Left half
//    CA+↑         Narrow right (~35%)
//    CA+↓         Bottom-right strip
// ============================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <wincodec.h>       // WIC — PNG encoding for screenshots
#pragma comment(lib, "windowscodecs.lib")
#include <memory>
#include <thread>
#include <atomic>
#include <string>
#include <algorithm>

#include "OverlayWindow.h"
#include "DesktopCaptureEngine.h"
#include "PreviewWindow.h"
#include "AgreementDialog.h"
#include "AudioRecorder.h"
#include "AIEngine.h"

// ── Custom thread messages ────────────────────────────────
static constexpr UINT WM_APP_START_RECORD   = WM_APP + 10;
static constexpr UINT WM_APP_STOP_RECORD    = WM_APP + 11;
static constexpr UINT WM_APP_AI_DONE        = WM_APP + 12;
static constexpr UINT WM_APP_SCREENSHOT_AI  = WM_APP + 13;
static constexpr UINT WM_APP_STT_DONE       = WM_APP + 14; // transcription ready, LLM in flight

struct AIResult {
    std::wstring question;
    std::wstring answer;
    std::wstring error;
};

// ── Hotkey IDs ────────────────────────────────────────────
enum HK : int {
    HK_TOGGLE_OVERLAY = 1,
    HK_TOGGLE_CAPTURE = 2,
    HK_CLEAR          = 3,
    HK_POS_RIGHT      = 4,
    HK_POS_LEFT       = 5,
    HK_POS_NARROW     = 6,
    HK_POS_BOTTOM     = 7,
    HK_QUIT           = 8,
    HK_CODE_ERROR     = 9,   // CS+F  — screenshot → identify & fix code error
    HK_OPACITY_UP     = 10,  // CS+=  — increase overlay opacity
    HK_OPACITY_DOWN   = 11,  // CS+-  — decrease overlay opacity
};

// ── Overlay position presets ──────────────────────────────
enum class OverlayPos { Right, Left, Narrow, Bottom };

static RECT ComputeOverlayRect(OverlayPos pos, int sw, int sh)
{
    switch (pos) {
    case OverlayPos::Right:   return { sw / 2,        0,       sw,       sh };
    case OverlayPos::Left:    return { 0,              0,  sw / 2,        sh };
    case OverlayPos::Narrow:  return { sw * 65 / 100, 0,       sw,       sh };
    case OverlayPos::Bottom:  return { sw / 2,  sh * 60 / 100, sw,       sh };
    }
    return { sw / 2, 0, sw, sh };
}

// ── Global state for keyboard hook ────────────────────────
static std::atomic<bool>  g_recording{false};
static DWORD              g_mainThreadId = 0;
static HHOOK              g_kbHook = nullptr;

// ── Global state for mouse hook (scroll forwarding) ──────
static HWND   g_overlayHwnd = nullptr;
static HHOOK  g_mouseHook   = nullptr;

// ── Low-level mouse hook ──────────────────────────────────
//  WS_EX_TRANSPARENT makes the OS skip the overlay for all mouse messages,
//  so WM_MOUSEWHEEL never arrives. This hook detects wheel events that land
//  inside the overlay rect and posts them directly to the overlay HWND.
static LRESULT CALLBACK LLMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && wParam == WM_MOUSEWHEEL && g_overlayHwnd) {
        auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        RECT  rc  = {};
        if (GetWindowRect(g_overlayHwnd, &rc) &&
            PtInRect(&rc, { ms->pt.x, ms->pt.y }))
        {
            PostMessageW(g_overlayHwnd, WM_MOUSEWHEEL,
                         ms->mouseData,
                         MAKELPARAM(ms->pt.x, ms->pt.y));
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

// ── Low-level keyboard hook (hold-to-talk) ────────────────
//  Ctrl+Space = hold to record, release to get AI answer.
static LRESULT CALLBACK LLKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION) {
        auto* kb   = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        bool  ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

        if (ctrl && kb->vkCode == VK_SPACE) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                if (!g_recording.exchange(true))
                    PostThreadMessageW(g_mainThreadId, WM_APP_START_RECORD, 0, 0);
            } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                if (g_recording.exchange(false))
                    PostThreadMessageW(g_mainThreadId, WM_APP_STOP_RECORD, 0, 0);
            }
        }
    }
    return CallNextHookEx(g_kbHook, nCode, wParam, lParam);
}

// ── Built-in default Groq key ─────────────────────────────
static const wchar_t* DEFAULT_GROQ_KEY =
    L"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";

// ── Config file helpers ───────────────────────────────────
static std::wstring GetConfigPath()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring p(exePath);
    auto pos = p.rfind(L'\\');
    return (pos != std::wstring::npos ? p.substr(0, pos + 1) : L"")
           + L"stealthoverlay.ini";
}

static void SaveConfig(const std::wstring& groqKey,
                       const std::wstring& anthropicKey,
                       const std::wstring& jobRole,
                       const std::wstring& resume,
                       const std::wstring& jobDesc)
{
    auto cfg = GetConfigPath();
    WritePrivateProfileStringW(L"API",     L"GroqKey",      groqKey.c_str(),      cfg.c_str());
    WritePrivateProfileStringW(L"API",     L"AnthropicKey", anthropicKey.c_str(), cfg.c_str());
    WritePrivateProfileStringW(L"Context", L"JobRole",      jobRole.c_str(),      cfg.c_str());
    // Resume + JD can be multi-line; WritePrivateProfileString handles them fine
    WritePrivateProfileStringW(L"Context", L"Resume",       resume.c_str(),       cfg.c_str());
    WritePrivateProfileStringW(L"Context", L"JobDesc",      jobDesc.c_str(),      cfg.c_str());
}

static std::wstring LoadConfig(const wchar_t* section, const wchar_t* key,
                                const wchar_t* defaultVal = L"")
{
    wchar_t buf[512] = {};
    GetPrivateProfileStringW(section, key, defaultVal,
                             buf, 512, GetConfigPath().c_str());
    return buf;
}

static void TryRegisterHK(int id, UINT mod, UINT vk, const wchar_t* name)
{
    if (!RegisterHotKey(nullptr, id, mod | MOD_NOREPEAT, vk)) {
        wchar_t buf[128];
        swprintf_s(buf, L"[App] Warning: could not register '%s'\n", name);
        OutputDebugStringW(buf);
    }
}

// ── Screenshot helper: captures screen as PNG bytes ──────
//  Uses GDI to grab pixels, WIC to encode PNG in memory.
//  Scales down to 1280-wide max so the Anthropic payload stays small.
//  Called from a worker thread — handles its own COM lifetime.
static std::vector<uint8_t> TakeScreenshotPng()
{
    // ── GDI capture ───────────────────────────────────────
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);

    // Scale down to ≤ 1280 px wide
    const int cw = std::min(sw, 1280);
    const int ch = static_cast<int>(static_cast<long long>(sh) * cw / sw);

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem    = CreateCompatibleDC(hdcScreen);
    HBITMAP hBmp  = CreateCompatibleBitmap(hdcScreen, cw, ch);
    HBITMAP hOld  = static_cast<HBITMAP>(SelectObject(hdcMem, hBmp));

    SetStretchBltMode(hdcMem, HALFTONE);
    StretchBlt(hdcMem, 0, 0, cw, ch, hdcScreen, 0, 0, sw, sh, SRCCOPY);

    SelectObject(hdcMem, hOld);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);

    // ── WIC encode to PNG ─────────────────────────────────
    std::vector<uint8_t> result;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // S_FALSE = already init on this thread — both cases are fine

    IWICImagingFactory* pFac = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFac))))
    {
        IWICBitmap* pBmp = nullptr;
        if (SUCCEEDED(pFac->CreateBitmapFromHBITMAP(
                hBmp, nullptr, WICBitmapIgnoreAlpha, &pBmp)))
        {
            IStream* pStream = nullptr;
            if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &pStream)))
            {
                IWICStream* pWS = nullptr;
                pFac->CreateStream(&pWS);
                if (pWS) {
                    pWS->InitializeFromIStream(pStream);

                    IWICBitmapEncoder* pEnc = nullptr;
                    pFac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &pEnc);
                    if (pEnc) {
                        pEnc->Initialize(pWS, WICBitmapEncoderNoCache);

                        IWICBitmapFrameEncode* pFrame = nullptr;
                        IPropertyBag2* pProps = nullptr;
                        if (SUCCEEDED(pEnc->CreateNewFrame(&pFrame, &pProps))) {
                            if (pProps) { pProps->Release(); pProps = nullptr; }
                            pFrame->Initialize(nullptr);
                            pFrame->SetSize(cw, ch);
                            WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
                            pFrame->SetPixelFormat(&fmt);
                            pFrame->WriteSource(pBmp, nullptr);
                            pFrame->Commit();
                            pFrame->Release();
                        }
                        pEnc->Commit();
                        pEnc->Release();
                    }
                    pWS->Release();
                }

                // Pull bytes from the in-memory IStream
                HGLOBAL hg = nullptr;
                if (SUCCEEDED(GetHGlobalFromStream(pStream, &hg)) && hg) {
                    SIZE_T sz   = GlobalSize(hg);
                    LPVOID pData = GlobalLock(hg);
                    if (pData && sz > 0)
                        result.assign(static_cast<uint8_t*>(pData),
                                      static_cast<uint8_t*>(pData) + sz);
                    GlobalUnlock(hg);
                }
                pStream->Release();
            }
            pBmp->Release();
        }
        pFac->Release();
    }

    if (hr == S_OK) CoUninitialize(); // only if we successfully init'd
    DeleteObject(hBmp);
    return result;
}

// ── wWinMain ──────────────────────────────────────────────
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    // ── DPI awareness — MUST be first, before any metric/window calls ─────
    //  Without this, Windows virtualises screen coordinates to 96 DPI.
    //  On a laptop with 125%/150% scaling (e.g. Intel Iris Xe, i7-1355U):
    //    • GetSystemMetrics(SM_CXSCREEN) would return logical pixels (~1536)
    //      instead of physical pixels (~1920), so the overlay covers the
    //      wrong region and the DXGI captured frame size mismatches.
    //  DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = Windows 10 1703+ / all Win11.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // ── Single-instance guard ─────────────────────────────
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"StealthOverlayApp_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMutex);
        return 0;   // Another instance is already running — exit silently
    }

    g_mainThreadId = GetCurrentThreadId();

    // ── Step 1: Agreement gate ────────────────────────────
    App::AgreementResult agr = App::AgreementDialog::Show();
    if (!agr.agreed) return 0;

    std::wstring groqKey = !agr.groqKey.empty()
                           ? agr.groqKey
                           : LoadConfig(L"API", L"GroqKey", DEFAULT_GROQ_KEY);
    std::wstring anthropicKey = !agr.anthropicKey.empty()
                                ? agr.anthropicKey
                                : LoadConfig(L"API", L"AnthropicKey");

    // Interview context — prefer what user just typed, else load from disk
    std::wstring jobRole = !agr.jobRole.empty()
                           ? agr.jobRole
                           : LoadConfig(L"Context", L"JobRole");
    std::wstring resume  = !agr.resume.empty()
                           ? agr.resume
                           : LoadConfig(L"Context", L"Resume");
    std::wstring jobDesc = !agr.jobDesc.empty()
                           ? agr.jobDesc
                           : LoadConfig(L"Context", L"JobDesc");

    SaveConfig(groqKey, anthropicKey, jobRole, resume, jobDesc);

    // ── Step 2: Screen geometry ───────────────────────────
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    // Use work area (excludes taskbar) so the overlay never covers it
    RECT workArea = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int sh = workArea.bottom; // top is usually 0; bottom excludes taskbar

    const RECT overlayRect = ComputeOverlayRect(OverlayPos::Right, sw, sh);
    const RECT previewRect = { 0, 0, sw / 2, sh };

    // ── Step 3: Overlay window ────────────────────────────
    auto overlay = std::make_unique<OverlayCore::OverlayWindow>();
    if (!overlay->Create(nullptr, overlayRect)) {
        MessageBoxW(nullptr,
            L"Failed to create overlay window.\n"
            L"Requires Windows 10 build 19041 or later.",
            L"StealthOverlay", MB_ICONERROR);
        return 1;
    }
    if (!agr.userName.empty())
        overlay->SetUserName(agr.userName);
    overlay->Show();

    // ── Step 4: Preview window ────────────────────────────
    auto preview = std::make_unique<App::PreviewWindow>();
    if (!preview->Create(nullptr, previewRect)) {
        MessageBoxW(nullptr, L"Failed to create preview window.",
                    L"StealthOverlay", MB_ICONERROR);
        return 1;
    }

    // ── Step 5: Capture engine (optional) ────────────────
    auto capture = std::make_unique<CaptureCore::DesktopCaptureEngine>();
    bool captureAvailable = capture->Initialize();

    // ── Step 6: AI engine ─────────────────────────────────
    auto audio = std::make_unique<AudioRecorder>();
    auto ai    = std::make_unique<AIEngine>();
    AIEngine::Config aiCfg;
    aiCfg.groqKey      = groqKey;
    aiCfg.anthropicKey = anthropicKey;
    aiCfg.jobRole      = jobRole;
    aiCfg.resume       = resume;
    aiCfg.jobDesc      = jobDesc;
    ai->SetConfig(aiCfg);

    OverlayCore::OverlayWindow* overlayPtr = overlay.get();
    AIEngine*     aiPtr    = ai.get();
    AudioRecorder* audioPtr = audio.get();

    // ── Step 7: Register hotkeys ──────────────────────────
    constexpr UINT CS = MOD_CONTROL | MOD_SHIFT;
    constexpr UINT CA = MOD_CONTROL | MOD_ALT;

    TryRegisterHK(HK_TOGGLE_OVERLAY, CS, 'O',      L"CS+O");
    TryRegisterHK(HK_TOGGLE_CAPTURE, CS, 'P',      L"CS+P");
    TryRegisterHK(HK_CLEAR,          CS, 'C',      L"CS+C");
    TryRegisterHK(HK_QUIT,           CS, 'Q',      L"CS+Q");
    TryRegisterHK(HK_CODE_ERROR,     CS, 'F',        L"CS+F");
    TryRegisterHK(HK_OPACITY_UP,     CS, VK_OEM_PLUS,  L"CS+=");   // opacity up
    TryRegisterHK(HK_OPACITY_DOWN,   CS, VK_OEM_MINUS, L"CS+-");   // opacity down
    TryRegisterHK(HK_POS_RIGHT,      CA, VK_RIGHT, L"CA+Right");
    TryRegisterHK(HK_POS_LEFT,       CA, VK_LEFT,  L"CA+Left");
    TryRegisterHK(HK_POS_NARROW,     CA, VK_UP,    L"CA+Up");
    TryRegisterHK(HK_POS_BOTTOM,     CA, VK_DOWN,  L"CA+Down");

    // ── Step 8: Low-level hooks ───────────────────────────
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, LLKeyboardProc,
                                  GetModuleHandleW(nullptr), 0);

    // Mouse hook forwards WM_MOUSEWHEEL into the overlay (which has
    // WS_EX_TRANSPARENT so the OS would otherwise swallow wheel events).
    g_overlayHwnd = overlay->GetHWND();
    g_mouseHook   = SetWindowsHookExW(WH_MOUSE_LL, LLMouseProc,
                                       GetModuleHandleW(nullptr), 0);

    // ── Step 9: Message loop ──────────────────────────────
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        // ── Hold-to-talk: START ───────────────────────────
        if (msg.message == WM_APP_START_RECORD) {
            if (ai->IsConfigured()) {
                if (audioPtr->Start()) {
                    overlayPtr->SetListening(true);
                    // Show which sources are active
                    bool lb = audioPtr->HasLoopback();
                    bool mc = audioPtr->HasMic();
                    if (lb && mc)
                        overlayPtr->SetAIStatus(L"Listening (speaker + mic)...");
                    else if (lb)
                        overlayPtr->SetAIStatus(L"Listening (speaker only)...");
                    else
                        overlayPtr->SetAIStatus(L"Listening (mic only)...");
                } else {
                    overlayPtr->SetAIStatus(L"Error: Could not open audio device");
                }
            } else {
                overlayPtr->SetAIStatus(
                    L"No API key — add Groq key to stealthoverlay.ini");
            }
            continue;
        }

        // ── Hold-to-talk: STOP + AI ───────────────────────
        if (msg.message == WM_APP_STOP_RECORD) {
            overlayPtr->SetListening(false);
            audioPtr->Stop();
            overlayPtr->SetAIStatus(L"Transcribing...");

            auto wavData = audioPtr->GetWAVData();
            std::thread([wavData = std::move(wavData),
                         aiPtr, overlayPtr, tid = g_mainThreadId]() mutable
            {
                auto* result = new AIResult{};

                // Stage 1: transcribe — show question immediately on success
                std::wstring sttErr;
                result->question = aiPtr->Transcribe(wavData, sttErr);
                if (!sttErr.empty()) {
                    result->error = L"STT: " + sttErr;
                    PostThreadMessageW(tid, WM_APP_AI_DONE,
                                       0, reinterpret_cast<LPARAM>(result));
                    return;
                }

                // Show "Q: what we heard" right away while LLM is thinking
                // WM_APP_STT_DONE: lParam = heap-allocated copy of the question wstring
                auto* q = new std::wstring(result->question);
                PostThreadMessageW(tid, WM_APP_STT_DONE,
                                   0, reinterpret_cast<LPARAM>(q));

                // Stage 2: get AI answer
                std::wstring llmErr;
                result->answer = aiPtr->GetAnswer(result->question, llmErr);
                if (!llmErr.empty()) result->error = L"LLM: " + llmErr;

                PostThreadMessageW(tid, WM_APP_AI_DONE,
                                   0, reinterpret_cast<LPARAM>(result));
            }).detach();
            continue;
        }

        // ── Transcription ready — show question, indicate LLM thinking ──
        if (msg.message == WM_APP_STT_DONE) {
            auto* q = reinterpret_cast<std::wstring*>(msg.lParam);
            if (q) {
                // Display the heard question with empty answer + "Thinking..." status
                overlayPtr->SetAIAnswer(*q, L"");
                overlayPtr->SetAIStatus(L"Thinking...");
                delete q;
            }
            continue;
        }

        // ── AI done ───────────────────────────────────────
        if (msg.message == WM_APP_AI_DONE) {
            auto* result = reinterpret_cast<AIResult*>(msg.lParam);
            if (result) {
                if (!result->error.empty()) {
                    overlayPtr->SetAIStatus(L"Error: " + result->error);
                } else {
                    overlayPtr->SetAIAnswer(result->question, result->answer);
                }
                delete result;
            }
            continue;
        }

        // ── Registered hotkeys ────────────────────────────
        if (msg.message == WM_HOTKEY) {
            switch (static_cast<HK>(msg.wParam))
            {
            case HK_QUIT:
                goto cleanup;

            case HK_CODE_ERROR: {
                // Screenshot → Anthropic vision → identify & fix code error
                overlayPtr->SetAIStatus(L"Capturing screen...");
                std::thread([aiPtr, overlayPtr, tid = g_mainThreadId]() mutable
                {
                    auto* result = new AIResult{};
                    result->question = L"[Code Error Analysis]";

                    auto pngData = TakeScreenshotPng();
                    if (pngData.empty()) {
                        result->error = L"Screenshot failed — no image captured";
                        PostThreadMessageW(tid, WM_APP_AI_DONE,
                                           0, reinterpret_cast<LPARAM>(result));
                        return;
                    }

                    // Update status (thread-safe)
                    overlayPtr->SetAIStatus(L"Analyzing code...");

                    std::wstring err;
                    result->answer = aiPtr->GetAnswerFromImage(pngData, err);
                    if (!err.empty()) result->error = err;

                    PostThreadMessageW(tid, WM_APP_AI_DONE,
                                       0, reinterpret_cast<LPARAM>(result));
                }).detach();
                break;
            }

            case HK_TOGGLE_OVERLAY:
                if (overlay->IsVisible()) overlay->Hide();
                else                      overlay->Show();
                break;

            case HK_CLEAR:
                overlay->ClearAI();
                overlay->ResetScroll();
                break;

            case HK_OPACITY_UP:
                overlay->StepOpacity(+1);
                break;

            case HK_OPACITY_DOWN:
                overlay->StepOpacity(-1);
                break;

            case HK_TOGGLE_CAPTURE:
                if (!captureAvailable) break;
                if (capture->IsRunning()) {
                    capture->Stop();
                    preview->Hide();
                } else {
                    capture->SetOverlayRect(overlay->GetBounds());
                    preview->Show();
                    capture->Start(
                        [&](std::shared_ptr<CaptureCore::FrameData> f) {
                            preview->PostFrame(std::move(f));
                        },
                        overlay->GetBounds());
                }
                break;

            case HK_POS_RIGHT: {
                RECT r = ComputeOverlayRect(OverlayPos::Right, sw, sh);
                overlay->SetPosition(r);
                if (capture->IsRunning()) capture->SetOverlayRect(r);
                break;
            }
            case HK_POS_LEFT: {
                RECT r = ComputeOverlayRect(OverlayPos::Left, sw, sh);
                overlay->SetPosition(r);
                if (capture->IsRunning()) capture->SetOverlayRect(r);
                break;
            }
            case HK_POS_NARROW: {
                RECT r = ComputeOverlayRect(OverlayPos::Narrow, sw, sh);
                overlay->SetPosition(r);
                if (capture->IsRunning()) capture->SetOverlayRect(r);
                break;
            }
            case HK_POS_BOTTOM: {
                RECT r = ComputeOverlayRect(OverlayPos::Bottom, sw, sh);
                overlay->SetPosition(r);
                if (capture->IsRunning()) capture->SetOverlayRect(r);
                break;
            }
            }
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

cleanup:
    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
    if (g_kbHook)    { UnhookWindowsHookEx(g_kbHook);    g_kbHook    = nullptr; }
    for (int id = HK_TOGGLE_OVERLAY; id <= HK_OPACITY_DOWN; ++id)
        UnregisterHotKey(nullptr, id);
    capture->Stop();
    return static_cast<int>(msg.wParam);
}
