#include "SupertonicVoiceSynthesisProvider.hpp"
#include "impl/SupertonicInferenceImpl.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <regex>
#include <sstream>
#include <utility>

namespace va::thirdparty::tts {

SupertonicVoiceSynthesisProvider::SupertonicVoiceSynthesisProvider(SupertonicVoiceSynthesisOptions options)
    : QueuedVoiceSynthesisProvider("SupertonicTTS", Delivery::Thread),
      impl_(std::make_shared<SupertonicInferenceImpl>(options.onnxDir, options.voiceStylePath, options.language)) {
    SupertonicInferenceImpl::SynthesisParams params;
    params.totalSteps      = std::max(1, options.totalSteps);
    params.speed           = options.speed;
    params.silenceDuration = std::max(0.0f, options.silenceDurationSeconds);
    params.trimMargin      = std::max(0.0f, options.trimMargin);
    impl_->setParams(params);
    setPreBufferedSamplesMS(std::max(0, options.prebufferMs));
}

SupertonicVoiceSynthesisProvider::~SupertonicVoiceSynthesisProvider() {
    stopVoiceSynthesisProvider();
}

bool SupertonicVoiceSynthesisProvider::loadEngine() {
    if (!impl_->initialize()) {
        spdlog::error("[SupertonicTTS] Initialization failed.");
        return false;
    }
    _modelSampleRate = impl_->getSampleRate();
    if (_modelSampleRate != audioConfig().sampleRate && _modelSampleRate > 0 && audioConfig().sampleRate > 0) {
        spdlog::info("[SupertonicTTS] Resampling {} Hz (model) -> {} Hz (pipeline)",
                     _modelSampleRate, audioConfig().sampleRate);
    }
    return true;
}

/// Split normalized text into chunks of at most maxChars characters, breaking
/// preferentially at sentence boundaries (. ? !), then clause boundaries (,),
/// then word boundaries.  This is required because the Supertonic ONNX model
/// has a hard maximum sequence length of 1000 tokens (≈ 1000 characters using
/// the unicode_indexer); longer inputs raise an ONNX broadcast error in the
/// attention Mul node.
namespace {

std::vector<std::string> splitIntoChunks(const std::string& text, size_t maxChars = 900) {
    std::vector<std::string> chunks;
    if (text.empty()) return chunks;
    if (text.size() <= maxChars) {
        chunks.push_back(text);
        return chunks;
    }

    size_t pos = 0;
    while (pos < text.size()) {
        size_t remaining = text.size() - pos;
        if (remaining <= maxChars) {
            chunks.push_back(text.substr(pos));
            break;
        }

        // Search window: look backward from pos+maxChars for a split point.
        size_t windowEnd   = pos + maxChars;
        size_t windowStart = pos + maxChars / 2; // don't split in the first half

        size_t splitAt = std::string::npos;

        // 1. Sentence boundary: '. ', '? ', '! '
        for (size_t i = windowEnd; i > windowStart; --i) {
            if (i + 1 < text.size() &&
                (text[i] == '.' || text[i] == '?' || text[i] == '!') &&
                text[i + 1] == ' ') {
                splitAt = i + 2; // include the space, skip it on resume
                break;
            }
        }

        // 2. Clause boundary: ', '
        if (splitAt == std::string::npos) {
            for (size_t i = windowEnd; i > windowStart; --i) {
                if (i + 1 < text.size() && text[i] == ',' && text[i + 1] == ' ') {
                    splitAt = i + 2;
                    break;
                }
            }
        }

        // 3. Any word boundary: ' '
        if (splitAt == std::string::npos) {
            for (size_t i = windowEnd; i > pos; --i) {
                if (text[i] == ' ') {
                    splitAt = i + 1;
                    break;
                }
            }
        }

        // 4. Hard split - no boundary found in window.
        if (splitAt == std::string::npos || splitAt <= pos) {
            splitAt = pos + maxChars;
        }

        std::string chunk = text.substr(pos, splitAt - pos);
        // Trim trailing whitespace
        while (!chunk.empty() && chunk.back() == ' ') chunk.pop_back();
        if (!chunk.empty()) chunks.push_back(chunk);

        pos = splitAt;
        // Skip leading whitespace on next chunk
        while (pos < text.size() && text[pos] == ' ') ++pos;
    }
    return chunks;
}

/// Split text into sentences (after '.', '?' or '!' followed by a space), each further
/// length-capped by splitIntoChunks(). The LLM hands over a whole reply at once, so
/// synthesizing sentence by sentence lets the first one play while the rest is generated.
/// shortcut: abbreviations like "Dr. " also split; harmless (just an extra pause) - add an exception list if it sounds off.
std::vector<std::string> splitIntoSentenceChunks(const std::string& text) {
    std::vector<std::string> chunks;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        const bool boundary = i == text.size() ||
            ((text[i] == '.' || text[i] == '?' || text[i] == '!') && i + 1 < text.size() && text[i + 1] == ' ');
        if (!boundary) continue;
        const size_t end = std::min(i + 1, text.size());
        size_t s = start;
        while (s < end && text[s] == ' ') ++s;
        for (auto& c : splitIntoChunks(text.substr(s, end - s))) chunks.push_back(std::move(c));
        start = end;
    }
    return chunks;
}

} // namespace

