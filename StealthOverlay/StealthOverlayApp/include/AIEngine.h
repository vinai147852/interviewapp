#pragma once
// ============================================================
//  StealthOverlayApp / AIEngine.h
//
//  Two-stage AI pipeline using Groq's free API:
//    Stage 1 — Transcribe():  audio WAV → text via Groq Whisper
//    Stage 2 — GetAnswer():   question text → answer via Llama 3.3 70B
//
//  Groq free tier:
//    • Whisper: 20 requests/min, 2000 requests/day  (very fast)
//    • Llama:   30 RPM, 14400 RPD                   (sub-second)
//
//  All HTTP calls use WinHTTP (built-in, no extra dependencies).
//  Falls back to Anthropic Claude if anthropic key is set and
//  Groq key is not available.
// ============================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <string>
#include <vector>
#include <utility>

class AIEngine
{
public:
    struct Config {
        std::wstring groqKey;       ///< "gsk_..."  — free at console.groq.com
        std::wstring anthropicKey;  ///< "sk-ant-..." — optional fallback / vision
        std::wstring jobRole;       ///< Role being interviewed for (personalizes answers)
        std::wstring resume;        ///< Candidate's resume text
        std::wstring jobDesc;       ///< Job description text
    };

    AIEngine();
    ~AIEngine();

    void SetConfig(const Config& cfg);
    bool IsConfigured() const;

    // ── Stage 1: Speech → Text ────────────────────────────
    /// Sends WAV data to Groq Whisper.  Returns transcribed text.
    /// outError is set if something fails (empty string on success).
    std::wstring Transcribe(const std::vector<uint8_t>& wavData,
                             std::wstring& outError);

    // ── Stage 2: Question → Answer ────────────────────────
    /// Sends question to Groq Llama (or Anthropic as fallback).
    /// Maintains context of previous Q&A for coherent answers.
    std::wstring GetAnswer(const std::wstring& question,
                            std::wstring& outError);

    // ── Stage 2b: Screenshot → Code Error Analysis ────────
    /// Sends a PNG screenshot to Anthropic vision API.
    /// Asks Claude to identify the error and show corrected code.
    /// Requires Anthropic key (Groq does not support vision).
    std::wstring GetAnswerFromImage(const std::vector<uint8_t>& pngData,
                                     std::wstring& outError);

    /// Wipe conversation history (called by Ctrl+Shift+C).
    void ClearContext();

    // ── Utility ───────────────────────────────────────────
    bool HasAnthropicKey() const { return !m_config.anthropicKey.empty(); }

    /// Append a Q&A pair to the session transcript file.
    static void AppendTranscript(const std::wstring& question,
                                  const std::wstring& answer,
                                  const std::wstring& jobRole);

    static std::string Base64Encode(const uint8_t* data, size_t len);

private:
    Config m_config;

    struct Message {
        std::string role;    // "user" or "assistant"
        std::string content;
    };
    std::vector<Message> m_history;  // last N exchanges kept for context

    static constexpr int MAX_HISTORY = 6;   // 3 turns (Q+A pairs)

    // ── Encoding helpers ──────────────────────────────────
    std::string  WideToUtf8(const std::wstring& w);
    std::wstring Utf8ToWide(const std::string& s);
    std::string  JsonEscape(const std::string& s);

    // ── Simple JSON field extractor ───────────────────────
    // Looks for "fieldName": "value" or "fieldName":"value"
    std::string ExtractJsonString(const std::string& json,
                                   const std::string& field);

    // ── WinHTTP wrappers ─────────────────────────────────
    /// POST with JSON body.  Returns response body or empty on error.
    std::string HttpsPost(const std::wstring& host,
                           const std::wstring& path,
                           const std::string&  authHeader,
                           const std::string&  extraHeaders,
                           const std::string&  body,
                           std::string&        outErr);

    /// POST multipart/form-data for audio upload.
    std::string HttpsPostMultipart(const std::wstring& host,
                                    const std::wstring& path,
                                    const std::string&  authHeader,
                                    const std::vector<uint8_t>& audioData,
                                    std::string&        outErr);
};
