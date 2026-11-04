#pragma once

#include <VASDK/engine/ProviderRegistry.h>

#include <jni.h>

/**
 * Registers the "voz" STT provider when it's built in: it runs in the Kotlin SDK
 * (com.vasdk.android.VozRecognizer), reached over JNI. Call on a JVM thread (nativeInit), where
 * the app's classes can be found.
 */
void registerVozProvider(va::engine::ProviderRegistry &registry, JavaVM *javaVm);
