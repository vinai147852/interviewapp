// ============================================================
//  StealthOverlayApp / AudioRecorder.cpp
// ============================================================
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "Ole32.lib")

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <mmsystem.h>
#include <Audioclient.h>
#include <mmdeviceapi.h>
#include <combaseapi.h>
#include <mmreg.h>          // WAVEFORMATEXTENSIBLE
#include <functiondiscoverykeys_devpkey.h> // PKEY_Device_FriendlyName
#include "AudioRecorder.h"
#include <algorithm>
#include <cmath>
#include <cstring>

// ── Float subformat GUID (no ks.h needed) ────────────────────
// {00000003-0000-0010-8000-00AA00389B71}
static const GUID SUBFMT_FLOAT = {
    0x00000003, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}
};

// ── WAV header helpers ────────────────────────────────────────
static void W32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(uint8_t( x        & 0xFF)); v.push_back(uint8_t((x >>  8) & 0xFF));
    v.push_back(uint8_t((x >> 16) & 0xFF)); v.push_back(uint8_t((x >> 24) & 0xFF));
}
static void W16(std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back(uint8_t(x & 0xFF)); v.push_back(uint8_t((x >> 8) & 0xFF));
}

// ── Constructor / Destructor ──────────────────────────────────
AudioRecorder::AudioRecorder()
{
    for (int i = 0; i < BUF_COUNT; ++i) {
        m_buffers[i].resize(BUF_BYTES, 0);
        ZeroMemory(&m_headers[i], sizeof(WAVEHDR));
    }
}
AudioRecorder::~AudioRecorder() { Stop(); }

// ── Start ─────────────────────────────────────────────────────
bool AudioRecorder::Start()
{
    if (m_recording.load()) return true;

    // Drop endpoints from any previous session.  Their threads were
    // already joined and COM objects released by the prior Stop(), so
    // destroying the LoopbackEndpoint objects here is safe.  This must
    // happen on Start (not Stop) because GetWAVData() is called AFTER
    // Stop() and still needs the captured PCM.
    m_loopbacks.clear();
    ClearData();
    m_loopbackActive = m_micActive = false;

    // m_recording must be true BEFORE the capture threads spin up,
    // otherwise their loops exit immediately.
    m_recording.store(true);

    bool lb = StartLoopback();
    bool mc = StartMic();
    if (!lb && !mc) { m_recording.store(false); return false; }

    m_loopbackActive = lb;
    m_micActive      = mc;

    OutputDebugStringW(lb ? L"[Audio] Loopback OK\n"   : L"[Audio] Loopback FAILED\n");
    OutputDebugStringW(mc ? L"[Audio] Mic OK\n"        : L"[Audio] Mic FAILED\n");
    return true;
}

// ── Stop ──────────────────────────────────────────────────────
void AudioRecorder::Stop()
{
    if (!m_recording.exchange(false)) return;

    // Loopback: join every endpoint thread and release COM objects, but
    // KEEP the endpoint objects + their captured PCM alive — GetWAVData()
    // runs right after Stop() and reads them.  They are cleared on the
    // next Start().
    for (auto& ep : m_loopbacks) {
        if (ep->thread.joinable()) ep->thread.join();
        if (ep->capture) { ep->capture->Release(); ep->capture = nullptr; }
        if (ep->client)  { ep->client->Stop();
                           ep->client->Release();  ep->client  = nullptr; }
    }
    m_loopbackActive = false;

    if (m_micActive) {
        if (m_hWaveIn) {
            waveInStop(m_hWaveIn);
            waveInReset(m_hWaveIn);
            for (int i = 0; i < BUF_COUNT; ++i)
                waveInUnprepareHeader(m_hWaveIn, &m_headers[i], sizeof(WAVEHDR));
            waveInClose(m_hWaveIn);
            m_hWaveIn = nullptr;
        }
        m_micActive = false;
    }
}

// ── ClearData ─────────────────────────────────────────────────
void AudioRecorder::ClearData()
{
    for (auto& ep : m_loopbacks) {
        std::lock_guard<std::mutex> lk(ep->mutex);
        ep->pcm.clear();
    }
    { std::lock_guard<std::mutex> lk(m_micMutex); m_micPcm.clear(); }
}

// ── RMS helper ────────────────────────────────────────────────
double AudioRecorder::RmsOf(const std::vector<int16_t>& pcm)
{
    if (pcm.empty()) return 0.0;
    double sumSq = 0.0;
    for (int16_t s : pcm) sumSq += double(s) * double(s);
    return std::sqrt(sumSq / pcm.size());
}

