// The Kotlin binding over the C API (VASDK/include/VASDK/c/vasdk.h), by JNA. Messages are VASDK's
// .proto files as lite classes, as on Android. Tests load the library VASDK_LIBRARY names.
plugins {
    kotlin("jvm") version "2.2.10"
    id("com.google.protobuf") version "0.9.5"
}

// ctest builds out of the source tree: -Pvasdk.buildDir=<build>/bindings/kotlin/gradle
(findProperty("vasdk.buildDir") as String?)?.let { layout.buildDirectory.set(file(it)) }

group = "com.vasdk"
// The SDK's one version: the repository's VERSION file (docs/releasing.md)
version = rootDir.resolve("../../VERSION").readText().trim()

repositories { mavenCentral() }

val protobufVersion = "4.29.5"

dependencies {
    api("net.java.dev.jna:jna:5.15.0")
    api("com.google.protobuf:protobuf-kotlin-lite:$protobufVersion")
    api("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.9.0")
    api("org.jetbrains.kotlinx:kotlinx-serialization-json:1.7.3")
    testImplementation(kotlin("test"))
}

// VASDK's messages, and the providers' options (vasdk.providers.PhononOptions, ...) for a config built in code
sourceSets.main { proto { srcDir("../../VASDK/proto"); srcDir("../../VASDK_3P_IMPL/ProviderCatalog/proto") } }

protobuf {
    protoc { artifact = "com.google.protobuf:protoc:$protobufVersion" }
    generateProtoTasks {
        all().forEach { task ->
            task.builtins {
                named("java") { option("lite") }
                create("kotlin") { option("lite") }
            }
        }
    }
}

java {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
}

kotlin { compilerOptions { jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17) } }

tasks.test {
    useJUnitPlatform()
    jvmArgs("--enable-native-access=ALL-UNNAMED")   // JNA loads VASDK (JDK 22+ warns otherwise)
    // The library under test: ctest sets VASDK_LIBRARY; by hand, point it at build/VASDK/libVASDK.*
    environment("VASDK_LIBRARY", System.getenv("VASDK_LIBRARY") ?: "")
    testLogging { events("passed", "failed", "skipped"); showStandardStreams = true; exceptionFormat = org.gradle.api.tasks.testing.logging.TestExceptionFormat.FULL }
}
