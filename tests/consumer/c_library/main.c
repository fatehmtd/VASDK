/* Uses the installed C library from C: its headers, its version, and its providers. */
#include <VASDK/c/vasdk.h>
#include <VASDK/c/vasdk_providers.h>
#include <VASDK/version.h>

#include <stdio.h>
#include <string.h>

int main(void) {
    const char* version = vasdk_version();
    if (!version || strcmp(version, VASDK_VERSION_STRING) != 0) {
        fprintf(stderr, "vasdk_version() is '%s', the header says '%s'\n", version ? version : "(null)", VASDK_VERSION_STRING);
        return 1;
    }
    if (VASDK_C_API_VERSION < 1) {
        fprintf(stderr, "VASDK_C_API_VERSION is %d\n", VASDK_C_API_VERSION);
        return 1;
    }
    vasdk_providers* providers = vasdk_providers_create();
    if (!providers) {
        fprintf(stderr, "vasdk_providers_create() gave NULL\n");
        return 1;
    }
    vasdk_providers_add_all(providers);   /* every provider built in */
    vasdk_providers_free(providers);
    printf("VASDK %s, C API %d: ok\n", version, VASDK_C_API_VERSION);
    return 0;
}
