#pragma once

#include "FrameProcessor.h"
#include "Transport.h"
#include <vector>
#include <memory>

namespace va {
namespace pipeline {

/**
 * @brief Pipeline manager - chains processors and manages execution
 */
class VASDK_EXPORT Pipeline {
public:
    Pipeline() = default;
    ~Pipeline();

    /**
     * @brief Add processor to the pipeline
     */
    Pipeline& addProcessor(std::shared_ptr<FrameProcessor> processor);

    /**
     * @brief Set input transport
     */
    Pipeline& setInputTransport(std::shared_ptr<transport::Transport> transport);

    /**
     * @brief Set output transport
     */
    Pipeline& setOutputTransport(std::shared_ptr<transport::Transport> transport);

    /**
     * @brief Build the pipeline (chain all processors)
     */
    bool build();

    /**
     * @brief Start the entire pipeline
     */
    bool start();

    /**
     * @brief Stop the entire pipeline
     */
    void stop();

    /**
     * @brief Get all processors
     */
    const std::vector<std::shared_ptr<FrameProcessor>>& getProcessors() const { return _processors; }

private:
    std::vector<std::shared_ptr<FrameProcessor>> _processors;
    std::shared_ptr<transport::Transport> _inputTransport;
    std::shared_ptr<transport::Transport> _outputTransport;
    bool _built{false};
};

/**
 * @brief Pipeline builder with fluent API
 */
class VASDK_EXPORT PipelineBuilder {
public:
    static std::shared_ptr<Pipeline> create() {
        return std::make_shared<Pipeline>();
    }
};

} // namespace pipeline
} // namespace va
