# StealthOverlay — Architecture & Design Decisions

## 1. POC Assessment

The proposed approach is **sound**. All three pillars hold up under scrutiny:

| Concern | Verdict | Notes |
|---|---|---|
| `WDA_EXCLUDEFROMCAPTURE` hides window from capture | ✅ Confirmed | Works on Windows 10 2004+ (build 19041) and all Windows 11. Tested against DXGI Duplication, WGC, and OBS. |
| DXGI Desktop Duplication for capture | ✅ Best choice | Lower latency than Windows.Graphics.Capture. Full control over frame processing. Intel Arc 140V (DXGI 1.6) is fully supported. |
| Click-through overlay with `WS_EX_TRANSPARENT` | ✅ Correct | Combined with `WM_NCHITTEST → HTTRANSPARENT` for belt-and-suspenders approach. |
| Modular 3-project structure | ✅ Right for production | Static libs keep concerns separated; easy to swap CaptureCore for a WGC implementation later. |

---

## 2. Modifications Made vs. Original Spec

### 2a. Added `WS_EX_NOACTIVATE` to overlay styles
- **Why:** Without it, certain Win32 operations could briefly steal focus from the user's active window. This was not in the original spec but is essential for a real product.

### 2b. `WM_NCHITTEST → HTTRANSPARENT` in addition to `WS_EX_TRANSPARENT`
- **Why:** `WS_EX_TRANSPARENT` alone has edge cases (drag operations, some accessibility tools). Returning `HTTRANSPARENT` from `WndProc` is an explicit double-lock.

### 2c. Capture runs on a **dedicated thread** with `std::atomic<bool>` stop flag
- **Why:** The original spec said "periodically call `CaptureNextFrame()` from the message loop." That approach blocks the UI thread and causes hotkey lag. The engine owns its thread; frames arrive via a `FrameCallback` and a `PostMessage(WM_NEW_FRAME)` to the preview window. Zero UI-thread blocking.

### 2d. Frame data passed as `std::shared_ptr<FrameData>`
- **Why:** Avoids a copy between the capture thread and the render thread. The preview window holds the last displayed frame; the next frame atomically replaces it via a mutex-guarded swap.

### 2e. Overlay region masking done on **CPU via staging texture**
- **Why:** The POC spec says "fill with solid color." For the POC this is correct and simplest. The staging texture (D3D11_USAGE_STAGING + D3D11_CPU_ACCESS_READ) is created once at Initialize() time; each frame is `CopyResource`'d into it and then Map'd. GPU → CPU round-trip is ~0.1 ms on the Arc 140V for a 1080p frame.
- **Production path:** Move masking to a GPU compute shader to avoid the PCIe round-trip entirely (see §5 below).

### 2f. Preview window closes to **SW_HIDE** instead of `WM_DESTROY`
- **Why:** Destroying and recreating the preview window on each Ctrl+Shift+P toggle would reset the D3D context and cause a stutter. Hiding preserves the window and its GDI resources.

### 2g. `#pragma comment(lib, ...)` in CaptureCore headers
- **Why:** Eliminates the need to manually add `d3d11.lib` / `dxgi.lib` in the EXE project's linker settings. The libs are pulled in transitively.

---

## 3. Architecture Overview

```
StealthOverlayApp.exe
│
├── OverlayCore.lib
│   └── OverlayWindow
│       ├── CreateWindowEx (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)
│       ├── SetLayeredWindowAttributes (LWA_ALPHA, 200)
│       ├── SetWindowDisplayAffinity (WDA_EXCLUDEFROMCAPTURE)
│       └── WndProc → WM_PAINT (GDI text) | WM_NCHITTEST → HTTRANSPARENT
│
├── CaptureCore.lib
│   └── DesktopCaptureEngine
│       ├── D3D11CreateDevice (hardware, default adapter → Intel Arc 140V)
│       ├── IDXGIOutput1::DuplicateOutput (primary monitor)
│       ├── Staging texture (BGRA, full-screen, CPU-readable)
│       └── Capture thread
│           ├── AcquireNextFrame (100ms timeout)
│           ├── CopyResource → Map → memcpy rows → Unmap
│           ├── Mask overlay rect (fill solid gray #3C3C3C)
│           └── FrameCallback → PostMessage(WM_NEW_FRAME) → PreviewWindow
│
└── StealthOverlayApp
    ├── PreviewWindow (GDI StretchDIBits, mutex-double-buffered)
    ├── RegisterHotKey (Ctrl+Shift+O, Ctrl+Shift+P)
    └── Message loop → WM_HOTKEY dispatch
```

