#pragma once

#include <VASDK/engine/ProviderRegistry.h>

namespace va {
namespace thirdparty {

/**
 * Registers every VAD, STT and TTS provider built into this binary under its config id
 * (VAD: silero; STT: gradium, soniox, deepgram, cartesia, fishaudio, gladia, whistle, moonshine,
 * parakeet; TTS: gradium, soniox, supertonic, deepgram, cartesia, fishaudio, moss, phonon,
 * pocket). Registration order is the default preference: a spec with an empty id gets the
 * first one built in. Options messages are in proto/vasdk/providers/options.proto.
 */
void registerAllProviders(engine::ProviderRegistry& registry);

} // namespace thirdparty
} // namespace va
