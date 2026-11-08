package com.vasdk

import com.google.protobuf.MessageLite

/**
 * A provider's own options message (vasdk.providers.PhononOptions, ...) as the Any a
 * ProviderSpec's options hold, for an engine config built in code:
 *
 *     tts = providerSpec { id = "phonon"; options = phononOptions { temperature = 0.4f }.toAny() }
 *
 * Lite messages have no descriptor to name their type, so it comes from the generated class:
 * VASDK's messages set no java_package, so a message's class is <proto package>.<Message>.
 */
fun MessageLite.toAny(): com.google.protobuf.Any =
    com.google.protobuf.Any.newBuilder()
        .setTypeUrl("type.googleapis.com/" + javaClass.name.substringBeforeLast('.') + "." + javaClass.simpleName)
        .setValue(toByteString())
        .build()
