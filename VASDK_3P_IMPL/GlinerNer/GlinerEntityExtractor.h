#pragma once

#include <VASDK/nlu/EntityExtractor.h>

#include <memory>
#include <mutex>
#include <string>

namespace va {
namespace thirdparty::nlu {

/** Settings of GlinerEntityExtractor; every field but the model folder has a default. */
struct GlinerEntityExtractorOptions {
    /** Folder holding model.onnx, spm.model, added_tokens.json and gliner_config.json
     *  (VASDK_3P_IMPL/cmake/DownloadGlinerModel.cmake). */
    std::string modelDir;
    /** A span's score (0-1) must reach it to be kept. */
    float threshold = 0.5f;
};

/**
 * @brief GLiNER (span mode, markerV0) on ONNX Runtime: a zero-shot entity model told the agent's
 *        entity labels ("car repair", "available time"), finding where each is said. On device.
 *        Models: onnx-community/gliner_small-v2.1 int8, English, or gliner_multi-v2.1,
 *        multilingual; Apache-2.0.
 */
class GlinerEntityExtractor : public va::nlu::EntityExtractor {
public:
    explicit GlinerEntityExtractor(GlinerEntityExtractorOptions options);
    ~GlinerEntityExtractor() override;
    /** False, with the reason logged, when a file is missing or unreadable. */
    bool initialize();
    std::string name() const override { return "gliner"; }
    std::vector<va::nlu::EntitySpan> extract(const std::vector<std::string>& words, const std::vector<va::nlu::EntityLabel>& labels,
                                         bool sensitive) const override;

private:
    struct Model;
    std::string _modelDir;
    float _threshold;
    std::unique_ptr<Model> _model;
    mutable std::mutex _mutex;   // one inference at a time
};

} // namespace thirdparty::nlu
} // namespace va
