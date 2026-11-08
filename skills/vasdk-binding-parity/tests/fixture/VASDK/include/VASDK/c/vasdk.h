/* The fixture's C API */
typedef void (*vasdk_widget_fn)(void* user);   /* a callback type: not a function */
VASDK_EXPORT int vasdk_widget_start(void* widget);
VASDK_EXPORT void vasdk_widget_stop(void* widget);
VASDK_EXPORT void vasdk_widget_spin(void* widget);              /* no binding declares it */
VASDK_EXPORT char* vasdk_widget_start_json(void* widget);       /* left out: JSON twin */
