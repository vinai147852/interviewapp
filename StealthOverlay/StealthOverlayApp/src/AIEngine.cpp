// ============================================================
//  StealthOverlayApp / AIEngine.cpp
//
//  Groq API endpoints (OpenAI-compatible):
//    STT:  POST api.groq.com/openai/v1/audio/transcriptions
//    LLM:  POST api.groq.com/openai/v1/chat/completions
//
//  Anthropic fallback for answers:
//    LLM:  POST api.anthropic.com/v1/messages
// ============================================================
#pragma comment(lib, "winhttp.lib")

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <winhttp.h>
#include "AIEngine.h"
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// ── Base system prompt ────────────────────────────────────────
static const char* SYSTEM_PROMPT_BASE =
    "You're a senior engineer helping a friend ace a live technical interview. "
    "Talk like a real person — clear, direct, no filler. "
    "Rules:\n"
    "- ANSWER THE SPECIFIC QUESTION ASKED — never give a generic template response\n"
    "- Answer in plain conversational English\n"
    "- Lead with the direct answer (1-2 sentences), then briefly support it\n"
    "- For coding questions: give the approach + a SHORT example (under 8 lines) in a ```lang block\n"
    "- Code must be plain ASCII only — no arrows or unicode\n"
    "- Skip 'Great question!', preambles, filler\n"
    "- Under 150 words total\n"
    "- Reference earlier answers if relevant";

// ── Build dynamic prompt with optional resume + JD context ────
static std::string BuildPrompt(const std::string& resume,
                                const std::string& jobDesc,
                                const std::string& jobRole)
{
    std::string prompt(SYSTEM_PROMPT_BASE);

    bool hasCtx = !resume.empty() || !jobDesc.empty() || !jobRole.empty();
    if (!hasCtx) return prompt;

    prompt += "\n\n--- CANDIDATE CONTEXT (use this to personalise every answer) ---";
    if (!jobRole.empty())
        prompt += "\nROLE APPLYING FOR: " + jobRole;
    if (!resume.empty())
        prompt += "\n\nRESUME:\n" + resume.substr(0, 2000); // cap at 2k chars
    if (!jobDesc.empty())
        prompt += "\n\nJOB DESCRIPTION:\n" + jobDesc.substr(0, 1500);
    prompt += "\n--- END CONTEXT ---";
    return prompt;
}

// ── Detect question type for routing prompt style ─────────────
static std::string DetectQuestionType(const std::string& q)
{
    std::string lo = q;
    for (auto& c : lo) c = (char)tolower((unsigned char)c);

    // Behavioral
    if (lo.find("tell me about yourself") != std::string::npos ||
        lo.find("describe a time")        != std::string::npos ||
        lo.find("tell me about a time")   != std::string::npos ||
        lo.find("walk me through")        != std::string::npos ||
        lo.find("greatest strength")      != std::string::npos ||
        lo.find("greatest weakness")      != std::string::npos ||
        lo.find("why do you want")        != std::string::npos ||
        lo.find("why are you interested") != std::string::npos ||
        lo.find("where do you see")       != std::string::npos)
        return "behavioral";

    // System design — require explicit "design a" / "architect a" phrasing
    // to avoid treating generic questions like "what do you think of this design?" as SD
    bool isDesignVerb = lo.find("design a ") != std::string::npos ||
                        lo.find("design an ") != std::string::npos ||
                        lo.find("design the ") != std::string::npos ||
                        lo.find("how would you design") != std::string::npos ||
                        lo.find("architect a ") != std::string::npos ||
                        lo.find("architect an ") != std::string::npos ||
                        lo.find("how to design") != std::string::npos ||
                        lo.find("system design") != std::string::npos;
    if (isDesignVerb)
        return "system_design";

    // Coding / algorithms
    if (lo.find("algorithm")  != std::string::npos ||
        lo.find("complexity") != std::string::npos ||
        lo.find("implement")  != std::string::npos ||
        lo.find("function")   != std::string::npos ||
        lo.find("array")      != std::string::npos ||
        lo.find("linked list") != std::string::npos ||
        lo.find("binary tree") != std::string::npos ||
        lo.find("dynamic prog") != std::string::npos ||
        lo.find("leetcode")   != std::string::npos)
        return "coding";

    return "general";
}

