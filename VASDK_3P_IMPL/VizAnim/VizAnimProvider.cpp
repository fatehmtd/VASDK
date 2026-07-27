#include "VizAnimProvider.h"
#include "impl/VizAnimImpl.h"

#include <VASDK/AudioSIMD.h>
#include <VASDK/profiling/CTrack.h>
#include <cstring>

namespace va {
    namespace thirdparty {

        // 
        // Lifecycle
        // 

        VizAnimProvider::VizAnimProvider(int fftSize)
            : _impl(nullptr), _fftSize(fftSize)
        {
        }

        VizAnimProvider::~VizAnimProvider() = default;

        std::string VizAnimProvider::getName() const
        {
            return "VizAnimProvider";
        }

        bool VizAnimProvider::initialize(const va::audio::AudioConfig& config)
        {
            _config = config;
            _impl = std::make_unique<VizAnimImpl>(config.sampleRate, _fftSize);
            _sampleBuf.clear();
            _convertBuf.clear();
            return static_cast<bool>(_impl);
        }

        void VizAnimProvider::release()
        {
            _impl.reset();
            _sampleBuf.clear();
            _convertBuf.clear();
        }

        // 
        // processAudio - pass-through DSP 
        // 

        size_t VizAnimProvider::processAudio(const char* input, char* output,
            size_t numSamples)
        {
            return 0;
        }

        std::vector<VizAnimProvider::Features> VizAnimProvider::computeFeatures(const char* input, int numSamples) const
        {
            VASDK_CTRACK_NAME("VizAnim::computeFeatures");
            if (!_impl || input == nullptr || numSamples == 0) return {};

            //  Convert input bytes → normalised float [-1, 1] 
            const size_t buffSize = std::max(_convertBuf.size(), static_cast<size_t>(numSamples));
            _convertBuf.resize(buffSize);
            float* dst = _convertBuf.data();
            int dstOffset = 0;

            switch (_config.format) {
            case va::audio::AudioFormat::FLOAT:
            std::memcpy(dst, input, numSamples * sizeof(float));
            break;
            case va::audio::AudioFormat::SHORT:
            AudioSIMD::convertInt16ToFloat(
                reinterpret_cast<const int16_t*>(input), dst, numSamples);
            break;
            case va::audio::AudioFormat::INT:
            AudioSIMD::convertInt32ToFloat(
                reinterpret_cast<const int32_t*>(input), dst, numSamples);
            break;
            case va::audio::AudioFormat::BYTE:
            AudioSIMD::convertByteToFloat(
                reinterpret_cast<const uint8_t*>(input), dst, numSamples);
            break;
            default:
            return {};
            }

            _sampleBuf.resize(_convertBuf.size());

            if (_config.channels == 1) {
                std::copy(_convertBuf.begin() + static_cast<int>(dstOffset),
                    _convertBuf.end(),
                    _sampleBuf.begin());
            }
            else {
                return {};
            }

            //  Extract features whenever a full analysis window is available 
            const int fftSize = _impl->fftSize();
            int numFeatures = static_cast<int>(_sampleBuf.size()) / fftSize;

            std::vector<VizAnimProvider::Features> features;
            features.reserve(numFeatures);

            for(int offset = 0; offset < numFeatures; ++offset) {
                features.push_back(_impl->extract(_sampleBuf.data() + offset * fftSize));
            }

            return features;
        }

    } // namespace thirdparty
} // namespace va