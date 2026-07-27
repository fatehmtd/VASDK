#include "SupertonicVoiceSynthesisProvider.hpp"
#include "impl/SupertonicInferenceImpl.hpp"
#include <VASDK/AudioSIMD.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <regex>
#include <sstream>

SupertonicVoiceSynthesisProvider::SupertonicVoiceSynthesisProvider(
    const std::string& modelDir,
    const std::string& stylePath,
    const std::string& lang)
    : impl_(std::make_shared<SupertonicInferenceImpl>(modelDir, stylePath, lang)) {}

SupertonicVoiceSynthesisProvider::~SupertonicVoiceSynthesisProvider() {
    stopVoiceSynthesisProvider();
}

// --- GenericVoiceSynthesisProvider interface ---

bool SupertonicVoiceSynthesisProvider::startVoiceSynthesisProvider() {
    if (_running) {
        spdlog::warn("SupertonicTTS provider already running.");
        return true;
    }
    if (!impl_->initialize()) {
        return false;
    }
    _modelSampleRate = impl_->getSampleRate();
    if (_modelSampleRate != audioConfig_.sampleRate && _modelSampleRate > 0 && audioConfig_.sampleRate > 0) {
        spdlog::info("SupertonicTTS resampling active: {} Hz (model) -> {} Hz (pipeline)",
                     _modelSampleRate, audioConfig_.sampleRate);
    }
    _running = true;
    _stopWorker.store(false);
    _deliveryDone = false;
    _inSynthesisSequence = false;
    _synthThread    = std::thread(&SupertonicVoiceSynthesisProvider::workerLoop,   this);
    _deliveryThread = std::thread(&SupertonicVoiceSynthesisProvider::deliveryLoop, this);
    spdlog::info("SupertonicTTS provider started.");
    return true;
}

void SupertonicVoiceSynthesisProvider::stopVoiceSynthesisProvider() {
    if (!_running) return;
    _cancelSynth.store(true);
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        while (!_textQueue.empty()) _textQueue.pop();
        _stopWorker.store(true);
    }
    _queueCv.notify_all();
    if (_synthThread.joinable()) _synthThread.join();

    // Signal the delivery thread to exit after draining any in-flight slot.
    flushDeliverySlot();
    {
        std::lock_guard<std::mutex> lock(_deliveryMutex);
        _deliveryDone = true;
    }
    _deliveryReadyCv.notify_all();
    if (_deliveryThread.joinable()) _deliveryThread.join();

    _running = false;
    spdlog::info("SupertonicTTS provider stopped.");
}

bool SupertonicVoiceSynthesisProvider::isVoiceSynthesisProviderRunning() const {
    return _running;
}

bool SupertonicVoiceSynthesisProvider::synthesizeSpeech(const std::string& text) {
    if (!_running) {
        spdlog::error("SupertonicTTS provider is not running.");
        return false;
    }
    if (text.empty()) return false;

    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        _textQueue.push(text);
    }
    _queueCv.notify_one();
    return true;
}

void SupertonicVoiceSynthesisProvider::cancelSynthesis() {
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        while (!_textQueue.empty()) _textQueue.pop();
    }
    _cancelSynth.store(true);
    _queueCv.notify_all();
    flushDeliverySlot();
}

