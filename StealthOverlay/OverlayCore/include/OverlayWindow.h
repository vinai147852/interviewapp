#pragma once
// ============================================================
//  OverlayCore / OverlayWindow.h  (v5 — realtime AI only)
// ============================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <string>

namespace OverlayCore {

class OverlayWindow
{
public:
    OverlayWindow()  = default;
    ~OverlayWindow();

    OverlayWindow(const OverlayWindow&)            = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;

    // ── Lifecycle ─────────────────────────────────────────
    bool Create(HWND hParent, const RECT& targetRect);
    void Show();
    void Hide();

    // ── Accessors ─────────────────────────────────────────
    bool IsVisible() const { return m_visible; }
    RECT GetBounds() const { return m_bounds;  }
    HWND GetHWND()   const { return m_hwnd;    }

    // ── User identity ─────────────────────────────────────
    void SetUserName(const std::wstring& name);

    // ── Position control ──────────────────────────────────
    void SetPosition(const RECT& newRect);

    // ── AI live mode ──────────────────────────────────────

    /// Show status banner (thread-safe via PostMessage).
    void SetAIStatus(const std::wstring& status);

    /// Display transcribed question + AI answer.
    void SetAIAnswer(const std::wstring& question,
                     const std::wstring& answer);

    /// Reset back to idle / ready state.
    void ClearAI();

    /// Highlight the mic/listen icon (called on Ctrl+Space down/up).
    void SetListening(bool listening);

    /// Step opacity up or down (direction: +1 or -1).
    void StepOpacity(int direction);

    /// Reset scroll position when new answer arrives.
    void ResetScroll();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    bool RegisterWindowClass();
    void OnPaint();

    void Repaint() {
        if (m_hwnd) {
            InvalidateRect(m_hwnd, nullptr, TRUE);
            UpdateWindow(m_hwnd);
        }
    }

    HWND         m_hwnd    = nullptr;
    RECT         m_bounds  = {};
    bool         m_visible = false;
    std::wstring m_userName;
    std::wstring m_aiStatus;    ///< "Listening..." / "Thinking..." / ""
    std::wstring m_aiQuestion;  ///< Last transcribed question
    std::wstring m_aiAnswer;    ///< Last AI answer

    // Listening animation
    bool         m_listening   = false;
    int          m_pulseFrame  = 0;   ///< 0-2, cycles for ring animation
    UINT_PTR     m_pulseTimer  = 0;

    // Scroll state for long answers
    int          m_scrollY     = 0;   ///< pixels scrolled down in content area
    int          m_contentH    = 0;   ///< measured height of last rendered content
    BYTE         m_opacity     = 153; ///< current window opacity (0-255)

    static constexpr const wchar_t* CLASS_NAME = L"MediaRenderHost";
    static bool s_classRegistered;
};

} // namespace OverlayCore
