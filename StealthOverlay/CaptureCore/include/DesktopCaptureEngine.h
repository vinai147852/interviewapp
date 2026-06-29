#pragma once
// ============================================================
//  CaptureCore / DesktopCaptureEngine.h
//
//  Captures the primary monitor via DXGI Desktop Duplication,
//  masks the overlay region with a solid colour, and delivers
//  BGRA frames to a caller-supplied callback on a background thread.
// ============================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

// Pull in D3D / DXGI libs automatically (no manual linker settings needed)
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace CaptureCore {

// ── Frame payload ─────────────────────────────────────────
/// Raw frame delivered to the caller.
/// Layout: BGRA, top-down, tightly packed (stride = width * 4).
struct FrameData
{
    std::vector<uint8_t> pixels;
    int width  = 0;
    int height = 0;
};

/// Callback signature — called on the **capture thread**.
/// The implementation should be fast: copy what you need and return.
/// Ownership of the shared_ptr may be retained to avoid an extra copy.
using FrameCallback = std::function<void(std::shared_ptr<FrameData>)>;

// ── Engine ────────────────────────────────────────────────
class DesktopCaptureEngine
{
public:
    DesktopCaptureEngine()  = default;
    ~DesktopCaptureEngine();

    DesktopCaptureEngine(const DesktopCaptureEngine&)            = delete;
    DesktopCaptureEngine& operator=(const DesktopCaptureEngine&) = delete;

    // --------------------------------------------------------
    //  Lifecycle
    // --------------------------------------------------------

    /// Create the D3D11 device and DXGI output-duplication object.
    /// Call once before Start(). Safe to call again after Stop().
    bool Initialize();

    /// Begin the capture loop on a dedicated thread.
    /// |callback|    — receives processed frames (called on capture thread)
    /// |overlayRect| — desktop-coords rectangle to mask with solid gray
    bool Start(FrameCallback callback, RECT overlayRect);

    /// Signal the capture thread to stop and block until it exits.
    void Stop();

    bool IsRunning() const { return m_running.load(std::memory_order_acquire); }

    /// Update the overlay mask rectangle at runtime (thread-safe).
    void SetOverlayRect(RECT r);

    /// Width / height of the captured output in pixels.
    int OutputWidth()  const { return m_outputWidth;  }
    int OutputHeight() const { return m_outputHeight; }

private:
    void CaptureThreadProc();

    // D3D / DXGI objects
    Microsoft::WRL::ComPtr<ID3D11Device>           m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>    m_context;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_duplication;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>        m_stagingTexture;

    // Thread management
    std::atomic<bool> m_running  { false };
    HANDLE            m_thread   = nullptr;

    // Frame delivery
    FrameCallback m_callback;

    // Overlay masking (mutex-protected for thread-safe updates)
    std::mutex m_overlayMtx;
    RECT       m_overlayRect = {};

    int  m_outputWidth  = 0;
    int  m_outputHeight = 0;
    bool m_initialized  = false;
};

} // namespace CaptureCore
