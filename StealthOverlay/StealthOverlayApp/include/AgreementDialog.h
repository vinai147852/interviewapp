#pragma once
// ============================================================
//  StealthOverlayApp / AgreementDialog.h  (v2 — AI keys)
//
//  Large (860×720) dark-themed modal shown on first launch.
//  Collects: user agreement, name, OpenAI key, Anthropic key.
// ============================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <string>

namespace App {

/// Everything returned after the user clicks Agree.
struct AgreementResult
{
    bool         agreed       = false;
    std::wstring userName;        ///< Display name shown in overlay header
    std::wstring groqKey;         ///< Free Groq key — used for both STT + LLM
    std::wstring anthropicKey;    ///< Optional Anthropic key (better Claude answers)
    std::wstring jobRole;         ///< Role/position being applied for
    std::wstring resume;          ///< Candidate resume text (plain text)
    std::wstring jobDesc;         ///< Job description text (plain text)
};

class AgreementDialog
{
public:
    /// Blocks until the user responds. Call from the main thread.
    static AgreementResult Show();

private:
    AgreementDialog() = default;

    bool RunModal();
    void OnCreate(HWND hwnd);
    void OnCommand(WORD id);
    void OnPaint();
    void OnDrawItem(DRAWITEMSTRUCT* dis);

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    // ── child controls ────────────────────────────────────
    HWND m_hwnd          = nullptr;
    HWND m_editTerms     = nullptr;   // read-only EULA
    HWND m_editName      = nullptr;   // display name
    HWND m_editJobRole   = nullptr;   // job role / title
    HWND m_editOpenAI    = nullptr;   // Groq API key
    HWND m_editAnthropic = nullptr;   // Anthropic API key
    HWND m_editResume    = nullptr;   // multiline resume
    HWND m_editJobDesc   = nullptr;   // multiline job description
    HWND m_btnAgree      = nullptr;   // owner-drawn amber
    HWND m_btnDecline    = nullptr;   // owner-drawn dark

    // ── state ─────────────────────────────────────────────
    bool m_done   = false;
    bool m_agreed = false;

    // ── captured field values (read in OnCommand before window closes) ──
    std::wstring m_capturedName;
    std::wstring m_capturedOpenAI;   // actually Groq key
    std::wstring m_capturedAnthropic;
    std::wstring m_capturedJobRole;
    std::wstring m_capturedResume;
    std::wstring m_capturedJobDesc;

    // ── GDI resources ─────────────────────────────────────
    HBRUSH m_bgBrush    = nullptr;   // dark window BG
    HBRUSH m_editBrush  = nullptr;   // lighter edit BG
    HFONT  m_fontTitle  = nullptr;
    HFONT  m_fontSub    = nullptr;
    HFONT  m_fontBody   = nullptr;
    HFONT  m_fontLabel  = nullptr;
    HFONT  m_fontBtn    = nullptr;

    // ── control IDs ───────────────────────────────────────
    static constexpr WORD ID_BTN_AGREE    = 101;
    static constexpr WORD ID_BTN_DECLINE  = 102;
    static constexpr WORD ID_EDIT_NAME    = 103;
    static constexpr WORD ID_EDIT_TERMS   = 104;
    static constexpr WORD ID_EDIT_OPENAI  = 105;
    static constexpr WORD ID_EDIT_ANTHRO  = 106;
    static constexpr WORD ID_EDIT_JOBROLE = 107;
    static constexpr WORD ID_EDIT_RESUME  = 108;
    static constexpr WORD ID_EDIT_JOBDESC = 109;

    static constexpr const wchar_t* CLASS_NAME = L"AppSetupDialog";
    static bool s_classRegistered;
};

} // namespace App