// ── GetWAVData ────────────────────────────────────────────────
//
//  Picks the loopback endpoint that actually carried audio (highest
//  RMS) and mixes it with the mic.  Idle endpoints contribute nothing,
//  so this transparently follows whichever device Windows routed the
//  meeting audio to (speakers, headphones, Bluetooth, comms device…).
std::vector<uint8_t> AudioRecorder::GetWAVData()
{
    // ── Choose the loudest loopback endpoint ─────────────────
    std::vector<int16_t> lb;
    double bestRms = -1.0;

    for (auto& ep : m_loopbacks) {
        std::vector<int16_t> buf;
        { std::lock_guard<std::mutex> lk(ep->mutex); buf = ep->pcm; }
        double rms = RmsOf(buf);

        wchar_t dbg[256];
        swprintf_s(dbg, L"[Audio] Endpoint \"%s\": %zu samples, RMS=%.1f\n",
                   ep->name.c_str(), buf.size(), rms);
        OutputDebugStringW(dbg);

        if (rms > bestRms) { bestRms = rms; lb = std::move(buf); }
    }

    {
        wchar_t dbg[128];
        swprintf_s(dbg, L"[Audio] Selected loopback RMS=%.1f (%zu samples)\n",
                   bestRms < 0 ? 0.0 : bestRms, lb.size());
        OutputDebugStringW(dbg);
    }

    std::vector<int16_t> mc;
    { std::lock_guard<std::mutex> lk(m_micMutex); mc = m_micPcm; }

    const size_t len = std::max(lb.size(), mc.size());
    if (len == 0) return {};

    lb.resize(len, 0);
    mc.resize(len, 0);

    std::vector<int16_t> mixed(len);
    for (size_t i = 0; i < len; ++i) {
        int32_t s = int32_t(lb[i]) + int32_t(mc[i]);
        mixed[i] = int16_t(std::max(-32768, std::min(32767, s)));
    }

    const uint32_t dataSize = uint32_t(len * 2);
    std::vector<uint8_t> wav;
    wav.reserve(44 + dataSize);

    wav.insert(wav.end(), {'R','I','F','F'}); W32(wav, 4 + 24 + 8 + dataSize);
    wav.insert(wav.end(), {'W','A','V','E'});
    wav.insert(wav.end(), {'f','m','t',' '}); W32(wav, 16);
    W16(wav, 1); W16(wav, CHANNELS); W32(wav, SAMPLE_RATE);
    W32(wav, SAMPLE_RATE * 2); W16(wav, 2); W16(wav, 16);
    wav.insert(wav.end(), {'d','a','t','a'}); W32(wav, dataSize);
    const auto* raw = reinterpret_cast<const uint8_t*>(mixed.data());
    wav.insert(wav.end(), raw, raw + dataSize);
    return wav;
}

// ═════════════════════════════════════════════════════════════
//  WASAPI LOOPBACK — capture EVERY active render endpoint
//
//  The interviewer audio does not reliably render to the system
//  default endpoint (headphones / Bluetooth / comms device route it
//  elsewhere).  Binding loopback to one default device captured a
//  silent endpoint -> "no speech detected".  We now attach a loopback
//  capture to every active render endpoint and let GetWAVData pick the
//  one that actually carried sound.
// ═════════════════════════════════════════════════════════════

std::wstring AudioRecorder::DeviceFriendlyName(IMMDevice* pDevice)
{
    std::wstring name = L"(unknown)";
    IPropertyStore* store = nullptr;
    if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &store)) && store) {
        PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &pv))
            && pv.vt == VT_LPWSTR && pv.pwszVal) {
            name = pv.pwszVal;
        }
        PropVariantClear(&pv);
        store->Release();
    }
    return name;
}

bool AudioRecorder::StartLoopback()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;

    IMMDeviceEnumerator* pEnum = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void**>(&pEnum));
    if (FAILED(hr)) return false;

    // Enumerate ALL active render endpoints and attach loopback to each.
    IMMDeviceCollection* pColl = nullptr;
    hr = pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pColl);
    if (SUCCEEDED(hr) && pColl) {
        UINT count = 0;
        pColl->GetCount(&count);

        wchar_t dbg[64];
        swprintf_s(dbg, L"[Audio] %u active render endpoint(s)\n", count);
        OutputDebugStringW(dbg);

        for (UINT i = 0; i < count; ++i) {
            IMMDevice* pDev = nullptr;
            if (SUCCEEDED(pColl->Item(i, &pDev)) && pDev) {
                StartLoopbackOn(pDev, DeviceFriendlyName(pDev));
                pDev->Release();
            }
        }
        pColl->Release();
    }

    // Fallback: if enumeration somehow yielded nothing, try the default.
    if (m_loopbacks.empty()) {
        IMMDevice* pDev = nullptr;
        if (SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev)) && pDev) {
            StartLoopbackOn(pDev, DeviceFriendlyName(pDev));
            pDev->Release();
        }
    }

    pEnum->Release();
    return !m_loopbacks.empty();
}