static const char* CODE_ERROR_PROMPT =
    "You're a senior dev reviewing a code snippet shown in a screenshot. "
    "Be direct and conversational — like a colleague explaining over your shoulder. "
    "Rules:\n"
    "- In 1-2 sentences, say what's wrong\n"
    "- Show the corrected code in a ```lang block — plain ASCII only\n"
    "- Keep the fixed code minimal, only show the relevant part\n"
    "- Under 120 words total";

// ── MIME boundary for multipart uploads ───────────────────────
static const std::string BOUNDARY = "----GroqAudioBoundary7f3a9b2c";

// ── Constructor / Destructor ──────────────────────────────────
AIEngine::AIEngine()  = default;
AIEngine::~AIEngine() = default;

void AIEngine::SetConfig(const Config& cfg) { m_config = cfg; }

bool AIEngine::IsConfigured() const
{
    return !m_config.groqKey.empty() || !m_config.anthropicKey.empty();
}

void AIEngine::ClearContext() { m_history.clear(); }

// ── Encoding helpers ──────────────────────────────────────────
std::string AIEngine::WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1,
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1,
                        s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring AIEngine::Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string AIEngine::JsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                sprintf_s(buf, "\\u%04x", static_cast<unsigned char>(c));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

// Simple JSON string field extractor.
// Finds: "field": "value"  or  "field":"value"
// Loops through all occurrences of "field" to skip values (e.g. "type":"text")
// and only stop when "field" is a key (followed by ':').
std::string AIEngine::ExtractJsonString(const std::string& json,
                                         const std::string& field)
{
    std::string key = "\"" + field + "\"";
    size_t pos = 0;
    while (true) {
        size_t found = json.find(key, pos);
        if (found == std::string::npos) return {};
        size_t check = found + key.size();
        while (check < json.size() && json[check] == ' ') ++check;
        if (check < json.size() && json[check] == ':') {
            pos = check + 1; // pos now points just after the ':'
            break;
        }
        pos = found + key.size(); // not a key — try next occurrence
    }
    while (pos < json.size() && json[pos] == ' ') ++pos;
    if (pos >= json.size() || json[pos] != '"') return {};
    ++pos; // skip opening quote
    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            ++pos;
            switch (json[pos]) {
            case '"':  result += '"';  break;
            case '\\': result += '\\'; break;
            case 'n':  result += '\n'; break;
            case 'r':  result += '\r'; break;
            case 't':  result += '\t'; break;
            case 'u':  {
                // Decode \uXXXX → UTF-8 so we never show raw escapes
                if (pos + 4 < json.size()) {
                    char hex[5] = {};
                    memcpy(hex, &json[pos + 1], 4);
                    unsigned int cp = (unsigned int)strtoul(hex, nullptr, 16);
                    pos += 4;
                    if (cp < 0x80) {
                        result += (char)cp;
                    } else if (cp < 0x800) {
                        result += (char)(0xC0 | (cp >> 6));
                        result += (char)(0x80 | (cp & 0x3F));
                    } else {
                        result += (char)(0xE0 | (cp >> 12));
                        result += (char)(0x80 | ((cp >> 6) & 0x3F));
                        result += (char)(0x80 | (cp & 0x3F));
                    }
                }
                break;
            }
            default:   result += json[pos]; break;
            }
        } else {
            result += json[pos];
        }
        ++pos;
    }
    return result;
}

// ── WinHTTP POST ──────────────────────────────────────────────
std::string AIEngine::HttpsPost(
    const std::wstring& host,
    const std::wstring& path,
    const std::string&  authHeader,
    const std::string&  extraHeaders,
    const std::string&  body,
    std::string&        outErr)
{
    std::string result;
    HINTERNET hSession  = nullptr;
    HINTERNET hConnect  = nullptr;
    HINTERNET hRequest  = nullptr;

    hSession = WinHttpOpen(
        L"StealthOverlay/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { outErr = "WinHttpOpen failed"; return {}; }

    // Timeouts: resolve=5s, connect=10s, send=30s, receive=30s
    WinHttpSetTimeouts(hSession, 5000, 10000, 30000, 30000);

    hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { outErr = "WinHttpConnect failed"; goto cleanup; }

    hRequest = WinHttpOpenRequest(
        hConnect, L"POST", path.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hRequest) { outErr = "WinHttpOpenRequest failed"; goto cleanup; }

    {
        // Build combined headers string
        std::string headers = authHeader;
        if (!extraHeaders.empty()) headers += "\r\n" + extraHeaders;
        headers += "\r\nContent-Type: application/json";
        std::wstring wHeaders = Utf8ToWide(headers);

        BOOL ok = WinHttpSendRequest(
            hRequest,
            wHeaders.c_str(), static_cast<DWORD>(-1L),
            const_cast<void*>(static_cast<const void*>(body.data())),
            static_cast<DWORD>(body.size()),
            static_cast<DWORD>(body.size()),
            0);

        if (!ok) { outErr = "WinHttpSendRequest failed"; goto cleanup; }
        if (!WinHttpReceiveResponse(hRequest, nullptr)) {
            outErr = "WinHttpReceiveResponse failed"; goto cleanup;
        }

        // Read response
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
            std::string chunk(avail, '\0');
            DWORD read = 0;
            WinHttpReadData(hRequest, chunk.data(), avail, &read);
            result.append(chunk.data(), read);
        }
    }

cleanup:
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);
    return result;
}

