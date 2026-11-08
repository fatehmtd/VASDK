#pragma once

/* For C code: the providers ProviderCatalog builds in, for vasdk_session_create (VASDK/c/vasdk.h).
 * Link vasdk_provider_catalog, or load the shared vasdk_providers library at run time. */

#include <VASDK/c/vasdk.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Adds every provider va::thirdparty::registerAllProviders() registers. */
void vasdk_providers_add_all(vasdk_providers* providers);
/* The shared vasdk_providers library only: how much its providers log, as vasdk_set_log_level
 * (its own logger, apart from VASDK's). 0 for a level it doesn't know. */
int vasdk_providers_set_log_level(const char* level);

#ifdef __cplusplus
}
#endif
