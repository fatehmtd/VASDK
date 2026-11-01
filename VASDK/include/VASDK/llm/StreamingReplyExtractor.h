#pragma once

#include <VASDK/llm/ResponseChunker.h>
#include "vasdk_export.h"

#include <string>
#include <vector>

namespace va {
namespace llm {

/**
 * @brief Pulls speakable sentences out of a streaming LLM reply as it arrives.
 *
 * Replies follow the structured format the prompt asks for ({"text": "...", ...} or
 * {"tools": [...]}), or are plain prose from models that ignore it. The shape is decided from
 * the first characters, so speech can start while the model is still generating:
 *   - {"text": "..."  : the string value is JSON-decoded and split into sentences;
 *   - {"tools": ...   : nothing is spoken (toolsDetected() turns true);
 *   - any other JSON  : nothing is streamed; the final parse decides what to say;
 *   - not JSON        : the raw text is split into sentences.
 * A leading reasoning block (<think>...</think>, tags supplied by the caller) and a leading
 * ``` code fence are skipped. Transport-agnostic: it only needs the text deltas.
 */
class VASDK_EXPORT StreamingReplyExtractor {
public:
    enum class Shape { Unknown, Text, Tools, OtherJson, Prose };

    StreamingReplyExtractor(std::string reasoningOpen = {}, std::string reasoningClose = {},
                            size_t minSentenceBytes = 8);

    /// Appends a delta; returns the sentences that became complete (possibly none).
    std::vector<std::string> feed(const std::string& delta);

    /// Returns the speakable text decoded so far but not yet returned by feed(), and marks it spoken.
    std::string flush();

    Shape shape() const { return _shape; }
    bool toolsDetected() const { return _shape == Shape::Tools; }

    /// Everything returned so far by feed() and flush(), concatenated.
    const std::string& spokenText() const { return _spoken; }

private:
    bool resolveShape();              // advances _pos past reasoning/fence/key; false = need more input
    void decodeJsonString();          // appends decoded chars of the "text" value to _decoded
    std::vector<std::string> takeSentences();

    std::string _reasoningOpen, _reasoningClose;
    transport::PunctuationChunker _chunker;

    std::string _raw;                 // all input
    size_t      _pos = 0;             // parse position in _raw
    Shape       _shape = Shape::Unknown;
    bool        _stringClosed = false;

    std::string _decoded;             // speakable text so far
    size_t      _emitted = 0;         // bytes of _decoded already returned
    size_t      _scanned = 0;         // bytes of _decoded already checked for sentence ends
    std::string _spoken;
};

} // namespace llm
} // namespace va