// ── WinHTTP multipart POST (for audio upload) ─────────────────
std::string AIEngine::HttpsPostMultipart(
    const std::wstring& host,
    const std::wstring& path,
    const std::string&  authHeader,
    const std::vector<uint8_t>& audioData,
    std::string&        outErr)
{
    // Build multipart body
    std::string body;
    body.reserve(audioData.size() + 512);

    // Part 1: audio file
    body += "--" + BOUNDARY + "\r\n";
    body += "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n";
    body += "Content-Type: audio/wav\r\n\r\n";
    body.append(reinterpret_cast<const char*>(audioData.data()), audioData.size());
    body += "\r\n";

    // Part 2: model
    body += "--" + BOUNDARY + "\r\n";
    body += "Content-Disposition: form-data; name=\"model\"\r\n\r\n";
    body += "whisper-large-v3-turbo\r\n";

    // Part 3: response_format
    body += "--" + BOUNDARY + "\r\n";
    body += "Content-Disposition: form-data; name=\"response_format\"\r\n\r\n";
    body += "json\r\n";

    // Part 4: language — forces English; massively reduces hallucinations
    // (without this Whisper guesses language and invents plausible-sounding garbage)
    body += "--" + BOUNDARY + "\r\n";
    body += "Content-Disposition: form-data; name=\"language\"\r\n\r\n";
    body += "en\r\n";

    // Part 5: temperature 0 — deterministic output; no creative hallucination
    body += "--" + BOUNDARY + "\r\n";
    body += "Content-Disposition: form-data; name=\"temperature\"\r\n\r\n";
    body += "0\r\n";

    // Part 6: prompt — seeds Whisper with domain vocabulary so it biases toward
    // technical interview phrases instead of generic conversational filler.
    // This is the single most effective hallucination suppressor.
    body += "--" + BOUNDARY + "\r\n";
    body += "Content-Disposition: form-data; name=\"prompt\"\r\n\r\n";
    body += "Technical software engineering interview. "
            "Questions about algorithms, data structures, system design, "
            "object-oriented programming, APIs, databases, or behavioral experience.\r\n";

    body += "--" + BOUNDARY + "--\r\n";

    // Post as if it were JSON but with multipart content-type
    std::string result;
    HINTERNET hSession  = nullptr;
    HINTERNET hConnect  = nullptr;
    HINTERNET hRequest  = nullptr;

    hSession = WinHttpOpen(L"StealthOverlay/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { outErr = "WinHttpOpen failed"; return {}; }

    // Timeouts: resolve=5s, connect=10s, send=60s (audio upload), receive=30s
    WinHttpSetTimeouts(hSession, 5000, 10000, 60000, 30000);

    hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { outErr = "WinHttpConnect failed"; goto cleanup; }

    hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) { outErr = "WinHttpOpenRequest failed"; goto cleanup; }

    {
        std::string ctHeader = "Content-Type: multipart/form-data; boundary=" + BOUNDARY;
        std::string allHeaders = authHeader + "\r\n" + ctHeader;
        std::wstring wHeaders = Utf8ToWide(allHeaders);

        BOOL ok = WinHttpSendRequest(
            hRequest,
            wHeaders.c_str(), static_cast<DWORD>(-1L),
            const_cast<void*>(static_cast<const void*>(body.data())),
            static_cast<DWORD>(body.size()),
            static_cast<DWORD>(body.size()),
            0);

        if (!ok) { outErr = "WinHttpSendRequest failed"; goto cleanup; }
        if (!WinHttpReceiveResponse(hRequest, nullptr)) {
            outErr = "WinHttpReceiveResponse failed"; goto cleanup;
        }

        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
            std::string chunk(avail, '\0');
            DWORD read = 0;
            WinHttpReadData(hRequest, chunk.data(), avail, &read);
            result.append(chunk.data(), read);
        }
    }