bool AudioRecorder::StartLoopbackOn(IMMDevice* pDevice, const std::wstring& friendlyName)
{
    IAudioClient* pClient = nullptr;
    HRESULT hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                    nullptr, reinterpret_cast<void**>(&pClient));
    if (FAILED(hr)) return false;

    // Ask the audio engine for this endpoint's native mix format.
    WAVEFORMATEX* pFmt = nullptr;
    hr = pClient->GetMixFormat(&pFmt);
    if (FAILED(hr)) { pClient->Release(); return false; }

    auto ep = std::make_unique<LoopbackEndpoint>();
    ep->name          = friendlyName;
    ep->sampleRate    = pFmt->nSamplesPerSec;
    ep->channels      = pFmt->nChannels;
    ep->bitsPerSample = pFmt->wBitsPerSample;
    ep->isFloat       = false;

    if (pFmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        ep->isFloat = true;
    } else if (pFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* pExt = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pFmt);
        ep->isFloat = (memcmp(&pExt->SubFormat, &SUBFMT_FLOAT, sizeof(GUID)) == 0);
    }

    {
        wchar_t dbg[256];
        swprintf_s(dbg, L"[Audio] Open \"%s\": %u Hz %u ch %u-bit %s\n",
                   ep->name.c_str(), ep->sampleRate, ep->channels,
                   ep->bitsPerSample, ep->isFloat ? L"float" : L"int");
        OutputDebugStringW(dbg);
    }

    // Initialize SHARED loopback at the native format (no AUTOCONVERTPCM).
    hr = pClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        10000000LL, 0, pFmt, nullptr);

    CoTaskMemFree(pFmt);

    if (FAILED(hr)) {
        wchar_t dbg[128];
        swprintf_s(dbg, L"[Audio] Initialize \"%s\" hr=0x%08X\n",
                   ep->name.c_str(), hr);
        OutputDebugStringW(dbg);
        pClient->Release();
        return false;
    }

    IAudioCaptureClient* pCapture = nullptr;
    hr = pClient->GetService(__uuidof(IAudioCaptureClient),
                              reinterpret_cast<void**>(&pCapture));
    if (FAILED(hr)) { pClient->Release(); return false; }

    hr = pClient->Start();
    if (FAILED(hr)) { pCapture->Release(); pClient->Release(); return false; }

    ep->client  = pClient;
    ep->capture = pCapture;

    // Store the endpoint, then start its thread on the stored object.
    LoopbackEndpoint* raw = ep.get();
    m_loopbacks.push_back(std::move(ep));
    raw->thread = std::thread(&AudioRecorder::LoopbackCaptureLoop, this, raw);
    return true;
}

// ── ConvertNative: device-native PCM → 16 kHz mono int16 ─────
std::vector<int16_t> AudioRecorder::ConvertNative(const LoopbackEndpoint* ep,
                                                  const BYTE* pData, UINT32 frames)
{
    if (!ep || !pData || frames == 0) return {};

    const uint16_t ch = ep->channels;

    // ── Step 1: to mono float ─────────────────────────────
    std::vector<float> monoF(frames);

    if (ep->isFloat && ep->bitsPerSample == 32) {
        const auto* src = reinterpret_cast<const float*>(pData);
        for (UINT32 f = 0; f < frames; ++f) {
            float sum = 0;
            for (uint16_t c = 0; c < ch; ++c) sum += src[f * ch + c];
            monoF[f] = sum / ch;
        }
    } else if (!ep->isFloat && ep->bitsPerSample == 16) {
        const auto* src = reinterpret_cast<const int16_t*>(pData);
        for (UINT32 f = 0; f < frames; ++f) {
            float sum = 0;
            for (uint16_t c = 0; c < ch; ++c) sum += src[f * ch + c] / 32768.0f;
            monoF[f] = sum / ch;
        }
    } else if (!ep->isFloat && ep->bitsPerSample == 24) {
        for (UINT32 f = 0; f < frames; ++f) {
            float sum = 0;
            for (uint16_t c = 0; c < ch; ++c) {
                const uint8_t* b = pData + (f * ch + c) * 3;
                int32_t s = (int32_t(b[2]) << 16) | (int32_t(b[1]) << 8) | b[0];
                if (s & 0x800000) s |= 0xFF000000; // sign-extend
                sum += s / 8388608.0f;
            }
            monoF[f] = sum / ch;
        }
    } else if (!ep->isFloat && ep->bitsPerSample == 32) {
        const auto* src = reinterpret_cast<const int32_t*>(pData);
        for (UINT32 f = 0; f < frames; ++f) {
            float sum = 0;
            for (uint16_t c = 0; c < ch; ++c) sum += src[f * ch + c] / 2147483648.0f;
            monoF[f] = sum / ch;
        }
    } else {
        return {}; // unknown format
    }

    // ── Step 2: resample mono float → 16 000 Hz (linear) ──
    double ratio = static_cast<double>(ep->sampleRate) / 16000.0;
    size_t outCount = static_cast<size_t>(std::ceil(frames / ratio));

    std::vector<int16_t> out(outCount);
    for (size_t i = 0; i < outCount; ++i) {
        double srcIdx = i * ratio;
        size_t i0 = static_cast<size_t>(srcIdx);
        size_t i1 = std::min(i0 + 1, static_cast<size_t>(frames) - 1);
        double t  = srcIdx - i0;
        float  s  = static_cast<float>(monoF[i0] * (1.0 - t) + monoF[i1] * t);
        int32_t si = static_cast<int32_t>(s * 32767.0f);
        out[i] = static_cast<int16_t>(std::max(-32768, std::min(32767, si)));
    }
    return out;
}

