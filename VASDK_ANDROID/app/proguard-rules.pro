# Keep JNI-callable methods and annotated classes
-keepclasseswithmembernames class * {
    native <methods>;
}

-keep @androidx.annotation.Keep class * { *; }
-keepclassmembers class * {
    @androidx.annotation.Keep *;
}