cleanup:
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);
    return result;
}

// ── Stage 1: Transcribe ───────────────────────────────────────
std::wstring AIEngine::Transcribe(const std::vector<uint8_t>& wavData,
                                   std::wstring& outError)
{
    if (wavData.empty()) {
        outError = L"No audio captured — hold Ctrl+Space while someone is speaking";
        return {};
    }
    if (m_config.groqKey.empty()) {
        outError = L"Groq API key not configured";
        return {};
    }

    // ── Pre-flight: RMS voice activity detection ──────────────
    // WAV header is 44 bytes; PCM samples start at byte 44 as int16_t.
    // If the RMS energy is below threshold, nobody was actually speaking —
    // sending silence to Whisper causes it to hallucinate common phrases.
    if (wavData.size() > 44) {
        const auto* samples = reinterpret_cast<const int16_t*>(wavData.data() + 44);
        const size_t count  = (wavData.size() - 44) / sizeof(int16_t);

        // RMS across all samples
        double sumSq = 0.0;
        for (size_t i = 0; i < count; ++i)
            sumSq += static_cast<double>(samples[i]) * samples[i];
        double rms = (count > 0) ? sqrt(sumSq / count) : 0.0;

        // Duration in seconds
        double durationSec = static_cast<double>(count) / 16000.0;

        OutputDebugStringA(
            ("Audio RMS=" + std::to_string((int)rms) +
             " dur=" + std::to_string(durationSec).substr(0,4) + "s\n").c_str());

        // RMS threshold: 80 out of 32767 max.
        // With native-format capture + proper float→int16 scaling, real speech
        // in a Google Meet call typically lands at RMS 800–8000. Background hum
        // without any voice is < 80. If you see "no speech detected" for audio
        // that IS present, check the VS Output window for the logged RMS value
        // and lower this number if needed.
        if (rms < 80.0) {
            outError = L"No speech detected — hold Ctrl+Space while interviewer is talking";
            return {};
        }

        // Reject recordings shorter than 0.5 s — too short to be a real question
        if (durationSec < 0.5) {
            outError = L"Recording too short — hold Ctrl+Space for the full question";
            return {};
        }
    }

    // ── Send to Groq Whisper ──────────────────────────────────
    std::string authHeader =
        "Authorization: Bearer " + WideToUtf8(m_config.groqKey);

    std::string err;
    std::string response = HttpsPostMultipart(
        L"api.groq.com",
        L"/openai/v1/audio/transcriptions",
        authHeader,
        wavData, err);

    if (!err.empty() || response.empty()) {
        outError = Utf8ToWide("Whisper network error: " + err);
        return {};
    }

    // Log raw response (VS Output window — useful for debugging)
    OutputDebugStringA(("Whisper response: " + response + "\n").c_str());

    // ── Parse response ────────────────────────────────────────
    std::string text = ExtractJsonString(response, "text");
    if (text.empty()) {
        // Groq returned an error object — show the real reason
        std::string apiErr = ExtractJsonString(response, "message");
        if (apiErr.empty()) apiErr = ExtractJsonString(response, "error");
        if (!apiErr.empty())
            outError = L"Whisper API: " + Utf8ToWide(apiErr);
        else
            outError = L"No speech detected";
        return {};
    }

    // Trim leading/trailing whitespace
    size_t ts = text.find_first_not_of(" \t\r\n");
    size_t te = text.find_last_not_of (" \t\r\n");
    if (ts == std::string::npos) {
        outError = L"No speech detected";
        return {};
    }
    text = text.substr(ts, te - ts + 1);

    // ── Hallucination filter ──────────────────────────────────
    // Even with language=en and temperature=0, Whisper still occasionally
    // generates these stock phrases when audio is noisy or too quiet.
    // Block them — they are never real interview questions.
    std::string lo = text;
    for (auto& c : lo) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    // Strip trailing punctuation for comparison
    while (!lo.empty() && (lo.back() == '.' || lo.back() == '!' || lo.back() == ','))
        lo.pop_back();

    static const char* HALLUCINATIONS[] = {
        "thank you",            "thanks for watching",  "thanks for listening",
        "you're welcome",       "your welcome",         "you are welcome",
        "go crush",             "crush it",             "good luck",
        "you got this",         "all the best",         "see you",
        "see you next time",    "goodbye",              "bye",
        "bye bye",              "you",                  "thanks",
        "okay",                 "ok",                   "hmm",
        "um",                   "uh",                   "oh",
        "yes",                  "no",                   "alright",
        nullptr
    };
    for (int i = 0; HALLUCINATIONS[i]; ++i) {
        if (lo == HALLUCINATIONS[i]) {
            OutputDebugStringA(("Hallucination filtered: [" + text + "]\n").c_str());
            outError = L"Noise/hallucination filtered — ensure interviewer audio is audible";
            return {};
        }
    }

    // Very short transcriptions (< 3 words) that aren't a known phrase
    // are almost certainly noise-triggered. Log them but pass through
    // so real short answers (e.g. "Explain REST") are not blocked.
    int wordCount = 0; bool inW = false;
    for (char c : lo) {
        if (!isspace(static_cast<unsigned char>(c))) { if (!inW) { wordCount++; inW = true; } }
        else inW = false;
    }
    if (wordCount < 2) {
        OutputDebugStringA(("Suspiciously short transcript: [" + text + "]\n").c_str());
        outError = L"Could not detect a clear question — try again";
        return {};
    }

    outError.clear();
    return Utf8ToWide(text);
}

