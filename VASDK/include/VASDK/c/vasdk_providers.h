/*
 * The providers built into the C library (vasdk_c, VASDK::c): VAD, STT, TTS, turn detection and the
 * rest, picked by id from an engine config (docs/provider-catalog.md). Which ones are in depends on
 * how the library was built (its flavor: docs/integrating.md).
 *
 *   vasdk_providers* providers = vasdk_providers_create();
 *   vasdk_providers_add_all(providers);
 *   vasdk_session* session = vasdk_session_create("engine.txtpb", providers, ...);
 *
 * Also in a program linked to the C++ libraries (VASDK and vasdk_provider_catalog).
 */
#ifndef VASDK_C_VASDK_PROVIDERS_H
#define VASDK_C_VASDK_PROVIDERS_H

#include "vasdk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Adds every provider built in (va::thirdparty::registerAllProviders). */
void vasdk_providers_add_all(vasdk_providers* providers);
/* How much the providers log, as vasdk_set_log_level. 0 for a level it doesn't know. */
int vasdk_providers_set_log_level(const char* level);

#ifdef __cplusplus
}
#endif

#endif /* VASDK_C_VASDK_PROVIDERS_H */