// ── LoopbackCaptureLoop ───────────────────────────────────────
void AudioRecorder::LoopbackCaptureLoop(LoopbackEndpoint* ep)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    bool error = false;
    while (m_recording.load() && !error)
    {
        Sleep(20);

        UINT32 packetSize = 0;
        if (FAILED(ep->capture->GetNextPacketSize(&packetSize))) break;

        while (packetSize > 0)
        {
            BYTE*  pData  = nullptr;
            UINT32 frames = 0;
            DWORD  flags  = 0;

            if (FAILED(ep->capture->GetBuffer(&pData, &frames, &flags,
                                              nullptr, nullptr)))
            { error = true; break; }

            if (frames > 0) {
                std::lock_guard<std::mutex> lk(ep->mutex);
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    size_t silenceFrames = static_cast<size_t>(
                        std::ceil(frames * 16000.0 / ep->sampleRate));
                    ep->pcm.insert(ep->pcm.end(), silenceFrames, 0);
                } else {
                    auto converted = ConvertNative(ep, pData, frames);
                    ep->pcm.insert(ep->pcm.end(),
                                   converted.begin(), converted.end());
                }
            }

            ep->capture->ReleaseBuffer(frames);

            if (FAILED(ep->capture->GetNextPacketSize(&packetSize)))
            { error = true; break; }
        }
    }

    CoUninitialize();
}

// ═════════════════════════════════════════════════════════════
//  WAVEIN MIC PATH
// ═════════════════════════════════════════════════════════════
bool AudioRecorder::StartMic()
{
    WAVEFORMATEX wfx   = {};
    wfx.wFormatTag     = WAVE_FORMAT_PCM;
    wfx.nChannels      = CHANNELS;
    wfx.nSamplesPerSec = SAMPLE_RATE;
    wfx.wBitsPerSample = BITS_PER_SAMPLE;
    wfx.nBlockAlign    = uint16_t(CHANNELS * (BITS_PER_SAMPLE / 8));
    wfx.nAvgBytesPerSec= SAMPLE_RATE * wfx.nBlockAlign;

    MMRESULT res = waveInOpen(
        &m_hWaveIn, WAVE_MAPPER, &wfx,
        reinterpret_cast<DWORD_PTR>(WaveCallback),
        reinterpret_cast<DWORD_PTR>(this),
        CALLBACK_FUNCTION);
    if (res != MMSYSERR_NOERROR) return false;

    for (int i = 0; i < BUF_COUNT; ++i) {
        WAVEHDR& h       = m_headers[i];
        h.lpData         = m_buffers[i].data();
        h.dwBufferLength = static_cast<DWORD>(BUF_BYTES);
        h.dwFlags        = 0;
        h.dwUser         = static_cast<DWORD_PTR>(i);
        waveInPrepareHeader(m_hWaveIn, &h, sizeof(WAVEHDR));
        waveInAddBuffer    (m_hWaveIn, &h, sizeof(WAVEHDR));
    }
    waveInStart(m_hWaveIn);
    return true;
}

void CALLBACK AudioRecorder::WaveCallback(
    HWAVEIN, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR)
{
    auto* self = reinterpret_cast<AudioRecorder*>(inst);
    if (self && msg == WIM_DATA)
        self->OnBufferDone(reinterpret_cast<WAVEHDR*>(p1));
}

void AudioRecorder::OnBufferDone(WAVEHDR* hdr)
{
    if (!hdr || hdr->dwBytesRecorded == 0) return;
    {
        std::lock_guard<std::mutex> lk(m_micMutex);
        const auto* src     = reinterpret_cast<const int16_t*>(hdr->lpData);
        const size_t samples = hdr->dwBytesRecorded / sizeof(int16_t);
        m_micPcm.insert(m_micPcm.end(), src, src + samples);
    }
    if (m_recording.load() && m_hWaveIn) {
        hdr->dwFlags &= ~WHDR_DONE;
        waveInAddBuffer(m_hWaveIn, hdr, sizeof(WAVEHDR));
    }
}
