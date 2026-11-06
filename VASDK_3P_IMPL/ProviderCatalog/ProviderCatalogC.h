#pragma once

/* For C code: the providers ProviderCatalog builds in, for vasdk_session_create (VASDK/c/vasdk.h).
 * Link vasdk_provider_catalog, or load the shared vasdk_providers library at run time. */

#include <VASDK/c/vasdk.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Adds every provider va::thirdparty::registerAllProviders() registers. */
void vasdk_providers_add_all(vasdk_providers* providers);

#ifdef __cplusplus
}
#endif
