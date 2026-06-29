# Loopback Audio Capture — Debugging Prompt for Claude Opus

## Context: What This App Is

A Win32/C++ stealth interview assistant overlay. When the user holds **Ctrl+Space** while an interviewer is speaking on a Google Meet/Zoom/Teams call, the app should capture that audio, send it to Groq Whisper for transcription, then send the transcript to an LLM for an AI answer. The overlay then displays the question and answer.

**The app already works correctly for microphone capture** (the user's own voice). The **only broken thing** is capturing what the interviewer says through the speakers/headphones during a Google Meet call.

---

## The Problem

> "still its not able to listen what the interviewer is asking — says 'no speech detected'. but it IS able to listen what I am speaking when I am in a Google Meet call."

- **Mic capture (waveIn)** → works, transcribes user's own voice correctly  
- **Loopback capture (WASAPI)** → captures something, but RMS energy reads as near-zero / silent even when interviewer is actively talking through Meet

---

## File Structure

```
StealthOverlay/
  StealthOverlayApp/
    include/
      AudioRecorder.h      ← audio capture class
      AIEngine.h
    src/
      AudioRecorder.cpp    ← all audio capture logic
      AIEngine.cpp         ← Transcribe() — sends WAV to Groq Whisper
      main.cpp             ← hotkey handler, calls AudioRecorder
  OverlayCore/
    src/OverlayWindow.cpp  ← Win32 overlay rendering
```

---

## What Has Been Tried (All Failed)

### Attempt 1 — WASAPI Loopback with AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM

```cpp
// Opened default eRender/eConsole endpoint as loopback
// Requested 16kHz mono 16-bit directly from Windows
WAVEFORMATEX wfx = { WAVE_FORMAT_PCM, 1, 16000, 32000, 2, 16, 0 };
hr = pClient->Initialize(
    AUDCLNT_SHAREMODE_SHARED,
    AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
        | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
    10000000LL, 0, &wfx, nullptr);
```

**Result:** Loopback initializes OK. Capture thread runs. Packets arrive with non-SILENT flag. But RMS of captured samples is < 5 out of 32767 — essentially zero amplitude. AUTOCONVERTPCM appears to inverse-scale the float→int16 conversion on this system (or the format conversion simply drops amplitude). Whisper hallucination filter rejects it.

---

### Attempt 2 — WASAPI Loopback with Native Format + Manual Conversion (current code)

```cpp
// Get device's actual mix format (usually 48kHz stereo float32)
WAVEFORMATEX* pFmt = nullptr;
pClient->GetMixFormat(&pFmt);
// m_nativeSampleRate=48000, m_nativeChannels=2, m_nativeBitsPerSample=32, m_nativeIsFloat=true

// Initialize at native format — no AUTOCONVERTPCM
hr = pClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
    AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000LL, 0, pFmt, nullptr);
```

**ConvertNative() manual conversion:**
```cpp
// Float32 stereo → mono float → resample to 16kHz → int16
const float* src = reinterpret_cast<const float*>(pData);
for (UINT32 f = 0; f < frames; ++f) {
    float sum = 0;
    for (uint16_t c = 0; c < m_nativeChannels; ++c)
        sum += src[f * m_nativeChannels + c];
    monoF[f] = sum / m_nativeChannels;
}
// Linear interpolation resample 48kHz → 16kHz
// float * 32767 → int16
```

Also tried `eCommunications` endpoint as fallback.

**Result:** Still "no speech detected". The SILENT flag is NOT set on packets (we are getting actual data packets), but after ConvertNative the resulting int16 values are still near-zero. Possible causes still unknown.

---

## Current Code State

### AudioRecorder.cpp (current — Attempt 2)

```cpp
bool AudioRecorder::StartLoopbackOn(IMMDevice* pDevice) {
    IAudioClient* pClient;
    pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &pClient);

    WAVEFORMATEX* pFmt = nullptr;
    pClient->GetMixFormat(&pFmt);
    // Stores: m_nativeSampleRate, m_nativeChannels, m_nativeBitsPerSample, m_nativeIsFloat

    pClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000LL, 0, pFmt, nullptr);
    CoTaskMemFree(pFmt);

    IAudioCaptureClient* pCapture;
    pClient->GetService(__uuidof(IAudioCaptureClient), &pCapture);
    pClient->Start();
    // Starts LoopbackCaptureLoop() thread
}

void AudioRecorder::LoopbackCaptureLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (m_recording.load()) {
        Sleep(20);
        UINT32 packetSize;
        m_pCapture->GetNextPacketSize(&packetSize);
        while (packetSize > 0) {
            BYTE* pData; UINT32 frames; DWORD flags;
            m_pCapture->GetBuffer(&pData, &frames, &flags, nullptr, nullptr);
            if (frames > 0) {
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    // write silence zeros
                } else {
                    auto converted = ConvertNative(pData, frames); // float32→mono→16kHz→int16
                    m_loopbackPcm.insert(end, converted);
                }
            }
            m_pCapture->ReleaseBuffer(frames);
        }
    }
}
```

### AIEngine.cpp — RMS check in Transcribe()

```cpp
// WAV header = 44 bytes, then int16 PCM samples
const int16_t* samples = reinterpret_cast<const int16_t*>(wavData.data() + 44);
size_t count = (wavData.size() - 44) / 2;
double sumSq = 0;
for (size_t i = 0; i < count; ++i) sumSq += (double)samples[i] * samples[i];
double rms = sqrt(sumSq / count);
// Threshold: 80.0 (out of 32767 max)
// Real speech from waveIn mic: RMS ~ 2000–8000
// Loopback after ConvertNative: RMS ~ 1–10 (near-silent despite audio playing)
if (rms < 80.0) { outError = L"No speech detected"; return {}; }
```

---

## Key Observations

1. **Mic path works perfectly.** waveIn at 16kHz mono 16-bit captures correctly. RMS values for user's speech: 1000–8000.

2. **Loopback initializes without errors.** `pClient->Initialize()` returns S_OK, `pClient->Start()` returns S_OK.

3. **Packets arrive in capture loop.** `GetNextPacketSize()` returns non-zero, `AUDCLNT_BUFFERFLAGS_SILENT` is NOT set, `frames > 0`.

4. **But samples are near-zero.** After `ConvertNative`, the resulting int16 values are in the range ±2 to ±15 even when Google Meet is playing the interviewer's voice clearly through headphones.

5. **Google Meet is a browser app (Chrome).** It routes audio through the Windows audio graph to the default playback device. WASAPI loopback on the default render endpoint SHOULD capture it.

6. **The user is likely using headphones.** When headphones are connected, Windows may set a separate audio endpoint (USB headset, Bluetooth, etc.) as the default render device. Our code tries `eConsole` then `eCommunications`.

7. **The SILENT flag behavior:** When Google Meet is playing audio, packets arrive WITHOUT the SILENT flag — so Windows knows audio is happening. But the actual sample values are still near-zero.

---

## Hypotheses Not Yet Tested

1. **Wrong endpoint entirely.** The interviewer audio plays through headphones on a device that is NOT `eConsole` OR `eCommunications`. May need to enumerate ALL render endpoints, find the one with highest peak amplitude, and attach loopback to that one.

2. **Chrome audio sandbox.** Chrome runs an audio processing pipeline in a sandboxed subprocess. The audio may be routed through an internal virtual device before hitting the Windows audio graph. WASAPI loopback at the render endpoint captures post-mixer output — this should still work, but Chrome's sandboxed audio renderer may behave differently.

3. **IAudioClient2 / IAudioClient3.** Chrome and other modern browsers may use `IAudioClient2` or `IAudioClient3` interfaces which have different sharing and routing behavior. The standard `IAudioClient` loopback may not capture from streams opened with the newer interfaces.

4. **Exclusive mode headsets.** Some USB headsets or gaming headsets operate in exclusive mode. WASAPI loopback does NOT work with exclusive mode — the endpoint is taken over by the headset driver and the Windows audio engine is bypassed.

5. **Volume is correct but rendering endpoint differs.** Enumerate all active (non-zero-peak) render endpoints and attach loopback to each, merge audio.

6. **Alternative: Windows Audio Session API (WASAPI) per-process capture.** Windows 11 22H2+ supports per-application loopback via `AUDCLNT_STREAMFLAGS_LOOPBACK_PROCESS`. This would capture only Chrome's audio regardless of endpoint.

7. **Alternative: Use Stereo Mix / What U Hear virtual device.** Some audio drivers expose a "Stereo Mix" or "What U Hear" recording input that captures all playback. This would be a regular `waveIn` or WASAPI capture on an INPUT endpoint, not a loopback.

8. **Alternative: Windows.Media.AudioGraph UWP API** — captures all system audio correctly even across endpoints on Windows 10+.

9. **Alternative: Named pipe / virtual audio cable approach** — install VB-Audio Virtual Cable or similar, route Meet audio through it, capture the virtual cable's output.

---

## What Approach to Try Next

Please analyze the hypotheses above and implement a fix. The most promising leads are:

- **Enumerate ALL render endpoints** (not just default) and try loopback on each, pick the one with actual audio energy
- **AUDCLNT_STREAMFLAGS_LOOPBACK_PROCESS** (Windows 11 only) to target Chrome specifically
- **"Stereo Mix" input device** — check if Windows exposes one and use it as a regular capture input
- **IAudioClient2** with the `EnableOffloadMode` or `SetClientProperties` for a communications stream

The constraint: **no external installs** (no VB-Cable), must work on Windows 10/11, must work for Google Meet, Zoom, Teams. C++/Win32 only, no UWP.

---

## Build Environment

- Visual Studio 2022, C++17
- Win32 API only (no MFC, no ATL, no UWP)
- Target: Windows 10 build 19041+
- Linked libs: `winmm.lib`, `Ole32.lib`, `winhttp.lib`, `windowscodecs.lib`
- Audio target format for Groq Whisper: **16 kHz, 16-bit, mono, PCM WAV**

---

## Files to Edit

- `StealthOverlay/StealthOverlayApp/include/AudioRecorder.h`
- `StealthOverlay/StealthOverlayApp/src/AudioRecorder.cpp`

The `AIEngine.cpp` RMS threshold can also be adjusted (currently `80.0` out of `32767`). The `Transcribe()` function receives a `std::vector<uint8_t>` containing the complete RIFF/WAV file.

---

## RESOLUTION (implemented)

**Root cause:** the loopback was bound to the *default* render endpoint only, and
`StartLoopbackOn` reported success the moment that endpoint *opened* — it never
actually fell back to another device when the default was silent. In a real Meet
call the interviewer's audio frequently renders to a **different endpoint** than
`eConsole`'s default: headphones, a USB/Bluetooth headset (A2DP vs hands-free),
or the separate "communications" default device. WASAPI loopback on the wrong
endpoint returns packets flagged *non-silent* that nonetheless carry only the
noise floor (RMS ~1–15) — exactly the reported symptom. Because **two unrelated
conversion paths (AUTOCONVERTPCM and manual float→int16) produced the same
near-zero result**, the captured data itself was near-silent, proving the bug
was endpoint selection, not conversion.

**Fix (`AudioRecorder.h` / `AudioRecorder.cpp`):**
- Enumerate **every active render endpoint** (`EnumAudioEndpoints(eRender,
  DEVICE_STATE_ACTIVE, …)`) and start an independent WASAPI loopback capture
  (own `IAudioClient` + `IAudioCaptureClient` + thread + PCM buffer) on each.
- Each endpoint is captured at its native mix format and converted manually
  (deinterleave→mono, linear resample→16 kHz, →int16), as before.
- `GetWAVData()` computes the RMS of every endpoint buffer, **selects the
  loudest** (the one that actually carried the meeting audio), and mixes it with
  the mic. Idle endpoints produce no packets, so they self-exclude.
- This is fully self-selecting and needs no user device configuration. It works
  regardless of whether Windows routed the call to speakers, headphones,
  Bluetooth, or the communications device. C++/Win32 only, no external installs.

**How to verify (VS Output / DebugView):** on each release of Ctrl+Space the log
now prints one line per endpoint, e.g.

```
[Audio] 3 active render endpoint(s)
[Audio] Endpoint "Speakers (Realtek)": 0 samples, RMS=0.0
[Audio] Endpoint "Headphones (Jabra)": 96000 samples, RMS=2143.7
[Audio] Endpoint "Digital Output": 0 samples, RMS=0.0
[Audio] Selected loopback RMS=2143.7 (96000 samples)
```

The "Selected loopback RMS" should now be in the hundreds-to-thousands while the
interviewer is talking. If it is still near-zero on **every** endpoint, the audio
device is in exclusive mode or muted at the OS mixer — the per-endpoint log will
make that obvious.

**Rebuild:** rebuild the `StealthOverlayApp` project (Release|x64) in
`StealthOverlay.sln`, or run `build.bat`.
