#pragma once

#include <string>
#include <memory>

namespace va {
namespace transport {

// 
// ResponseChunkerInterface
//
// Controls how streaming LLM output is split into chunks that are forwarded to
// downstream processors (e.g. TTSFrameProcessor) before the full response has
// finished generating.
//
// findChunkEnd() is called after each token is appended to `response`.
//   response       - the full accumulated response string so far
//   chunkStart     - byte offset of the start of the current (un-emitted) chunk
//   newTokenStart  - byte offset where the most-recently appended token begins
//                    (= response.size() - n_piece before the append)
//
// Returns the exclusive end of the chunk to emit, or std::string::npos if no
// chunk should be emitted yet.  The caller advances chunkStart to the returned
// value and emits response.substr(old_chunkStart, returnedValue - old_chunkStart).
// 
class ResponseChunkerInterface {
public:
    virtual ~ResponseChunkerInterface() = default;

    virtual size_t findChunkEnd(const std::string& response,
                                size_t             chunkStart,
                                size_t             newTokenStart) const = 0;

    virtual const char* name() const = 0;
};

// 
// PunctuationChunker
//
// Emits a chunk at each sentence-ending punctuation mark followed by whitespace
// (". ", "! ", "? ", ": ").
//
// Scans from max(chunkStart+1, newTokenStart-1) so the check correctly catches
// punctuation/space pairs that span a token boundary - e.g. token "." then
// token " Hello" - which a simple last-two-chars check would miss.
//
// A chunk is only emitted when it is at least minChunkBytes long to avoid
// forwarding tiny fragments at the start of a sentence.
// 
class PunctuationChunker final : public ResponseChunkerInterface {
public:
    explicit PunctuationChunker(size_t minChunkBytes = 8)
        : minChunkBytes_(minChunkBytes) {}

    size_t findChunkEnd(const std::string& response,
                        size_t             chunkStart,
                        size_t             newTokenStart) const override
    {
        if (response.size() < 2)
            return std::string::npos;

        // Scan from one char before the new token so we catch pairs that
        // straddle token boundaries (e.g. prev token ends with '.',
        // new token starts with ' ').
        size_t scanFrom = (newTokenStart > 0) ? newTokenStart - 1 : 0;
        // But never scan before chunkStart + 1 (need i-1 to be valid and
        // within the current chunk).
        if (scanFrom < chunkStart + 1)
            scanFrom = chunkStart + 1;

        for (size_t i = scanFrom; i < response.size(); ++i) {
            char prev = response[i - 1];
            char curr = response[i];

            if (isPunctuationBoundary(prev, curr)) {
                size_t chunkLen = (i + 1) - chunkStart;
                if (chunkLen >= minChunkBytes_)
                    return i + 1; // exclusive end - include the space/newline
            }
        }
        return std::string::npos;
    }

    const char* name() const override { return "PunctuationChunker"; }

private:
    bool isPunctuationBoundary(char prev, char curr) const {
        return (curr == ' ' || curr == '\n') &&
               (prev == '.' || prev == '!' ||
                prev == '?' || prev == ':');
    }
    size_t minChunkBytes_;
};

// 
// WordCountChunker
//
// Emits a chunk once the current un-emitted segment reaches wordsPerChunk words.
// Useful for models that rarely produce explicit punctuation, or for keeping
// TTS synthesis fed at a steady cadence regardless of sentence length.
// 
class WordCountChunker final : public ResponseChunkerInterface {
public:
    explicit WordCountChunker(int wordsPerChunk = 10)
        : wordsPerChunk_(wordsPerChunk) {}

    size_t findChunkEnd(const std::string& response,
                        size_t             chunkStart,
                        size_t             /*newTokenStart*/) const override
    {
        int   wordCount = 0;
        bool  inWord    = false;
        size_t lastSpaceAfterWord = std::string::npos;

        for (size_t i = chunkStart; i < response.size(); ++i) {
            char c = response[i];
            bool spaceFlag = isSpace(c);

            if (!spaceFlag) {
                inWord = true;
            } else if (inWord) {
                ++wordCount;
                inWord = false;
                lastSpaceAfterWord = i + 1; // end of word boundary (after space)
                if (wordCount >= wordsPerChunk_)
                    return lastSpaceAfterWord;
            }
        }
        return std::string::npos;
    }

    const char* name() const override { return "WordCountChunker"; }

private:
    bool isSpace(char c) const {
        return (c == ' ' || c == '\t' || c == '\n' || c == '\r');
    }
    int wordsPerChunk_;
};

// 
// NoChunkingChunker
//
// Never emits mid-generation chunks.  The full response is forwarded as a
// single frame after generation completes.  Equivalent to the original
// single-shot behaviour and useful for latency benchmarking.
// 
class NoChunkingChunker final : public ResponseChunkerInterface {
public:
    size_t findChunkEnd(const std::string& /*response*/,
                        size_t             /*chunkStart*/,
                        size_t             /*newTokenStart*/) const override
    {
        return std::string::npos;
    }

    const char* name() const override { return "NoChunkingChunker"; }
};

} // namespace transport
} // namespace va
