// Parity of SmartTurnV3Impl's feature extraction with pipecat smart-turn's reference preprocessing
// (left zero-padding + HuggingFace WhisperFeatureExtractor). The golden is made by
// gen_features_golden.py from the same synthetic signal.
//
//   smartturn_features_test GOLDEN.txt

#include <VASDK/CommonAudioUtils.h>
#include <cstring>
#include "SmartTurnV3Provider.h"
#include "SmartTurnV3Impl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s GOLDEN.txt\n", argv[0]);
        return 2;
    }
    std::ifstream golden(argv[1]);
    size_t frames = 0, mels = 0;
    if (!(golden >> frames >> mels)) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }

    const int n = 6000, maxLen = 8000;  // keep in sync with gen_features_golden.py
    const double pi = 3.14159265358979323846;
    std::vector<float> signal(n);
    for (int i = 0; i < n; ++i) {
        signal[i] = static_cast<float>((8000 * std::sin(2 * pi * 440 * i / 16000)
            + 3000 * std::sin(2 * pi * 1234.5 * i / 16000 + 0.3)
            + 200.0 * (static_cast<long>((i * 7919L) % 101) - 50)) / 32768);
    }

    va::thirdparty::SmartTurnV3Impl impl("", va::thirdparty::whisper::N_FFT, va::thirdparty::whisper::HOP_LENGTH,
        va::thirdparty::whisper::N_MELS, maxLen);
    if (!impl.initializeFeatures()) {
        std::fprintf(stderr, "initializeFeatures failed\n");
        return 1;
    }
    const int computed = impl.computeFeatures(signal.data(), 0, n);
    if (computed != static_cast<int>(frames) || mels != static_cast<size_t>(va::thirdparty::whisper::N_MELS)) {
        std::fprintf(stderr, "shape mismatch: got %d frames, expected %zu x %zu\n", computed, frames, mels);
        return 1;
    }

    const auto& features = impl.getFeatures();
    double maxDiff = 0.0, sumDiff = 0.0;
    for (size_t i = 0; i < frames * mels; ++i) {
        double expected = 0.0;
        golden >> expected;
        const double diff = std::abs(expected - features[i]);
        maxDiff = std::max(maxDiff, diff);
        sumDiff += diff;
    }
    const double meanDiff = sumDiff / (frames * mels);
    std::printf("abs diff vs WhisperFeatureExtractor: max %g, mean %g\n", maxDiff, meanDiff);
    return (maxDiff < 5e-3 && meanDiff < 1e-4) ? 0 : 1;
}