/// Split normalized text into chunks of at most maxChars characters, breaking
/// preferentially at sentence boundaries (. ? !), then clause boundaries (,),
/// then word boundaries.  This is required because the Supertonic ONNX model
/// has a hard maximum sequence length of 1000 tokens (≈ 1000 characters using
/// the unicode_indexer); longer inputs raise an ONNX broadcast error in the
/// attention Mul node.
static std::vector<std::string> splitIntoChunks(const std::string& text, size_t maxChars = 900) {
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

void SupertonicVoiceSynthesisProvider::workerLoop() {
    while (true) {
        std::string text;
        bool isSequenceStart = false;
        {
            std::unique_lock<std::mutex> lock(_queueMutex);
            _queueCv.wait(lock, [this] {
                return !_textQueue.empty() || _stopWorker.load();
            });
            if (_stopWorker.load() && _textQueue.empty()) break;
            if (_textQueue.empty()) continue;
            text = std::move(_textQueue.front());
            _textQueue.pop();
            isSequenceStart = !_inSynthesisSequence;
            _inSynthesisSequence = true;
        }

        if (isSequenceStart) {
            // Fresh sequence - clear any stale cancel from a previous barge-in.
            _cancelSynth.store(false);
        } else if (_cancelSynth.load()) {
            // Barge-in arrived mid-sequence: discard this and any remaining queued items.
            std::lock_guard<std::mutex> lock(_queueMutex);
            while (!_textQueue.empty()) _textQueue.pop();
            _inSynthesisSequence = false;
            continue;
        }

        if (isSequenceStart && resultListener_)
            resultListener_->onSynthesisStarted();

        bool completed = synthesizeOne(text);

        bool shouldFireCompleted;
        {
            std::lock_guard<std::mutex> lock(_queueMutex);
            if (!completed || _cancelSynth.load()) {
                // Cancelled or error: drain remaining queued items.
                while (!_textQueue.empty()) _textQueue.pop();
                _inSynthesisSequence = false;
                shouldFireCompleted = false;
            } else if (_textQueue.empty()) {
                // Queue drained - all sentences have been spoken.
                _inSynthesisSequence = false;
                shouldFireCompleted = true;
            } else {
                // More sentences pending - keep synthesising without completing.
                shouldFireCompleted = false;
            }
        }

        if (shouldFireCompleted && resultListener_)
            resultListener_->onSynthesisCompleted();
    }
}

bool SupertonicVoiceSynthesisProvider::synthesizeOne(const std::string& text) {
    std::string normalized = normalizeText(text);
    auto chunks = splitIntoChunks(normalized);
    spdlog::debug("SupertonicTTS: {} chunk(s) for {} chars total",
                  chunks.size(), normalized.size());
    try {
        const auto params = impl_->getParams();
        // Silence is expressed in model sample rate so it travels through the normal
        // handoff→deliverAudio path and gets resampled to the correct output count.
        // (zeros → zeros regardless of resampling, just need the right duration.)
        const int  silenceSamplesModel = (params.silenceDuration > 0.0f && _modelSampleRate > 0)
            ? static_cast<int>(_modelSampleRate * params.silenceDuration) : 0;

        for (const auto& chunk : chunks) {
            if (_cancelSynth.load()) return false;
            spdlog::debug("SupertonicTTS synthesizing chunk ({} chars)", chunk.size());
            auto out = impl_->synthesize(chunk);
            if (_cancelSynth.load()) return false;  // barge-in during ONNX - discard result
            // Hand off to delivery thread so synthesis of the next chunk can begin immediately.
            handoffAudio(std::move(out.wav));
            // Append inter-chunk silence (separate handoff so the main wav buffer stays clean).
            if (silenceSamplesModel > 0 && !_cancelSynth.load()) {
                handoffAudio(std::vector<float>(silenceSamplesModel, 0.0f));
            }
        }
        // Wait for the delivery thread to pick up the last slot before returning.
        // This ensures onSynthesisCompleted() fires only after all audio has been
        // submitted to deliverAudio(), preserving the original ordering guarantee.
        {
            std::unique_lock<std::mutex> lock(_deliveryMutex);
            _deliveryCv.wait(lock, [this] {
                return !_deliverySlot.has_value() || _cancelSynth.load();
            });
        }
        return true;
    } catch (const std::exception& e) {
        spdlog::error("SupertonicTTS synthesis failed: {}", e.what());
        if (resultListener_) resultListener_->onSynthesisError(e.what());
        return false;
    }
}

void SupertonicVoiceSynthesisProvider::setAudioConfiguration(const va::audio::AudioConfig& config) {
    audioConfig_ = config;
}

va::audio::AudioConfig SupertonicVoiceSynthesisProvider::getAudioConfiguration() const {
    return audioConfig_;
}

void SupertonicVoiceSynthesisProvider::setVoiceSynthesisResultListener(
    va::audio::VoiceSynthesisResultListener* listener) {
    resultListener_ = listener;
}

va::audio::VoiceSynthesisResultListener* SupertonicVoiceSynthesisProvider::getVoiceSynthesisResultListener() const {
    return resultListener_;
}

// --- Synthesis quality / timing knobs ---

void SupertonicVoiceSynthesisProvider::setSpeed(float speed) {
    auto p = impl_->getParams();
    p.speed = speed;
    impl_->setParams(p);
}

float SupertonicVoiceSynthesisProvider::getSpeed() const {
    return impl_->getParams().speed;
}

void SupertonicVoiceSynthesisProvider::setSilenceDuration(float seconds) {
    auto p = impl_->getParams();
    p.silenceDuration = std::max(0.0f, seconds);
    impl_->setParams(p);
}

float SupertonicVoiceSynthesisProvider::getSilenceDuration() const {
    return impl_->getParams().silenceDuration;
}

void SupertonicVoiceSynthesisProvider::setTrimMargin(float margin) {
    auto p = impl_->getParams();
    p.trimMargin = std::max(0.0f, margin);
    impl_->setParams(p);
}

float SupertonicVoiceSynthesisProvider::getTrimMargin() const {
    return impl_->getParams().trimMargin;
}

void SupertonicVoiceSynthesisProvider::setTotalSteps(int steps) {
    auto p = impl_->getParams();
    p.totalSteps = std::max(1, steps);
    impl_->setParams(p);
}

int SupertonicVoiceSynthesisProvider::getTotalSteps() const {
    return impl_->getParams().totalSteps;
}

// --- Chunking knobs ---

// --- Text normalization (numbers to words) ---

static const char* const ONES[] = {
    "", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
    "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen",
    "seventeen", "eighteen", "nineteen"
};
static const char* const TENS[] = {
    "", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"
};

static std::string numberToWords(long long n) {
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
static std::string digitByDigit(const std::string& digits) {
    static const char* const DIGIT_NAMES[] = {
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
static std::string stripMarkdown(const std::string& text) {
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

// --- Resampling & audio format conversion ---

// Normalized sinc function: sin(pi*x) / (pi*x), sinc(0) = 1
static constexpr double PI = 3.14159265358979323846;
static inline double sinc(double x) {
    if (std::abs(x) < 1e-9) return 1.0;
    double px = PI * x;
    return std::sin(px) / px;
}

/// Windowed-sinc resample from srcRate to dstRate, writing into a pre-allocated buffer.
/// Uses a Lanczos kernel (a=3) for high-quality anti-aliased resampling.
/// `out` is resized to the required output length.
static void resample(const std::vector<float>& in, int srcRate, int dstRate,
                     std::vector<float>& out) {
    if (in.empty() || srcRate <= 0 || dstRate <= 0) { out.clear(); return; }

    const int A = 3; // Lanczos kernel half-width (a=3 is high quality)
    double ratio = static_cast<double>(dstRate) / srcRate;
    // When downsampling, stretch the kernel to act as a low-pass filter at the Nyquist of the output
    double filterScale = std::min(1.0, ratio);
    int kernelRadius = static_cast<int>(std::ceil(A / filterScale));

    size_t outLen = static_cast<size_t>(std::ceil(in.size() * ratio));
    out.resize(outLen);

    for (size_t i = 0; i < outLen; ++i) {
        double center = i / ratio; // position in the input
        int left  = static_cast<int>(std::floor(center)) - kernelRadius + 1;
        int right = static_cast<int>(std::floor(center)) + kernelRadius;

        double sample = 0.0;
        double weightSum = 0.0;

        for (int j = left; j <= right; ++j) {
            double x = (center - j) * filterScale;
            // Lanczos window: sinc(x) * sinc(x/a) for |x| < a, else 0
            double w = 0.0;
            if (std::abs(x) < A) {
                w = sinc(x) * sinc(x / A);
            }
            if (j >= 0 && j < static_cast<int>(in.size())) {
                sample += in[j] * w;
                weightSum += w;
            }
        }

        out[i] = (weightSum > 0.0) ? static_cast<float>(sample / weightSum) : 0.0f;
    }
}

void SupertonicVoiceSynthesisProvider::deliverAudio(const std::vector<float>& pcmIn) {
    if (!resultListener_ || pcmIn.empty()) return;

    // Resample from model native rate to pipeline rate if they differ.
    // _modelSampleRate is cached at startup - no virtual call or mutex needed here.
    const std::vector<float>* pcm = &pcmIn;
    if (_modelSampleRate != audioConfig_.sampleRate && _modelSampleRate > 0 && audioConfig_.sampleRate > 0) {
        resample(pcmIn, _modelSampleRate, audioConfig_.sampleRate, _resampleBuf);
        pcm = &_resampleBuf;
    }

    switch (audioConfig_.format) {
    case va::audio::AudioFormat::SHORT: {
        _int16Buf.resize(pcm->size());
        AudioSIMD::convertFloatToInt16(pcm->data(), _int16Buf.data(), pcm->size());
        resultListener_->onAudioDataReceived(
            reinterpret_cast<const char*>(_int16Buf.data()),
            _int16Buf.size() * sizeof(int16_t));
        break;
    }
    case va::audio::AudioFormat::FLOAT: {
        resultListener_->onAudioDataReceived(
            reinterpret_cast<const char*>(pcm->data()),
            pcm->size() * sizeof(float));
        break;
    }
    case va::audio::AudioFormat::INT: {
        _int32Buf.resize(pcm->size());
        AudioSIMD::convertFloatToInt32(pcm->data(), _int32Buf.data(), pcm->size());
        resultListener_->onAudioDataReceived(
            reinterpret_cast<const char*>(_int32Buf.data()),
            _int32Buf.size() * sizeof(int32_t));
        break;
    }
    case va::audio::AudioFormat::BYTE: {
        _uint8Buf.resize(pcm->size());
        AudioSIMD::convertFloatToUint8(pcm->data(), _uint8Buf.data(), pcm->size());
        resultListener_->onAudioDataReceived(
            reinterpret_cast<const char*>(_uint8Buf.data()),
            _uint8Buf.size() * sizeof(uint8_t));
        break;
    }
    default:
        spdlog::warn("SupertonicTTS: unknown audio format, delivering as float.");
        resultListener_->onAudioDataReceived(
            reinterpret_cast<const char*>(pcm->data()),
            pcm->size() * sizeof(float));
        break;
    }
}

// --- Delivery pipeline (synthesis+delivery overlap) ---

void SupertonicVoiceSynthesisProvider::deliveryLoop() {
    while (true) {
        std::vector<float> wav;
        {
            std::unique_lock<std::mutex> lock(_deliveryMutex);
            _deliveryReadyCv.wait(lock, [this] {
                return _deliverySlot.has_value() || _deliveryDone;
            });
            if (_deliveryDone && !_deliverySlot.has_value()) break;
            wav = std::move(*_deliverySlot);
            _deliverySlot.reset();
            _deliveryCv.notify_one(); // unblock synthesis thread waiting to place next chunk
        }
        deliverAudio(wav); // runs without any lock held
    }
}

void SupertonicVoiceSynthesisProvider::handoffAudio(std::vector<float> wav) {
    std::unique_lock<std::mutex> lock(_deliveryMutex);
    // Wait until the slot is free OR barge-in has been requested.
    _deliveryCv.wait(lock, [this] {
        return !_deliverySlot.has_value() || _cancelSynth.load();
    });
    if (_cancelSynth.load()) return; // barge-in - discard this chunk
    _deliverySlot = std::move(wav);
    _deliveryReadyCv.notify_one();
}

void SupertonicVoiceSynthesisProvider::flushDeliverySlot() {
    std::lock_guard<std::mutex> lock(_deliveryMutex);
    _deliverySlot.reset();
    _deliveryCv.notify_one(); // unblock synthesis thread if it was waiting to hand off
}