bool SupertonicVoiceSynthesisProvider::synthesizeOne(const std::string& text) {
    // A part said faster or slower (say { speed }): its speed times the part's, for this text only.
    struct SpeedFor {
        std::shared_ptr<SupertonicInferenceImpl> impl;
        float own = 0;
        SpeedFor(std::shared_ptr<SupertonicInferenceImpl> engine, float factor) : impl(std::move(engine)) {
            if (factor <= 0) return;
            auto p = impl->getParams();
            own = p.speed;
            p.speed = own * factor;
            impl->setParams(p);
        }
        ~SpeedFor() {
            if (own <= 0) return;
            auto p = impl->getParams();
            p.speed = own;
            impl->setParams(p);
        }
    } speedFor(impl_, style().speed);
    std::string normalized = normalizeText(text);
    auto chunks = splitIntoSentenceChunks(normalized);
    spdlog::debug("[SupertonicTTS] {} chunk(s) for {} chars total", chunks.size(), normalized.size());
    const auto params = impl_->getParams();
    // Silence at the model's rate: it's resampled with the speech (zeros stay zeros, at the right length).
    const int silenceSamplesModel = (params.silenceDuration > 0.0f && _modelSampleRate > 0)
        ? static_cast<int>(_modelSampleRate * params.silenceDuration) : 0;

    for (const auto& chunk : chunks) {
        if (cancelled()) return false;
        spdlog::debug("[SupertonicTTS] Synthesizing chunk ({} chars)", chunk.size());
        auto out = impl_->synthesize(chunk);
        if (cancelled()) return false;  // barge-in during ONNX - discard result
        // The delivery thread plays it while the next chunk is synthesized.
        handoffAudio(std::move(out.wav), _modelSampleRate);
        if (silenceSamplesModel > 0)
            handoffAudio(std::vector<float>(silenceSamplesModel, 0.0f), _modelSampleRate);
    }
    return finishHandoff();
}

// --- Chunking knobs ---

// --- Text normalization (numbers to words) ---

namespace {

constexpr const char* ONES[] = {
    "", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
    "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen",
    "seventeen", "eighteen", "nineteen"
};
constexpr const char* TENS[] = {
    "", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"
};

std::string numberToWords(long long n) {
    if (n == 0) return "zero";
    if (n < 0) return "negative " + numberToWords(-n);

    std::string result;
    if (n >= 1000000000LL) {
        result += numberToWords(n / 1000000000LL) + " billion ";
        n %= 1000000000LL;
    }
    if (n >= 1000000LL) {
        result += numberToWords(n / 1000000LL) + " million ";
        n %= 1000000LL;
    }
    if (n >= 1000LL) {
        result += numberToWords(n / 1000LL) + " thousand ";
        n %= 1000LL;
    }
    if (n >= 100) {
        result += std::string(ONES[n / 100]) + " hundred ";
        n %= 100;
    }
    if (n >= 20) {
        result += std::string(TENS[n / 10]);
        if (n % 10) result += std::string(" ") + ONES[n % 10];
    } else if (n > 0) {
        result += ONES[n];
    }
    // Trim trailing space
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
}

/// Spell out a digit string one digit at a time: "778" -> "seven seven eight"
std::string digitByDigit(const std::string& digits) {
    constexpr const char* DIGIT_NAMES[] = {
        "zero", "one", "two", "three", "four",
        "five", "six", "seven", "eight", "nine"
    };
    std::string result;
    for (char c : digits) {
        if (c >= '0' && c <= '9') {
            if (!result.empty()) result += ' ';
            result += DIGIT_NAMES[c - '0'];
        }
    }
    return result;
}

/// Strip markdown formatting and LLM artifacts so the TTS model only sees
/// clean, speakable prose.  Must run BEFORE number normalization because
/// markdown markers like `**` or `#` are not in the model's unicode indexer
/// and confuse the attention mechanism, causing entire words to be skipped.
std::string stripMarkdown(const std::string& text) {
    std::string result;
    result.reserve(text.size());

    size_t i = 0;
    while (i < text.size()) {
        char c = text[i];

        // Skip bold/italic markers: ** or *
        if (c == '*') { ++i; continue; }

        // Skip backtick (inline code / code blocks)
        if (c == '`') { ++i; continue; }

        // Skip leading bullet markers at the start of a line: "- " or "• "
        if ((c == '-' || c == '\xE2') && (i == 0 || text[i - 1] == '\n')) {
            // UTF-8 bullet "•" = E2 80 A2
            if (c == '\xE2' && i + 2 < text.size() &&
                static_cast<unsigned char>(text[i + 1]) == 0x80 &&
                static_cast<unsigned char>(text[i + 2]) == 0xA2) {
                i += 3;
                if (i < text.size() && text[i] == ' ') ++i;
                continue;
            }
            if (c == '-' && i + 1 < text.size() && text[i + 1] == ' ') {
                i += 2;
                continue;
            }
        }

        // Replace newlines / carriage returns with space
        if (c == '\n' || c == '\r') {
            // Collapse consecutive newlines into a single space
            if (!result.empty() && result.back() != ' ') result += ' ';
            ++i;
            continue;
        }

        // Strip leading numbered-list markers at the start of a line: "1. ", "2) "
        if (std::isdigit(static_cast<unsigned char>(c)) &&
            (i == 0 || text[i - 1] == '\n')) {
            size_t j = i;
            while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) ++j;
            if (j < text.size() && (text[j] == '.' || text[j] == ')') &&
                j + 1 < text.size() && text[j + 1] == ' ') {
                i = j + 2; // skip "1. " or "2) "
                continue;
            }
        }

        result += c;
        ++i;
    }

