#ifndef SMART_TURN_V3_IMPL_H
#define SMART_TURN_V3_IMPL_H

#include <string>
#include <vector>

#include <memory>
#include <limits>
#include <mutex>
#include <pffft/pffft.h>
#include <onnxruntime_cxx_api.h>
#ifdef VASDK_USE_NNAPI
#include <nnapi_provider_factory.h>
#endif

#include <spdlog/spdlog.h>

#ifdef _WIN32
#define _USE_MATH_DEFINES
#endif

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace va {
    namespace thirdparty {
        class SmartTurnV3Impl {
            public:
            SmartTurnV3Impl(const std::string& modelPath,
                int nFFT,
                int hopLength,
                int nMels,
                int maxBufferSize) : _modelPath(modelPath),
                _nFFT(nFFT),
                _hopLength(hopLength),
                _nMels(nMels),
                _maxBufferSize(maxBufferSize) {
            }

            ~SmartTurnV3Impl() = default;

            /**
             * @brief Initialize the SmartTurnV3 processor, including loading the ONNX model and setting up necessary buffers.
             */
            bool initialize(const va::audio::AudioConfig& audioConfig);

            /**
             * @brief Process incoming audio frames, compute features, and perform turn detection inference.
             * @return the probability score (between 0 and 1) that indicates the likelihood that the current speaker has finished their turn.
             */
            float processAudio(const float* buffer, int offset, int numSamples);

            // RAII deleters for PFFFT resources
            struct PFFTSetupDeleter {
                void operator()(PFFFT_Setup* s) const { if (s) pffft_destroy_setup(s); }
            };
            struct PFFTBufferDeleter {
                void operator()(float* p) const { pffft_aligned_free(p); }
            };

            private:
            std::string _modelPath;
            int _sampleRate = 16000;
            int _nFFT;
            int _hopLength;
            int _nMels;
            int _maxBufferSize;
            int _numFrames;
            std::vector<float> _workingBuffer;
            std::vector<float> _featuresBuffer;

            // ONNX Runtime environment and session for running the turn detection model
            Ort::Env _ortEnv;
            std::unique_ptr<Ort::Session> _ortSession;
            std::mutex _inferenceMutex; // guards _workingBuffer, _featuresBuffer, and _ortSession->Run()
            std::string _inputName, _outputName;

            // Pre-computed during initialize() - zero per-call heap allocation in hot path
            int _nFreqs = 0;
            int _fftPaddedSize = 0;
            int _maxNumFrames = 0;
            std::vector<float> _melFilterBankFlat;       // [_nMels × nFreqs], row-major
            std::vector<float> _hannWindow;               // [_nFFT]
            std::vector<float> _transposedFeaturesCache;  // [_nMels × (maxNumFrames-1)]
            std::vector<float> _powerSpectrumBuf;         // [nFreqs]
            std::vector<float> _tempLogMelBuf;            // [maxNumFrames × _nMels]
            std::unique_ptr<PFFFT_Setup,  PFFTSetupDeleter>  _pffftSetup;
            std::unique_ptr<float, PFFTBufferDeleter>        _fftInput;
            std::unique_ptr<float, PFFTBufferDeleter>        _fftOutput;
            std::unique_ptr<float, PFFTBufferDeleter>        _fftWork;

            /**
             * @brief Compute features from the audio frame using Whisper's feature extraction algorithm.
             */
            int computeWhisperFeatures(const float* audioData, int numSamples,
                int nFFT, int fftPaddedSize, int hopLength, int nMels,
                float* featuresBuffer, int featuresBufferSize);
        };

        // Slaney mel scale helper functions (used by HuggingFace Whisper)
        // Slaney uses linear scale below 1000 Hz, log scale above
        float hzToMelSlaney(float hz) {
            const float f_min = 0.0f;
            const float f_sp = 200.0f / 3.0f;  // ~66.67 Hz
            const float min_log_hz = 1000.0f;
            const float min_log_mel = (min_log_hz - f_min) / f_sp;
            const float logstep = std::log(6.4f) / 27.0f;

            if (hz >= min_log_hz) {
                return min_log_mel + std::log(hz / min_log_hz) / logstep;
            }
            else {
                return (hz - f_min) / f_sp;
            }
        }

        float melToHzSlaney(float mel) {
            const float f_min = 0.0f;
            const float f_sp = 200.0f / 3.0f;
            const float min_log_hz = 1000.0f;
            const float min_log_mel = (min_log_hz - f_min) / f_sp;
            const float logstep = std::log(6.4f) / 27.0f;

            if (mel >= min_log_mel) {
                return min_log_hz * std::exp(logstep * (mel - min_log_mel));
            }
            else {
                return f_min + f_sp * mel;
            }
        }

        /**
         * @brief Perform mean normalization and variance scaling on the input audio data, in-place.
         */
        void meanNormalize(float* data, int length) {
            float mean = 0.0f;
            for (int i = 0; i < length; ++i) {
                mean += data[i];
            }
            mean /= static_cast<float>(length);

            float variance = 0.0f;
            for (int i = 0; i < length; ++i) {
                float diff = data[i] - mean;
                variance += diff * diff;
            }
            variance /= static_cast<float>(length);

            float invStd = 1.0f / std::sqrt(variance + 1e-7f);
            for (int i = 0; i < length; ++i) {
                data[i] = (data[i] - mean) * invStd;
            }
        }

        bool SmartTurnV3Impl::initialize(const va::audio::AudioConfig& audioConfig)
        {
            _ortEnv = Ort::Env(ORT_LOGGING_LEVEL_WARNING, "SmartTurnV3");
            Ort::SessionOptions sessionOptions;
            sessionOptions.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
            sessionOptions.SetInterOpNumThreads(1);
            sessionOptions.SetIntraOpNumThreads(1);
            sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef VASDK_USE_NNAPI
            OrtSessionOptionsAppendExecutionProvider_Nnapi(sessionOptions, 0);
#endif
            // Load the ONNX model for turn detection
            try {
#ifdef _WIN32
                std::wstring wModelPath(_modelPath.begin(), _modelPath.end());
                _ortSession = std::make_unique<Ort::Session>(_ortEnv, wModelPath.c_str(), sessionOptions);
#else
                _ortSession = std::make_unique<Ort::Session>(_ortEnv, _modelPath.c_str(), sessionOptions);
#endif  
            }
            catch (const Ort::Exception& e) {
                spdlog::error("Failed to load ONNX model: {}", e.what());
                return false;
            }

            // Assumes a single input and single output.
            Ort::AllocatorWithDefaultOptions allocator;
            auto inputNameAlloc = _ortSession->GetInputNameAllocated(0, allocator);
            auto outputNameAlloc = _ortSession->GetOutputNameAllocated(0, allocator);
            _inputName = inputNameAlloc.get();
            _outputName = outputNameAlloc.get();

            _workingBuffer.reserve(_maxBufferSize);
            _workingBuffer.resize(_maxBufferSize, 0.0f);

            // Center-padded formula to match HuggingFace: numFrames = 1 + maxBufferSize / _hopLength.
            _numFrames = 1 + _maxBufferSize / _hopLength;
            const int featuresBufferSize = _numFrames * _nMels;

            _featuresBuffer.reserve(featuresBufferSize);
            _featuresBuffer.resize(featuresBufferSize, 0.0f);

            // Pre-compute FFT setup, Hann window, mel filterbank, and scratch buffers (done once)
            _fftPaddedSize = 480;
            _nFreqs        = _fftPaddedSize / 2 + 1;
            _maxNumFrames  = _numFrames; // 1 + _maxBufferSize / _hopLength

            _pffftSetup.reset(pffft_new_setup(_fftPaddedSize, PFFFT_REAL));
            if (!_pffftSetup) { return false; }
            _fftInput.reset(static_cast<float*>(pffft_aligned_malloc(_fftPaddedSize * sizeof(float))));
            _fftOutput.reset(static_cast<float*>(pffft_aligned_malloc(_fftPaddedSize * sizeof(float))));
            _fftWork.reset(static_cast<float*>(pffft_aligned_malloc(_fftPaddedSize * sizeof(float))));
            if (!_fftInput || !_fftOutput || !_fftWork) { return false; }

            // Hann window (_nFFT samples)
            _hannWindow.resize(_nFFT);
            for (int i = 0; i < _nFFT; ++i)
                _hannWindow[i] = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * i / (_nFFT - 1)));

            // Mel filterbank - Slaney normalization, row-major [_nMels x nFreqs]
            _melFilterBankFlat.assign(_nMels * _nFreqs, 0.0f);
            {
                const float kSR = 16000.0f;
                std::vector<float> melPts(_nMels + 2), hzPts(_nMels + 2);
                float melMin = hzToMelSlaney(0.0f), melMax = hzToMelSlaney(8000.0f);
                for (int i = 0; i < _nMels + 2; ++i) {
                    melPts[i] = melMin + (melMax - melMin) * i / (_nMels + 1);
                    hzPts[i]  = melToHzSlaney(melPts[i]);
                }
                for (int m = 0; m < _nMels; ++m) {
                    float lHz = hzPts[m], cHz = hzPts[m+1], rHz = hzPts[m+2];
                    float enorm = 2.0f / (rHz - lHz);
                    for (int k = 0; k < _nFreqs; ++k) {
                        float freq = k * kSR / (_fftPaddedSize + 1);
                        float v = 0.0f;
                        if (freq >= lHz && freq <= cHz && cHz > lHz)
                            v = (freq - lHz) / (cHz - lHz) * enorm;
                        else if (freq > cHz && freq <= rHz && rHz > cHz)
                            v = (rHz - freq) / (rHz - cHz) * enorm;
                        _melFilterBankFlat[m * _nFreqs + k] = v;
                    }
                }
            }

            // Per-call scratch buffers pre-sized to maximum (no realloc on hot path)
            _transposedFeaturesCache.assign(_nMels * (_maxNumFrames - 1), 0.0f);
            _powerSpectrumBuf.assign(_nFreqs, 0.0f);
            _tempLogMelBuf.assign(_maxNumFrames * _nMels, 0.0f);

            return true;
        }

        float SmartTurnV3Impl::processAudio(const float* buffer, int offset, int numSamples)
        {
            if (numSamples <= 0 || offset < 0 || buffer == nullptr) {
                spdlog::error("Invalid offset or length for processing audio frame.");
                return 0.0f;
            }

            std::lock_guard<std::mutex> lock(_inferenceMutex);

            // truncate input if it exceeds max buffer size, keeping the most recent audio
            if (numSamples > _maxBufferSize) {
                offset += numSamples - _maxBufferSize; // Adjust offset to get the last maxBufferSize samples
                numSamples = _maxBufferSize;
            }

            int internalOffset = _maxBufferSize - numSamples; // Where to place the new audio in the working buffer

            // copy the relevant portion of the input buffer into our working buffer for feature extraction
            memset(_workingBuffer.data(), 0, internalOffset * sizeof(float)); // Clear previous data
            memcpy(_workingBuffer.data() + internalOffset, buffer + offset, numSamples * sizeof(float));

            // Zero-mean unit-variance normalization, matching HuggingFace WhisperFeatureExtractor do_normalize=True.
            // Mean and variance are computed over the actual audio samples only (not the zero-padded region),
            // matching HuggingFace's use of attention_mask in zero_mean_unit_var_norm.
            // The zero-padded region at [0..internalOffset-1] is left as 0.0 (padding_value).
            {
                float* actualAudio = _workingBuffer.data() + internalOffset;
                meanNormalize(actualAudio, numSamples);
            }

            // Compute features using Whisper's algorithm (uses pre-computed member resources).
            int computedSize = computeWhisperFeatures(_workingBuffer.data(),
                _maxBufferSize,
                _nFFT,
                _fftPaddedSize,
                _hopLength,
                _nMels,
                _featuresBuffer.data(),
                static_cast<int>(_featuresBuffer.size()));

            // compute inference using ONNX Runtime with the computed features
            // Model expects input_features of shape [1, _nMels, numFrames], matching HuggingFace WhisperFeatureExtractor output.
            // Our features buffer is [numFrames, _nMels] (time-major), so transpose before inference.

            if (computedSize <= 0) {
                return 0.0f;
            }

            // Transpose features from [numFrames, _nMels] to [_nMels, numFrames] - reuse pre-allocated cache
            if (static_cast<size_t>(_nMels) * static_cast<size_t>(computedSize) > _transposedFeaturesCache.size()) {
                spdlog::error("[SmartTurnV3] Transposed features buffer too small: need {}x{} but have {}",
                              _nMels, computedSize, _transposedFeaturesCache.size());
                return 0.0f;
            }
            for (int f = 0; f < computedSize; ++f) {
                for (int m = 0; m < _nMels; ++m) {
                    _transposedFeaturesCache[m * computedSize + f] = _featuresBuffer[f * _nMels + m];
                }
            }

            // Input tensor shape: [1, _nMels, numFrames]
            std::array<int64_t, 3> inputShape = { 1, static_cast<int64_t>(_nMels), static_cast<int64_t>(computedSize) };
            Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
                memInfo,
                _transposedFeaturesCache.data(),
                static_cast<size_t>(_nMels * computedSize),
                inputShape.data(),
                inputShape.size());

            // Use cached input/output names (queried once during initialize())
            const char* inputNames[]  = { _inputName.c_str() };
            const char* outputNames[] = { _outputName.c_str() };

            // Run ONNX inference
            auto ortOutputs = _ortSession->Run(
                Ort::RunOptions{ nullptr },
                inputNames, &inputTensor, 1,
                outputNames, 1);

            // Extract sigmoid probability (model returns probability of turn completion)
            float probability = ortOutputs[0].GetTensorData<float>()[0];
            return probability;
        }

        int va::thirdparty::SmartTurnV3Impl::computeWhisperFeatures(
            const float* audioData, int numSamples,
            int nFFT, int fftPaddedSize, int hopLength, int nMels,
            float* featuresBuffer, int featuresBufferSize)
        {
            if (!audioData || !featuresBuffer || numSamples <= 0 || featuresBufferSize <= 0) {
                return -1;
            }

            // Calculate number of frames that will be produced.
            // HuggingFace WhisperFeatureExtractor uses center=True (pads n_fft/2 on each side),
            // which gives: numFrames = 1 + numSamples / _hopLength.
            // The last frame is then discarded (HuggingFace convention), so outputFrames = numFrames - 1.
            // Example: 8s * 16kHz = 128000 samples → 1 + 128000/160 = 801 → 800 output frames.
            const int numFrames = 1 + numSamples / hopLength;
            const int outputFrames = numFrames - 1;
            const int expectedSize = outputFrames * nMels;

            if (featuresBufferSize < expectedSize || outputFrames <= 0) {
                return -1; // Buffer too small or not enough samples
            }

            // Constants for Whisper mel-spectrogram (matching HuggingFace implementation)
            const float sampleRate = 16000.0f;
            const int   nFreqs     = fftPaddedSize / 2 + 1;

            // All expensive resources are pre-computed in initialize(): no per-call heap allocation.
            float*       fftInput  = _fftInput.get();
            float*       fftOutput = _fftOutput.get();
            float*       work      = _fftWork.get();
            PFFFT_Setup* fftSetup  = _pffftSetup.get();

            // Process each frame
            for (int frameIdx = 0; frameIdx < numFrames; ++frameIdx) {
                int frameStart = frameIdx * hopLength;

                // Extract and window the frame (center=True zero-padding).
                const int centerOffset = nFFT / 2;
                for (int i = 0; i < nFFT; ++i) {
                    int sampleIdx = frameStart + i - centerOffset;
                    if (sampleIdx >= 0 && sampleIdx < numSamples) {
                        fftInput[i] = audioData[sampleIdx] * _hannWindow[i];
                    }
                    else {
                        fftInput[i] = 0.0f;
                    }
                }
                // Zero-pad from _nFFT to fftPaddedSize for a valid PFFFT transform
                for (int i = nFFT; i < fftPaddedSize; ++i)
                    fftInput[i] = 0.0f;

                // Compute FFT
                pffft_transform_ordered(fftSetup, fftInput, fftOutput, work, PFFFT_FORWARD);

                // Power spectrum (PFFFT ordered layout)
                _powerSpectrumBuf[0]               = fftOutput[0]               * fftOutput[0];
                _powerSpectrumBuf[fftPaddedSize/2] = fftOutput[fftPaddedSize/2] * fftOutput[fftPaddedSize/2];
                for (int k = 1; k < fftPaddedSize / 2; ++k) {
                    float re = fftOutput[k];
                    float im = fftOutput[fftPaddedSize - k];
                    _powerSpectrumBuf[k] = re * re + im * im;
                }

                // Apply mel filterbanks - use flat row-major layout for cache efficiency
                for (int m = 0; m < nMels; ++m) {
                    float melEnergy = 0.0f;
                    const float* filterRow = _melFilterBankFlat.data() + m * nFreqs;
                    for (int k = 0; k < nFreqs; ++k)
                        melEnergy += _powerSpectrumBuf[k] * filterRow[k];
                    melEnergy = std::max(melEnergy, 1e-10f);
                    _tempLogMelBuf[frameIdx * nMels + m] = std::log10(melEnergy);
                }
            }

            // Post-processing: find global max over output frames
            float maxVal = -std::numeric_limits<float>::infinity();
            for (int i = 0; i < outputFrames * nMels; ++i)
                maxVal = std::max(maxVal, _tempLogMelBuf[i]);

            // Dynamic range compression + normalization
            int outIdx = 0;
            for (int frameIdx = 0; frameIdx < outputFrames; ++frameIdx) {
                for (int m = 0; m < nMels; ++m) {
                    float val = _tempLogMelBuf[frameIdx * nMels + m];
                    val = std::max(val, maxVal - 8.0f);
                    val = (val + 4.0f) / 4.0f;
                    featuresBuffer[outIdx++] = val;
                }
            }

            return outputFrames;
        }
    }
}

#endif