/*
 * VASDK_C_API marks the C API's functions (vasdk.h). They are what the C library, vasdk_c
 * (VASDK::c), exports, and nothing else: a binary interface that holds across releases of the
 * same major version (VASDK_C_API_VERSION, docs/reference/c-api.md).
 *
 *   VASDK_C_BUILDING  defined while VASDK itself is compiled: the functions are exported
 *   VASDK_C_STATIC    defined for code linked to a static VASDK: plain declarations
 *   neither           a program using the shared C library: imported (Windows), default visibility elsewhere
 */
#ifndef VASDK_C_VASDK_C_EXPORT_H
#define VASDK_C_VASDK_C_EXPORT_H

#if defined(VASDK_C_STATIC)
#  define VASDK_C_API
#elif defined(_WIN32) || defined(__CYGWIN__)
#  if defined(VASDK_C_BUILDING)
#    define VASDK_C_API __declspec(dllexport)
#  else
#    define VASDK_C_API __declspec(dllimport)
#  endif
#else
#  define VASDK_C_API __attribute__((visibility("default")))
#endif

#endif /* VASDK_C_VASDK_C_EXPORT_H */