// ── Stage 2: GetAnswer (Groq LLaMA or Anthropic fallback) ─────
std::wstring AIEngine::GetAnswer(const std::wstring& question,
                                  std::wstring& outError)
{
    std::string q = WideToUtf8(question);
    std::string answer;

    // Build the personalised system prompt (includes resume/JD if available)
    std::string sysPrompt = BuildPrompt(
        WideToUtf8(m_config.resume),
        WideToUtf8(m_config.jobDesc),
        WideToUtf8(m_config.jobRole));

    // Append question-type hint to system prompt
    std::string qType = DetectQuestionType(q);
    if (qType == "behavioral")
        sysPrompt += "\n\nBEHAVIORAL QUESTION — answer with a real, specific personal "
                     "example (STAR format, 3-4 sentences). Sound natural, not rehearsed.";
    else if (qType == "system_design")
        sysPrompt += "\n\nSYSTEM DESIGN QUESTION — answer THIS specific question directly. "
                     "Do NOT dump a generic component list. Start with the core design "
                     "decision, then mention 1-2 concrete tradeoffs relevant to what was asked.";
    else if (qType == "coding")
        sysPrompt += "\n\nCODING QUESTION — state the algorithm/approach in one sentence, "
                     "then show a short working example in a code block. No extra explanation.";

    // ── Try Groq first (Llama 3.3 70B) ───────────────────
    if (!m_config.groqKey.empty())
    {
        // Add user message to history
        m_history.push_back({"user", q});

        // Build messages array from history (last MAX_HISTORY entries)
        std::ostringstream msgs;
        msgs << "[{\"role\":\"system\",\"content\":\""
             << JsonEscape(sysPrompt) << "\"}";

        int start = std::max(0, static_cast<int>(m_history.size()) - MAX_HISTORY);
        for (int i = start; i < static_cast<int>(m_history.size()); ++i) {
            msgs << ",{\"role\":\"" << m_history[i].role
                 << "\",\"content\":\""
                 << JsonEscape(m_history[i].content) << "\"}";
        }
        msgs << "]";

        std::string body =
            "{\"model\":\"llama-3.3-70b-versatile\","
            "\"max_tokens\":256,"
            "\"temperature\":0.3,"
            "\"messages\":" + msgs.str() + "}";

        std::string authHeader =
            "Authorization: Bearer " + WideToUtf8(m_config.groqKey);

        std::string err;
        std::string response = HttpsPost(
            L"api.groq.com",
            L"/openai/v1/chat/completions",
            authHeader, "", body, err);

        if (err.empty() && !response.empty()) {
            // Response: {"choices":[{"message":{"content":"..."}},...]}
            // Extract: find "content" inside choices
            auto cidx = response.find("\"choices\"");
            if (cidx != std::string::npos) {
                std::string sub = response.substr(cidx);
                answer = ExtractJsonString(sub, "content");
            }
        }

        if (!answer.empty()) {
            m_history.push_back({"assistant", answer});
            outError.clear();
            std::wstring ans = Utf8ToWide(answer);
            AppendTranscript(question, ans, m_config.jobRole);
            return ans;
        }
        // Groq failed — fall through to Anthropic
        OutputDebugStringW(L"[AIEngine] Groq LLM failed, trying Anthropic\n");
    }

    // ── Anthropic Claude fallback ────────────────────────
    if (!m_config.anthropicKey.empty())
    {
        std::ostringstream msgs;
        msgs << "[";
        bool first = true;
        int start = std::max(0, static_cast<int>(m_history.size()) - MAX_HISTORY);
        for (int i = start; i < static_cast<int>(m_history.size()); ++i) {
            if (!first) msgs << ",";
            msgs << "{\"role\":\"" << m_history[i].role
                 << "\",\"content\":\""
                 << JsonEscape(m_history[i].content) << "\"}";
            first = false;
        }
        // Append current question if not already in history
        if (m_history.empty() || m_history.back().content != q) {
            if (!first) msgs << ",";
            msgs << "{\"role\":\"user\",\"content\":\""
                 << JsonEscape(q) << "\"}";
        }
        msgs << "]";

        std::string body =
            "{\"model\":\"claude-haiku-4-5-20251001\","
            "\"max_tokens\":256,"
            "\"system\":\"" + JsonEscape(sysPrompt) + "\","
            "\"messages\":" + msgs.str() + "}";

        std::string authHeader =
            "x-api-key: " + WideToUtf8(m_config.anthropicKey);
        std::string extra =
            "anthropic-version: 2023-06-01";

        std::string err;
        std::string response = HttpsPost(
            L"api.anthropic.com",
            L"/v1/messages",
            authHeader, extra, body, err);

        if (!err.empty() || response.empty()) {
            outError = Utf8ToWide("Anthropic error: " + err);
            return {};
        }

        // Response: {"content":[{"type":"text","text":"..."}],...}
        auto tidx = response.find("\"text\"");
        if (tidx != std::string::npos) {
            answer = ExtractJsonString(response.substr(tidx), "text");
        }

        if (!answer.empty()) {
            if (m_history.empty() || m_history.back().role != "user")
                m_history.push_back({"user", q});
            m_history.push_back({"assistant", answer});
            outError.clear();
            std::wstring ans = Utf8ToWide(answer);
            AppendTranscript(question, ans, m_config.jobRole);
            return ans;
        }

        outError = L"Could not parse Anthropic response";
        OutputDebugStringA(("Anthropic raw: " + response + "\n").c_str());
        return {};
    }

    outError = L"No AI keys configured. Enter a Groq key in settings.";
    return {};
}

