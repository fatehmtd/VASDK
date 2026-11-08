// The C library without the third-party providers (VASDK_BUILD_THIRDPARTY_IMPL off): the provider
// catalog's entry point, with no provider to register. Apps add their own (vasdk_providers_add_*).

namespace va {
namespace engine {
class ProviderRegistry;
} // namespace engine
namespace thirdparty {
void registerAllProviders(engine::ProviderRegistry&) {}
} // namespace thirdparty
} // namespace va
