// The C entry point, an object of its own: an app linking the static catalog gets it, and the
// shared vasdk_providers library (ProvidersShared.cpp) defines its own, exported, instead.

#include "ProviderCatalog.h"

#include <VASDK/c/vasdk_cpp.h>

extern "C" void vasdk_providers_add_all(vasdk_providers* providers) {
    if (providers) va::thirdparty::registerAllProviders(vasdk_providers_registry(providers));
}