// ── Session transcript ────────────────────────────────────────
//  Appends a Q&A pair to sessions\session_YYYYMMDD_HHmm.txt
//  located next to the running .exe.  Creates the file + folder
//  on first call.  Call is cheap (just file append).
void AIEngine::AppendTranscript(const std::wstring& question,
                                 const std::wstring& answer,
                                 const std::wstring& jobRole)
{
    // Locate exe directory
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    auto slash = dir.rfind(L'\\');
    if (slash != std::wstring::npos) dir = dir.substr(0, slash + 1);

    // Ensure sessions\ sub-directory exists
    std::wstring folder = dir + L"sessions\\";
    CreateDirectoryW(folder.c_str(), nullptr); // no-op if already exists

    // Session file name: session_YYYYMMDD_HHmm.txt
    // (generated once per process start using a static local)
    static std::wstring s_sessionFile;
    if (s_sessionFile.empty()) {
        SYSTEMTIME st = {};
        GetLocalTime(&st);
        wchar_t ts[32];
        swprintf_s(ts, L"session_%04d%02d%02d_%02d%02d.txt",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
        s_sessionFile = folder + ts;

        // Write header on first call
        HANDLE hf = CreateFileW(s_sessionFile.c_str(),
            GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf != INVALID_HANDLE_VALUE) {
            // UTF-8 BOM + header
            const char bom[] = "\xEF\xBB\xBF";
            DWORD written;
            WriteFile(hf, bom, 3, &written, nullptr);

            std::string header = "=== StealthOverlay Interview Session ===\r\n";
            if (!jobRole.empty()) {
                // Convert wstring to utf8 for writing
                int n = WideCharToMultiByte(CP_UTF8, 0, jobRole.c_str(), -1,
                                            nullptr, 0, nullptr, nullptr);
                if (n > 0) {
                    std::string jrUtf8(n - 1, '\0');
                    WideCharToMultiByte(CP_UTF8, 0, jobRole.c_str(), -1,
                                        jrUtf8.data(), n, nullptr, nullptr);
                    header += "Role: " + jrUtf8 + "\r\n";
                }
            }
            header += "\r\n";
            WriteFile(hf, header.c_str(), (DWORD)header.size(), &written, nullptr);
            CloseHandle(hf);
        }
    }

    // Append Q+A
    HANDLE hf = CreateFileW(s_sessionFile.c_str(),
        GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return;
    SetFilePointer(hf, 0, nullptr, FILE_END);

    // Timestamp
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char ts[32];
    sprintf_s(ts, "[%02d:%02d:%02d]\r\n", st.wHour, st.wMinute, st.wSecond);

    auto toUtf8 = [](const std::wstring& w) -> std::string {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1,
                                    nullptr, 0, nullptr, nullptr);
        if (n <= 0) return {};
        std::string s(n - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1,
                            s.data(), n, nullptr, nullptr);
        return s;
    };

    std::string entry =
        std::string(ts) +
        "Q: " + toUtf8(question) + "\r\n" +
        "A: " + toUtf8(answer)   + "\r\n\r\n";

    DWORD written;
    WriteFile(hf, entry.c_str(), (DWORD)entry.size(), &written, nullptr);
    CloseHandle(hf);
}

// ── Base64 encoder ────────────────────────────────────────────
static const char B64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string AIEngine::Base64Encode(const uint8_t* data, size_t len)
{
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = (uint32_t)data[i] << 16;
        if (i + 1 < len) b |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) b |= (uint32_t)data[i + 2];
        out += B64_CHARS[(b >> 18) & 0x3F];
        out += B64_CHARS[(b >> 12) & 0x3F];
        out += (i + 1 < len) ? B64_CHARS[(b >>  6) & 0x3F] : '=';
        out += (i + 2 < len) ? B64_CHARS[ b        & 0x3F] : '=';
    }
    return out;
}

