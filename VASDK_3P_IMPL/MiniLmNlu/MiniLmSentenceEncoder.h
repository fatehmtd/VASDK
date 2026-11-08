#pragma once

#include <VASDK/nlu/EmbeddingNluProvider.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace va {
namespace thirdparty::nlu {

/** Settings of MiniLmSentenceEncoder. */
struct MiniLmSentenceEncoderOptions {
    /** Folder holding model.onnx and vocab.txt (VASDK_3P_IMPL/cmake/DownloadMiniLmModel.cmake). */
    std::string modelDir;
    /** ONNX Runtime threads. */
    int threads = 1;
};

/**
 * @brief all-MiniLM-L6-v2 on ONNX Runtime: a sentence as a 384-number vector (mean of its tokens,
 *        length 1). About 23 MB int8; a few milliseconds a sentence on a phone.
 */
class MiniLmSentenceEncoder : public va::nlu::SentenceEncoder {
public:
    explicit MiniLmSentenceEncoder(MiniLmSentenceEncoderOptions options);
    ~MiniLmSentenceEncoder() override;
    /** False, with the reason logged, when the model or vocabulary can't be loaded. */
    bool initialize();
    std::string name() const override { return "minilm"; }
    std::vector<float> encode(const std::string& sentence) const override;
    /** Each word's vector in the sentence: the mean of its pieces' (length 1). Empty past 126 pieces. */
    std::vector<std::vector<float>> encodeWords(const std::vector<std::string>& words) const override;

    /** BERT's uncased WordPiece: [CLS] the pieces [SEP], as vocabulary ids (at most `maxTokens`). */
    std::vector<int64_t> tokenize(const std::string& sentence, size_t maxTokens = 128) const;

private:
    struct Model;
    // A word's WordPiece ids (lower case); [UNK] when it can't be split.
    std::vector<int64_t> pieces(const std::string& word) const;
    // The model's last hidden state for `ids`: tokens × size, row after row; empty on failure.
    std::vector<float> hidden(const std::vector<int64_t>& ids, size_t& size) const;
    std::string _modelDir;
    int _threads;
    std::map<std::string, int64_t> _vocabulary;
    std::unique_ptr<Model> _model;
    mutable std::mutex _mutex;   // one inference at a time
};

} // namespace thirdparty::nlu
} // namespace va
