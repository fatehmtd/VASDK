// The providers as a shared library, for code that loads VASDK at run time (Python's ctypes):
// vasdk_providers_add_all, exported. VASDK itself is the shared library beside it.

#include <VASDK/c/vasdk.h>
#include <VASDK/c/vasdk_cpp.h>
#include <VASDK/engine/ProviderRegistry.h>

#include <spdlog/spdlog.h>

#include <string>

#if defined(_WIN32)
#define VASDK_PROVIDERS_API __declspec(dllexport)
#else
#define VASDK_PROVIDERS_API __attribute__((visibility("default")))
#endif

namespace va {
namespace thirdparty {
void registerAllProviders(engine::ProviderRegistry& registry);   // ProviderCatalog.h
} // namespace thirdparty
} // namespace va

extern "C" VASDK_PROVIDERS_API void vasdk_providers_add_all(vasdk_providers* providers) {
    if (providers) va::thirdparty::registerAllProviders(vasdk_providers_registry(providers));
}

// The providers log through a logger of this library's own: vasdk_set_log_level's levels.
extern "C" VASDK_PROVIDERS_API int vasdk_providers_set_log_level(const char* level) {
    if (!level) return 0;
    const auto parsed = spdlog::level::from_str(level);
    if (parsed == spdlog::level::off && std::string(level) != "off") return 0;
    spdlog::set_level(parsed);
    return 1;
}
