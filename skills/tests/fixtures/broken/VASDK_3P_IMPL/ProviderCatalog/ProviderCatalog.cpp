// Fixture: "silero" is named in a skill, "ghost" in none.
void registerAll(Registry& registry) {
    registry.add<Vad, providers::SileroOptions>("silero", [](auto&, auto&) { return nullptr; });
    registry.add<Tts, providers::CloudOptions>("ghost", [](auto&, auto&) { return nullptr; });
}
