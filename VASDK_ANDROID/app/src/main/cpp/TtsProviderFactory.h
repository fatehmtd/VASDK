#pragma once

#include <VASDK/CommonAudioUtils.h>
#include <VASDK/GenericVoiceSynthesisProvider.h>

#include <memory>
#include <string>

struct TtsProviderParams {
    std::string providerId;
    std::string modelDir;
    std::string stylePath;
    std::string sonioxApiKey;
    std::string deepgramApiKey;
    std::string cartesiaApiKey;
    std::string cartesiaVoiceId;
    std::string fishaudioApiKey;
    std::string fishaudioVoiceId;
    std::string gradiumApiKey;
    std::string gradiumVoiceId;
    std::string caBundlePath;
    va::audio::AudioConfig outputAudioConfig;
};

// Builds the synthesis provider matching params.providerId ("soniox", "supertonic",
// "deepgram", "cartesia", "fishaudio", "gradium", anything else -> Pocket TTS). If the
// requested provider isn't compiled in, falls back through the rest in priority order
// (Pocket, Soniox, Supertonic, Deepgram, Cartesia, Fish Audio, Gradium). Returns nullptr
// only if none of the providers are compiled in at all.
std::shared_ptr<va::audio::GenericVoiceSynthesisProvider>
createSynthesisProvider(const TtsProviderParams &params);
