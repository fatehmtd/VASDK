#include "VASDK/Pipeline.h"
#include <spdlog/spdlog.h>

namespace va {
namespace pipeline {

Pipeline::~Pipeline() {
    stop();
}

namespace {
std::string jsonString(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
        else out += c;
    }
    return out + '"';
}
} // namespace

std::string Pipeline::describeAsJson() const {
    std::string out = "{\"processors\":[";
    for (size_t i = 0; i < _processors.size(); ++i) {
        if (i) out += ',';
        out += "{\"name\":" + jsonString(_processors[i]->getName()) +
               ",\"provider\":" + jsonString(_processors[i]->getProviderName()) + '}';
    }
    return out + "]}";
}

Pipeline& Pipeline::addProcessor(std::shared_ptr<FrameProcessor> processor) {
    if (processor) {
        _processors.push_back(processor);
    }
    return *this;
}

Pipeline& Pipeline::setInputTransport(std::shared_ptr<transport::Transport> transport) {
    _inputTransport = transport;
    return *this;
}

Pipeline& Pipeline::setOutputTransport(std::shared_ptr<transport::Transport> transport) {
    _outputTransport = transport;
    return *this;
}

bool Pipeline::build() {
    if (_processors.empty()) {
        spdlog::error("[Pipeline] No processors to build");
        return false;
    }

    for (size_t i = 0; i < _processors.size() - 1; ++i) {
        _processors[i]->setDownstream(_processors[i + 1]);
    }

    for (auto& processor : _processors) {
        if (!processor->initialize()) {
            spdlog::error("[Pipeline] Failed to initialize processor: {}", processor->getName());
            return false;
        }
    }

    _built = true;
    spdlog::info("[Pipeline] Built with {} processors", _processors.size());
    return true;
}

bool Pipeline::start() {
    if (!_built) {
        spdlog::error("[Pipeline] Pipeline not built");
        return false;
    }

    for (auto& processor : _processors) {
        if (!processor->start()) {
            spdlog::error("[Pipeline] Failed to start processor: {}", processor->getName());
            return false;
        }
    }

    spdlog::info("[Pipeline] Started");
    return true;
}

void Pipeline::stop() {
    for (auto it = _processors.rbegin(); it != _processors.rend(); ++it) {
        (*it)->stop();
    }
    spdlog::info("[Pipeline] Stopped");
}

} // namespace pipeline
} // namespace va
