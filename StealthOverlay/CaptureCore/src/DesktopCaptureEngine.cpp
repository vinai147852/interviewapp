// ============================================================
//  CaptureCore / DesktopCaptureEngine.cpp
// ============================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "DesktopCaptureEngine.h"

#include <algorithm>   // std::clamp
#include <cassert>
#include <cstring>     // memcpy

using Microsoft::WRL::ComPtr;

namespace CaptureCore {

// ── Helpers ───────────────────────────────────────────────
static void DbgHR(HRESULT hr, const wchar_t* tag)
{
    if (FAILED(hr)) {
        wchar_t buf[128];
        swprintf_s(buf, L"[CaptureEngine] %s  hr=0x%08X\n", tag, (unsigned)hr);
        OutputDebugStringW(buf);
    }
}

// ── Destructor ────────────────────────────────────────────
DesktopCaptureEngine::~DesktopCaptureEngine()
{
    Stop();
}

// ── Initialize ────────────────────────────────────────────
bool DesktopCaptureEngine::Initialize()
{
    if (m_initialized) return true;

    // --------------------------------------------------
    // 1. Create D3D11 hardware device
    //    nullptr pAdapter = system selects the default GPU.
    //    Works with any D3D11-capable adapter including:
    //      Intel Iris Xe Graphics (i7-1355U, 12th/13th gen)
    //      Intel Arc 140V / 130V
    //      Any discrete NVIDIA/AMD GPU
    // --------------------------------------------------
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDevice(
        nullptr,                    // pAdapter  — nullptr = system default
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,                          // flags (add D3D11_CREATE_DEVICE_DEBUG for debug)
        nullptr, 0,                 // feature level array — let runtime choose
        D3D11_SDK_VERSION,
        &m_device,
        &featureLevel,
        &m_context
    );
    DbgHR(hr, L"D3D11CreateDevice");
    if (FAILED(hr)) return false;

    // --------------------------------------------------
    // 2. Walk the DXGI chain: Device → Adapter → Output
    // --------------------------------------------------
    ComPtr<IDXGIDevice> dxgiDevice;
    hr = m_device.As(&dxgiDevice);
    DbgHR(hr, L"As<IDXGIDevice>");
    if (FAILED(hr)) return false;

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    DbgHR(hr, L"GetAdapter");
    if (FAILED(hr)) return false;

    {
        DXGI_ADAPTER_DESC adDesc = {};
        adapter->GetDesc(&adDesc);
        wchar_t msg[256];
        swprintf_s(msg, L"[CaptureEngine] Using adapter: %s\n", adDesc.Description);
        OutputDebugStringW(msg);
    }

    // Output 0 = primary monitor
    ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(0, &output);
    DbgHR(hr, L"EnumOutputs(0)");
    if (FAILED(hr)) return false;

    DXGI_OUTPUT_DESC outDesc = {};
    output->GetDesc(&outDesc);
    m_outputWidth  = outDesc.DesktopCoordinates.right  - outDesc.DesktopCoordinates.left;
    m_outputHeight = outDesc.DesktopCoordinates.bottom - outDesc.DesktopCoordinates.top;

    {
        wchar_t msg[256];
        swprintf_s(msg, L"[CaptureEngine] Output resolution: %d x %d\n",
                   m_outputWidth, m_outputHeight);
        OutputDebugStringW(msg);
    }

    // --------------------------------------------------
    // 3. Create DXGI Output Duplication
    // --------------------------------------------------
    ComPtr<IDXGIOutput1> output1;
    hr = output.As(&output1);
    DbgHR(hr, L"As<IDXGIOutput1>");
    if (FAILED(hr)) return false;

    hr = output1->DuplicateOutput(m_device.Get(), &m_duplication);
    DbgHR(hr, L"DuplicateOutput");
    if (FAILED(hr)) return false;

    // --------------------------------------------------
    // 4. Create a staging (CPU-readable) texture the same
    //    size as the desktop output.
    //    We will CopyResource into this every frame, then Map it.
    // --------------------------------------------------
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width            = static_cast<UINT>(m_outputWidth);
    stagingDesc.Height           = static_cast<UINT>(m_outputHeight);
    stagingDesc.MipLevels        = 1;
    stagingDesc.ArraySize        = 1;
    stagingDesc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage            = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;

    hr = m_device->CreateTexture2D(&stagingDesc, nullptr, &m_stagingTexture);
    DbgHR(hr, L"CreateTexture2D (staging)");
    if (FAILED(hr)) return false;

    m_initialized = true;
    return true;
}

// ── Start ─────────────────────────────────────────────────
bool DesktopCaptureEngine::Start(FrameCallback callback, RECT overlayRect)
{
    if (!m_initialized || m_running.load()) return false;

    m_callback = std::move(callback);
    {
        std::lock_guard<std::mutex> lk(m_overlayMtx);
        m_overlayRect = overlayRect;
    }

    m_running.store(true, std::memory_order_release);

    // Spawn capture thread via Win32 (no CRT thread overhead)
    m_thread = CreateThread(
        nullptr, 0,
        [](LPVOID param) -> DWORD {
            reinterpret_cast<DesktopCaptureEngine*>(param)->CaptureThreadProc();
            return 0;
        },
        this, 0, nullptr);

    if (!m_thread) {
        m_running.store(false);
        return false;
    }

    return true;
}

// ── Stop ──────────────────────────────────────────────────
void DesktopCaptureEngine::Stop()
{
    m_running.store(false, std::memory_order_release);
    if (m_thread) {
        WaitForSingleObject(m_thread, 4000);
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
}

// ── SetOverlayRect ────────────────────────────────────────
void DesktopCaptureEngine::SetOverlayRect(RECT r)
{
    std::lock_guard<std::mutex> lk(m_overlayMtx);
    m_overlayRect = r;
}

// ── CaptureThreadProc ─────────────────────────────────────
//  Runs entirely on the capture thread.
void DesktopCaptureEngine::CaptureThreadProc()
{
    OutputDebugStringW(L"[CaptureEngine] Capture thread started\n");

    while (m_running.load(std::memory_order_acquire))
    {
        // --------------------------------------------------
        // Acquire the next desktop frame
        // Timeout 100 ms: if nothing changed the desktop does
        // not produce a new frame; we loop and check m_running.
        // --------------------------------------------------
        DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
        ComPtr<IDXGIResource>   deskRes;

        HRESULT hr = m_duplication->AcquireNextFrame(100, &frameInfo, &deskRes);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            continue;   // No new frame — check stop flag and retry
        }
        if (FAILED(hr)) {
            // DXGI_ERROR_ACCESS_LOST: output was reconfigured (resolution
            // change, monitor sleep, RDP connect, etc.)
            // Production code would reinitialise here; for POC we stop.
            OutputDebugStringW(
                L"[CaptureEngine] AcquireNextFrame lost — stopping. "
                L"Restart capture with Ctrl+Shift+P.\n");
            m_running.store(false);
            break;
        }

        // --------------------------------------------------
        // Get the GPU texture backing this frame
        // --------------------------------------------------
        ComPtr<ID3D11Texture2D> deskTex;
        hr = deskRes.As(&deskTex);

        if (SUCCEEDED(hr))
        {
            // Copy the entire GPU texture into our staging texture
            m_context->CopyResource(m_stagingTexture.Get(), deskTex.Get());

            // Map staging texture to CPU address space
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            hr = m_context->Map(
                m_stagingTexture.Get(), 0,
                D3D11_MAP_READ, 0, &mapped);

            if (SUCCEEDED(hr))
            {
                // Build the FrameData payload
                auto frame    = std::make_shared<FrameData>();
                frame->width  = m_outputWidth;
                frame->height = m_outputHeight;
                frame->pixels.resize(
                    static_cast<size_t>(m_outputWidth) * m_outputHeight * 4);

                // Copy row-by-row (GPU row pitch may be padded to 256 bytes)
                const auto* src = static_cast<const uint8_t*>(mapped.pData);
                      auto* dst = frame->pixels.data();
                const size_t rowBytes = static_cast<size_t>(m_outputWidth) * 4;

                for (int y = 0; y < m_outputHeight; ++y) {
                    memcpy(dst + y * rowBytes,
                           src + y * mapped.RowPitch,
                           rowBytes);
                }

                m_context->Unmap(m_stagingTexture.Get(), 0);

                // --------------------------------------------------
                // Mask overlay region: replace pixels with solid gray
                // (RGB 60,60,60) to show a "clean" desktop view.
                // Production: run a compute shader on the GPU texture
                // before CopyResource to avoid this CPU round-trip.
                // --------------------------------------------------
                RECT ov;
                {
                    std::lock_guard<std::mutex> lk(m_overlayMtx);
                    ov = m_overlayRect;
                }

                const int x0 = std::max(0, static_cast<int>(ov.left));
                const int y0 = std::max(0, static_cast<int>(ov.top));
                const int x1 = std::min(m_outputWidth,  static_cast<int>(ov.right));
                const int y1 = std::min(m_outputHeight, static_cast<int>(ov.bottom));

                for (int y = y0; y < y1; ++y) {
                    uint8_t* row = dst + y * rowBytes + x0 * 4;
                    for (int x = x0; x < x1; ++x, row += 4) {
                        row[0] = 60;    // B
                        row[1] = 60;    // G
                        row[2] = 60;    // R
                        row[3] = 255;   // A (fully opaque)
                    }
                }

                // Deliver to caller (on this capture thread)
                if (m_callback) {
                    m_callback(std::move(frame));
                }
            }
            else {
                DbgHR(hr, L"Map(staging)");
            }
        }

        // Always release the acquired frame
        m_duplication->ReleaseFrame();
    }

    OutputDebugStringW(L"[CaptureEngine] Capture thread exiting\n");
}

} // namespace CaptureCore
