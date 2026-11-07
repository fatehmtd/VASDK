#pragma once

#include <VASDK/nlu/EntityExtractor.h>

#include <memory>
#include <mutex>
#include <string>

namespace va {
namespace thirdparty {

/**
 * @brief GLiNER (span mode, markerV0) on ONNX Runtime: a zero-shot entity model told the agent's
 *        entity labels ("car repair", "available time"), finding where each is said. On device.
 *        `modelDir` holds model.onnx, spm.model, added_tokens.json and gliner_config.json
 *        (cmake/DownloadGlinerModel.cmake: onnx-community/gliner_small-v2.1 int8, English, or
 *        gliner_multi-v2.1, multilingual; Apache-2.0). Spans under `threshold` (0 to 1) are dropped.
 */
class GlinerEntityExtractor : public nlu::EntityExtractor {
public:
    GlinerEntityExtractor(const std::string& modelDir, float threshold = 0.5f);
    ~GlinerEntityExtractor() override;
    /** False, with the reason logged, when a file is missing or unreadable. */
    bool initialize();
    std::string name() const override { return "gliner"; }
    std::vector<nlu::EntitySpan> extract(const std::vector<std::string>& words, const std::vector<nlu::EntityLabel>& labels,
                                         bool sensitive) const override;

private:
    struct Model;
    std::string _modelDir;
    float _threshold;
    std::unique_ptr<Model> _model;
    mutable std::mutex _mutex;   // one inference at a time
};

} // namespace thirdparty
} // namespace va