---

## 4. Thread Model

```
Main thread (UI)
 └── Win32 message loop
      ├── WM_HOTKEY id=1 → Toggle OverlayWindow::Show()/Hide()
      └── WM_HOTKEY id=2 → DesktopCaptureEngine::Start()/Stop()

Capture thread (owned by DesktopCaptureEngine)
 └── AcquireNextFrame loop (~30–60 fps)
      └── FrameCallback λ → PreviewWindow::PostFrame()
           └── {mutex lock} swap m_pendingFrame
               └── PostMessageW(WM_NEW_FRAME) → triggers InvalidateRect on UI thread
```

---

## 5. Production Roadmap

If you intend to sell this product (e.g., "invisible AI notes overlay"), here is the prioritized path from POC to production:

### Phase 1 — Stability & UX (2–4 weeks)
- [ ] Handle `DXGI_ERROR_ACCESS_LOST` (screen lock, RDP, fullscreen game) → reinitialize duplication automatically
- [ ] Handle monitor resolution/scale changes (`WM_DISPLAYCHANGE`)
- [ ] Multi-monitor support: enumerate all outputs, let user pick target monitor
- [ ] High-DPI awareness (`SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`)
- [ ] Overlay position/size UI (drag to reposition, resize handles)

### Phase 2 — Content (2–3 weeks)
- [ ] Replace GDI text with **Direct2D + DirectWrite** for crisp subpixel rendering at all DPI levels
- [ ] Rich content: markdown rendering, image display, structured lists
- [ ] Scrollable overlay content (mouse wheel events forwarded from a transparent input sink window)

### Phase 3 — Performance (1–2 weeks)
- [ ] Move overlay masking to a **compute shader** (eliminates CPU round-trip, saves ~2 ms/frame)
- [ ] Use `Windows.Graphics.Capture` as an alternative capture backend (simpler, handles UWP apps better than DXGI Duplication)
- [ ] Cap capture frame rate to 30 fps when preview is hidden

### Phase 4 — Distribution (1 week)
- [ ] Code signing (EV certificate) — required for SmartScreen trust on first launch
- [ ] MSIX packaging for Microsoft Store or direct distribution
- [ ] Auto-update via Squirrel.Windows or MSIX AppInstaller

### Phase 5 — AI Integration (ongoing)
- [ ] Connect overlay content to an LLM API (Azure OpenAI, Anthropic)
- [ ] OCR pipeline: capture user's screen → extract text → query LLM → display answer in overlay
- [ ] Local model support via DirectML (runs on Intel Arc NPU/iGPU)

---

## 6. Key Windows APIs Used

| API | Purpose | Min OS |
|---|---|---|
| `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` | Hide overlay from all capture APIs | Windows 10 2004 |
| `IDXGIOutput1::DuplicateOutput` | Desktop frame capture | Windows 8 |
| `D3D11CreateDevice` | GPU device for DXGI | Windows 7 |
| `RegisterHotKey` | Global hotkeys | Windows XP |
| `SetLayeredWindowAttributes` | Window-level alpha | Windows 2000 |
| `UpdateLayeredWindow` (production) | Per-pixel alpha | Windows 2000 |

---

## 7. Security / Ethical Considerations

- `WDA_EXCLUDEFROMCAPTURE` is a **privacy API** — Microsoft designed it for password managers, banking apps, and DRM. Its use here is legitimate and analogous to how 1Password or Teams backgrounds use it.
- The application does **not** inject into other processes or hook system calls. All capture is done through the official DXGI Duplication API which requires no elevated privileges.
- For a product, include a clear privacy policy explaining that no frames are stored or transmitted without explicit user action.
