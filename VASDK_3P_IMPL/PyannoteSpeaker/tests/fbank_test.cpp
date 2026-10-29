// Parity of KaldiFbank with torchaudio.compliance.kaldi.fbank as used by pyannote's WeSpeaker
// front end. The golden is made by gen_fbank_golden.py from the same synthetic signal.
//
//   pyannote_fbank_test GOLDEN.txt

#include "KaldiFbank.h"

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
    size_t frames = 0, bins = 0;
    if (!(golden >> frames >> bins)) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }

    const size_t n = 4000;  // keep in sync with gen_fbank_golden.py
    const double pi = 3.14159265358979323846;
    std::vector<float> signal(n);
    for (size_t i = 0; i < n; ++i) {
        signal[i] = static_cast<float>(8000 * std::sin(2 * pi * 440 * i / 16000)
            + 3000 * std::sin(2 * pi * 1234.5 * i / 16000 + 0.3)
            + 200.0 * (static_cast<long>((i * 7919) % 101) - 50));
    }

    va::thirdparty::KaldiFbank fbank;
    auto features = fbank.compute(signal.data(), n);
    if (bins != va::thirdparty::KaldiFbank::kNumBins || features.size() != frames * bins) {
        std::fprintf(stderr, "shape mismatch: got %zu values, expected %zu x %zu\n", features.size(), frames, bins);
        return 1;
    }

    double maxDiff = 0.0, sumDiff = 0.0;
    for (size_t i = 0; i < features.size(); ++i) {
        double expected = 0.0;
        golden >> expected;
        const double diff = std::abs(expected - features[i]);
        maxDiff = std::max(maxDiff, diff);
        sumDiff += diff;
    }
    const double meanDiff = sumDiff / features.size();
    std::printf("abs diff vs torchaudio: max %g, mean %g\n", maxDiff, meanDiff);
    // float32 FFTs (pffft SIMD vs torch) differ most on the lowest-energy bins, hence the looser max.
    return (maxDiff < 5e-3 && meanDiff < 1e-4) ? 0 : 1;
}
