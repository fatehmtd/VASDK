#pragma once

#include <VASDK/CommonAudioUtils.h>
#include <VASDK/GenericTranscriptionProvider.h>

#include <memory>
#include <string>

struct SttProviderParams {
    std::string providerId;
    std::string modelDir;
    std::string sonioxApiKey;
    std::string deepgramApiKey;
    std::string gladiaApiKey;
    std::string cartesiaApiKey;
    std::string fishaudioApiKey;
    std::string gradiumApiKey;
    std::string caBundlePath;
    va::audio::AudioConfig audioConfig;
    int frameSize{0};
};

// Builds the transcription provider matching params.providerId ("soniox", "deepgram",
// "gladia", "cartesia", "fishaudio", "gradium", anything else -> Parakeet). If the
// requested provider isn't compiled in, falls back through the rest in priority order
// (Parakeet, Soniox, Deepgram, Gladia, Cartesia, Fish Audio, Gradium). Returns nullptr
// only if none of the providers are compiled in at all.
std::shared_ptr<va::audio::GenericTranscriptionProvider>
createTranscriptionProvider(const SttProviderParams &params);