// ── Stage 2b: Screenshot → Code Error Analysis ───────────────
std::wstring AIEngine::GetAnswerFromImage(const std::vector<uint8_t>& pngData,
                                           std::wstring& outError)
{
    if (pngData.empty()) {
        outError = L"Screenshot is empty";
        return {};
    }
    if (m_config.anthropicKey.empty()) {
        outError = L"Anthropic key needed for screenshot analysis — add it in settings";
        return {};
    }

    // Encode image as base64
    std::string b64 = Base64Encode(pngData.data(), pngData.size());

    // User message describing the task
    std::string userText =
        "I'm in a coding interview. The screen shows a code snippet and I was asked "
        "to identify the error and make it work. What's the bug? Show the corrected code.";

    // Build Anthropic vision JSON body
    std::string body =
        "{\"model\":\"claude-haiku-4-5-20251001\","
        "\"max_tokens\":512,"
        "\"system\":\"" + std::string(JsonEscape(CODE_ERROR_PROMPT)) + "\","
        "\"messages\":[{"
            "\"role\":\"user\","
            "\"content\":["
                "{"
                    "\"type\":\"image\","
                    "\"source\":{"
                        "\"type\":\"base64\","
                        "\"media_type\":\"image/png\","
                        "\"data\":\"" + b64 + "\""
                    "}"
                "},"
                "{"
                    "\"type\":\"text\","
                    "\"text\":\"" + JsonEscape(userText) + "\""
                "}"
            "]"
        "}]}";

    std::string authHeader = "x-api-key: " + WideToUtf8(m_config.anthropicKey);
    std::string extra      = "anthropic-version: 2023-06-01";

    std::string err;
    std::string response = HttpsPost(
        L"api.anthropic.com",
        L"/v1/messages",
        authHeader, extra, body, err);

    if (!err.empty() || response.empty()) {
        outError = Utf8ToWide("Vision API error: " + err);
        return {};
    }

    // Extract answer text from response
    std::string answer;
    auto tidx = response.find("\"text\"");
    if (tidx != std::string::npos)
        answer = ExtractJsonString(response.substr(tidx), "text");

    if (answer.empty()) {
        outError = L"Could not parse vision response";
        OutputDebugStringA(("Vision raw: " + response.substr(0, 300) + "\n").c_str());
        return {};
    }

    outError.clear();
    return Utf8ToWide(answer);
}
