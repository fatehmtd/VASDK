#include <VASDK/llm/StreamingReplyExtractor.h>

#include <algorithm>

namespace va {
namespace llm {

namespace {

bool isWs(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

size_t skipWs(const std::string& s, size_t i) {
    while (i < s.size() && isWs(s[i])) ++i;
    return i;
}

// True when s[from..] is a strict prefix of `full` (more input may still complete it).
bool couldBecome(const std::string& s, size_t from, const std::string& full) {
    const size_t n = s.size() - from;
    return n < full.size() && full.compare(0, n, s, from, n) == 0;
}

bool parseHex4(const std::string& s, size_t at, unsigned& out) {
    out = 0;
    for (size_t k = at; k < at + 4; ++k) {
        const char c = s[k];
        out <<= 4;
        if (c >= '0' && c <= '9')      out |= unsigned(c - '0');
        else if (c >= 'a' && c <= 'f') out |= unsigned(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') out |= unsigned(c - 'A' + 10);
        else return false;
    }
    return true;
}

void appendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

} // namespace

StreamingReplyExtractor::StreamingReplyExtractor(std::string reasoningOpen, std::string reasoningClose,
                                                 size_t minSentenceBytes)
    : _reasoningOpen(std::move(reasoningOpen)),
      _reasoningClose(std::move(reasoningClose)),
      _chunker(minSentenceBytes) {}

std::vector<std::string> StreamingReplyExtractor::feed(const std::string& delta) {
    _raw += delta;
    if (_shape == Shape::Unknown && !resolveShape()) return {};

    if (_shape == Shape::Text) {
        decodeJsonString();
    } else if (_shape == Shape::Prose && !_stringClosed) {
        // The final parse reads the first JSON object when one shows up mid-prose, so stop there.
        const size_t brace = _raw.find('{', _pos);
        const size_t end   = brace == std::string::npos ? _raw.size() : brace;
        _decoded.append(_raw, _pos, end - _pos);
        _pos = end;
        if (brace != std::string::npos) _stringClosed = true;
    }
    return takeSentences();
}

std::string StreamingReplyExtractor::flush() {
    if (_shape != Shape::Text && _shape != Shape::Prose) return {};
    std::string tail = _decoded.substr(_emitted);
    _emitted = _scanned = _decoded.size();
    _spoken += tail;
    return tail;
}

bool StreamingReplyExtractor::resolveShape() {
    // Skip leading whitespace, a reasoning block and a ``` fence, in any order.
    while (true) {
        _pos = skipWs(_raw, _pos);
        if (_pos >= _raw.size()) return false;
        if (!_reasoningOpen.empty()) {
            if (_raw.compare(_pos, _reasoningOpen.size(), _reasoningOpen) == 0) {
                const size_t close = _raw.find(_reasoningClose, _pos + _reasoningOpen.size());
                if (close == std::string::npos) return false;
                _pos = close + _reasoningClose.size();
                continue;
            }
            if (couldBecome(_raw, _pos, _reasoningOpen)) return false;
        }
        if (_raw.compare(_pos, 3, "```") == 0) {
            const size_t nl = _raw.find('\n', _pos);
            if (nl == std::string::npos) return false;
            _pos = nl + 1;
            continue;
        }
        if (couldBecome(_raw, _pos, "```")) return false;
        break;
    }

    if (_raw[_pos] != '{') {
        _shape = Shape::Prose;
        return true;
    }

    // {"key": ...
    size_t i = skipWs(_raw, _pos + 1);
    if (i >= _raw.size()) return false;
    if (_raw[i] != '"') { _shape = Shape::OtherJson; return true; }
    const size_t keyEnd = _raw.find('"', i + 1);
    if (keyEnd == std::string::npos) return false;
    const std::string key = _raw.substr(i + 1, keyEnd - i - 1);
    if (key == "tools" || key == "tool") { _shape = Shape::Tools; return true; }
    if (key != "text") { _shape = Shape::OtherJson; return true; }

    i = skipWs(_raw, keyEnd + 1);
    if (i >= _raw.size()) return false;
    if (_raw[i] != ':') { _shape = Shape::OtherJson; return true; }
    i = skipWs(_raw, i + 1);
    if (i >= _raw.size()) return false;
    if (_raw[i] != '"') { _shape = Shape::OtherJson; return true; }
    _pos   = i + 1;
    _shape = Shape::Text;
    return true;
}

void StreamingReplyExtractor::decodeJsonString() {
    while (!_stringClosed && _pos < _raw.size()) {
        const char c = _raw[_pos];
        if (c == '"') {
            _stringClosed = true;
            ++_pos;
            return;
        }
        if (c != '\\') {
            _decoded += c;
            ++_pos;
            continue;
        }
        if (_pos + 1 >= _raw.size()) return;  // wait for the escaped char
        const char e = _raw[_pos + 1];
        if (e != 'u') {
            switch (e) {
                case 'n': _decoded += '\n'; break;
                case 't': _decoded += ' ';  break;
                case 'r': case 'b': case 'f': break;
                default:  _decoded += e;    break;  // \" \\ \/
            }
            _pos += 2;
            continue;
        }
        if (_pos + 6 > _raw.size()) return;
        unsigned cp = 0;
        if (!parseHex4(_raw, _pos + 2, cp)) { _pos += 2; continue; }
        size_t consumed = 6;
        if (cp >= 0xD800 && cp <= 0xDBFF) {  // high surrogate: needs the low half
            if (_pos + 12 > _raw.size()) return;
            unsigned lo = 0;
            if (_raw[_pos + 6] == '\\' && _raw[_pos + 7] == 'u' && parseHex4(_raw, _pos + 8, lo) &&
                lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                consumed = 12;
            }
        }
        appendUtf8(_decoded, cp);
        _pos += consumed;
    }
}

std::vector<std::string> StreamingReplyExtractor::takeSentences() {
    std::vector<std::string> sentences;
    while (true) {
        const size_t end = _chunker.findChunkEnd(_decoded, _emitted, std::max(_scanned, _emitted));
        if (end == std::string::npos) break;
        sentences.push_back(_decoded.substr(_emitted, end - _emitted));
        _spoken += sentences.back();
        _emitted = _scanned = end;
    }
    _scanned = _decoded.size();
    return sentences;
}

} // namespace llm
} // namespace va
