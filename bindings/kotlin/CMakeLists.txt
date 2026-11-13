# The Kotlin binding (VASDK_BUILD_KOTLIN_BINDING): Gradle builds it and runs its tests against this
# build's VASDK, out of the source tree. Needs Gradle and a JDK 17 or newer; without them nothing is
# registered, with a warning.
#   vasdk_kotlin_jar   the JVM library, build/bindings/kotlin/gradle/libs/vasdk-<version>.jar
#   vasdk_kotlin       its tests (ctest): the garage agent's suite, providers and processors in Kotlin
find_program(VASDK_GRADLE gradle)
find_package(Java 17 COMPONENTS Development QUIET)
if(NOT VASDK_GRADLE OR NOT Java_FOUND)
    message(WARNING "Kotlin binding: Gradle or a JDK 17+ not found: vasdk_kotlin_jar and the vasdk_kotlin test are not registered")
    return()
endif()

set(_vasdk_gradle ${VASDK_GRADLE} --no-daemon -q -p ${CMAKE_CURRENT_SOURCE_DIR}
    --project-cache-dir ${CMAKE_CURRENT_BINARY_DIR}/gradle-cache -Pvasdk.buildDir=${CMAKE_CURRENT_BINARY_DIR}/gradle)

add_custom_target(vasdk_kotlin_jar COMMAND ${_vasdk_gradle} jar
                  COMMENT "Kotlin binding: the JVM library" VERBATIM)

if(VASDK_BUILD_TESTS)
    add_test(NAME vasdk_kotlin COMMAND ${_vasdk_gradle} test --rerun)
    vasdk_label_tests(binding vasdk_kotlin)
    set_tests_properties(vasdk_kotlin PROPERTIES
        ENVIRONMENT "VASDK_LIBRARY=$<TARGET_FILE:vasdk_c>;VASDK_GARAGE_DIR=${VASDK_ROOT_DIR}/VASDK_APP/agents/garage-menu"
        TIMEOUT 900)
endif()