    // Collapse multiple spaces
    std::string cleaned;
    cleaned.reserve(result.size());
    bool lastSpace = false;
    for (char ch : result) {
        if (ch == ' ') {
            if (!lastSpace) cleaned += ch;
            lastSpace = true;
        } else {
            cleaned += ch;
            lastSpace = false;
        }
    }
    // Trim leading/trailing whitespace
    size_t a = cleaned.find_first_not_of(' ');
    if (a == std::string::npos) return "";
    size_t b = cleaned.find_last_not_of(' ');
    return cleaned.substr(a, b - a + 1);
}

} // namespace

std::string SupertonicVoiceSynthesisProvider::normalizeText(const std::string& text) {
    // First strip markdown/LLM formatting artifacts
    std::string stripped = stripMarkdown(text);
    if (stripped.empty()) return "";

    std::string result;
    result.reserve(stripped.size() * 2);

    size_t i = 0;
    while (i < stripped.size()) {
        // Check for a numeric sequence (possibly with embedded hyphens like phone numbers)
        if (std::isdigit(static_cast<unsigned char>(stripped[i]))) {
            // Collect the full numeric token (digits, hyphens between digit groups, dots for decimals)
            std::string token;
            size_t start = i;
            while (i < stripped.size() &&
                   (std::isdigit(static_cast<unsigned char>(stripped[i])) || stripped[i] == '-' || stripped[i] == '.')) {
                token += stripped[i];
                ++i;
            }
            // Remove trailing non-digit (e.g. trailing hyphen)
            while (!token.empty() && !std::isdigit(static_cast<unsigned char>(token.back()))) {
                token.pop_back();
                --i;
            }

            // Decide how to speak the number
            bool hasHyphen = token.find('-') != std::string::npos;
            bool hasDot = token.find('.') != std::string::npos;

            if (hasHyphen) {
                // Phone-number / code style: spell each group digit-by-digit
                // "778-3216-778" -> "seven seven eight, three two one six, seven seven eight"
                std::string spoken;
                std::string group;
                for (char c : token) {
                    if (c == '-') {
                        if (!group.empty()) {
                            if (!spoken.empty()) spoken += ", ";
                            spoken += digitByDigit(group);
                            group.clear();
                        }
                    } else {
                        group += c;
                    }
                }
                if (!group.empty()) {
                    if (!spoken.empty()) spoken += ", ";
                    spoken += digitByDigit(group);
                }
                result += spoken;
            } else if (hasDot) {
                // Decimal: "3.14" -> "three point one four"
                auto dotPos = token.find('.');
                std::string intPart = token.substr(0, dotPos);
                std::string fracPart = token.substr(dotPos + 1);
                long long intVal = 0;
                try { intVal = std::stoll(intPart); } catch (...) {}
                result += numberToWords(intVal) + " point " + digitByDigit(fracPart);
            } else {
                // Plain integer
                long long val = 0;
                try { val = std::stoll(token); } catch (...) {}
                if (token.size() <= 4 && val > 0) {
                    // Short numbers: speak as a number
                    result += numberToWords(val);
                } else if (token.size() > 7) {
                    // Very long digit strings: spell digit by digit
                    result += digitByDigit(token);
                } else {
                    result += numberToWords(val);
                }
            }
        } else {
            result += stripped[i];
            ++i;
        }
    }
    return result;
}

} // namespace va::thirdparty::tts
