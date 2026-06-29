#pragma once
// ============================================================
//  StealthOverlayApp / PreviewWindow.h
//
//  A normal Win32 window that displays processed BGRA frames
//  from DesktopCaptureEngine using GDI StretchDIBits.
//
//  Thread safety: PostFrame() is safe to call from the capture
//  thread; all GDI operations remain on the UI thread.
// ============================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <memory>
#include <mutex>
#include "DesktopCaptureEngine.h"   // FrameData

namespace App {

class PreviewWindow
{
public:
    PreviewWindow()  = default;
    ~PreviewWindow();

    PreviewWindow(const PreviewWindow&)            = delete;
    PreviewWindow& operator=(const PreviewWindow&) = delete;

    /// Create the preview window at |windowRect| (screen coords).
    bool Create(HWND hParent, const RECT& windowRect);

    void Show();
    void Hide();

    HWND GetHWND() const { return m_hwnd; }

    /// Called from the capture thread to push a new frame.
    /// Posts WM_NEW_FRAME to the window's message queue; actual
    /// rendering happens on the UI thread inside WM_PAINT.
    void PostFrame(std::shared_ptr<CaptureCore::FrameData> frame);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg,
                                    WPARAM wParam, LPARAM lParam);
    void OnPaint();

    HWND m_hwnd = nullptr;

    // Double-buffer: capture thread writes m_pendingFrame,
    // UI thread promotes it to m_displayFrame on paint.
    std::mutex m_frameMtx;
    std::shared_ptr<CaptureCore::FrameData> m_pendingFrame;
    std::shared_ptr<CaptureCore::FrameData> m_displayFrame;

    static constexpr const wchar_t* CLASS_NAME  = L"StealthPreviewWnd";
    static constexpr UINT           WM_NEW_FRAME = WM_USER + 1;
    static bool                     s_classRegistered;
};

} // namespace App
