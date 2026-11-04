#pragma once

#include <VASDK/nlu/EmbeddingNluProvider.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace va {
namespace thirdparty {

/**
 * @brief all-MiniLM-L6-v2 on ONNX Runtime: a sentence as a 384-number vector (mean of its tokens,
 *        length 1). About 23 MB int8; a few milliseconds a sentence on a phone. `modelDir` holds
 *        model.onnx and vocab.txt (cmake/DownloadMiniLmModel.cmake fetches them).
 */
class MiniLmSentenceEncoder : public nlu::SentenceEncoder {
public:
    explicit MiniLmSentenceEncoder(const std::string& modelDir);
    ~MiniLmSentenceEncoder() override;
    /** False, with the reason logged, when the model or vocabulary can't be loaded. */
    bool initialize();
    std::string name() const override { return "minilm"; }
    std::vector<float> encode(const std::string& sentence) const override;

    /** BERT's uncased WordPiece: [CLS] the pieces [SEP], as vocabulary ids (at most `maxTokens`). */
    std::vector<int64_t> tokenize(const std::string& sentence, size_t maxTokens = 128) const;

private:
    struct Model;
    std::string _modelDir;
    std::map<std::string, int64_t> _vocabulary;
    std::unique_ptr<Model> _model;
    mutable std::mutex _mutex;   // one inference at a time
};

} // namespace thirdparty
} // namespace va
