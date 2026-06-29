#pragma once
// ============================================================
//  StealthOverlayApp / AudioRecorder.h
//
//  Captures BOTH system speaker output (WASAPI loopback) and
//  microphone (waveIn) simultaneously, then mixes them into a
//  single 16 kHz / 16-bit / mono WAV for Groq Whisper.
//
//  Loopback path (ROOT-CAUSE FIX):
//    - The interviewer's audio in Google Meet / Zoom / Teams does
//      NOT always render to the system *default* endpoint. With
//      headphones, USB / Bluetooth headsets (A2DP vs hands-free),
//      or a separate "communications" default device, the audio
//      is rendered to a DIFFERENT endpoint than eConsole's default.
//      WASAPI loopback on the wrong endpoint returns packets that
//      are flagged non-silent yet carry only the noise floor
//      (RMS ~1-15) — exactly the "no speech detected" symptom.
//    - Fix: enumerate EVERY active render endpoint and run a
//      loopback capture on each one in parallel. At read time we
//      pick the endpoint that actually carried audio (highest RMS).
//      An idle endpoint produces no packets, so this is cheap and
//      self-selecting — it works regardless of which device Windows
//      decided to route the call to.
//    - Each endpoint is captured at its native mix format (usually
//      48 kHz stereo float32) WITHOUT AUDCLNT_STREAMFLAGS_
//      AUTOCONVERTPCM, then converted manually: deinterleave to
//      mono, resample to 16 kHz, float/int -> int16.
//
//  Mic path: waveIn WAVE_MAPPER, always 16 kHz mono 16-bit.
// ============================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <mmsystem.h>
#include <Audioclient.h>
#include <mmdeviceapi.h>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <string>
#include <cstdint>

class AudioRecorder
{
public:
    AudioRecorder();
    ~AudioRecorder();

    AudioRecorder(const AudioRecorder&)            = delete;
    AudioRecorder& operator=(const AudioRecorder&) = delete;

    bool Start();   ///< Returns true if at least one source opened.
    void Stop();

    bool IsRecording() const { return m_recording.load(); }
    bool HasLoopback() const { return m_loopbackActive; }
    bool HasMic()      const { return m_micActive; }

    /// Mix loopback + mic and return a RIFF/WAV file in memory.
    std::vector<uint8_t> GetWAVData();

    void ClearData();

    static constexpr DWORD SAMPLE_RATE     = 16000;
    static constexpr WORD  CHANNELS        = 1;
    static constexpr WORD  BITS_PER_SAMPLE = 16;

private:
    std::atomic<bool> m_recording{false};

    // ── WASAPI loopback: one capture stream per render endpoint ──
    //
    //  Each active render endpoint gets its own IAudioClient +
    //  IAudioCaptureClient + capture thread + PCM buffer.  At read
    //  time the loudest buffer wins (see GetWAVData).
    struct LoopbackEndpoint
    {
        IAudioClient*        client   = nullptr;
        IAudioCaptureClient* capture  = nullptr;
        std::thread          thread;

        // Native device format (used by ConvertNative)
        uint32_t sampleRate    = 48000;
        uint16_t channels      = 2;
        uint16_t bitsPerSample = 32;
        bool     isFloat       = true;

        std::wstring         name;     ///< friendly name for diagnostics
        std::mutex           mutex;
        std::vector<int16_t> pcm;      ///< already resampled to 16 kHz mono int16
    };

    std::vector<std::unique_ptr<LoopbackEndpoint>> m_loopbacks;
    bool                                           m_loopbackActive = false;

    bool StartLoopback();                       ///< enumerate + start all endpoints
    bool StartLoopbackOn(IMMDevice* pDevice,    ///< open one endpoint as loopback
                         const std::wstring& friendlyName);
    void LoopbackCaptureLoop(LoopbackEndpoint* ep);

    /// Convert one packet of native-format samples -> 16 kHz mono int16.
    static std::vector<int16_t> ConvertNative(const LoopbackEndpoint* ep,
                                              const BYTE* pData, UINT32 frames);

    static double RmsOf(const std::vector<int16_t>& pcm);
    static std::wstring DeviceFriendlyName(IMMDevice* pDevice);

    // ── waveIn mic ────────────────────────────────────────
    bool StartMic();

    static void CALLBACK WaveCallback(
        HWAVEIN, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR);
    void OnBufferDone(WAVEHDR* hdr);

    HWAVEIN m_hWaveIn   = nullptr;
    bool    m_micActive = false;

    static constexpr int    BUF_COUNT = 3;
    static constexpr SIZE_T BUF_BYTES = SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8) / 2;

    WAVEHDR           m_headers[BUF_COUNT] = {};
    std::vector<char> m_buffers[BUF_COUNT];

    std::mutex           m_micMutex;
    std::vector<int16_t> m_micPcm;
};
