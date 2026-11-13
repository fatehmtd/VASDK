#!/bin/sh
# Runs the ctest suite of an Android build (VASDK_BUILD_TESTS=ON) on a device or emulator over adb.
#   run-tests-on-device.sh <build dir> [models dir] [test name regex]
# The build dir is a CMake build of VASDK_ANDROID for the device's ABI, for instance:
#   cmake -S VASDK_ANDROID -B build/android-x86_64 -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=x86_64 \
#     -DANDROID_PLATFORM=android-33 -DCMAKE_BUILD_TYPE=Release -DVASDK_BUILD_TESTS=ON
#   cmake --build build/android-x86_64
# Each test runs as ctest would run it, from /data/local/tmp/vasdk: the paths in its command and
# the source paths compiled into it (golden files, entity packs) are pushed to the same paths
# under that folder, so nothing needs rewriting. With a models dir (Silero, Whistle, Pocket,
# Phonon), the offline conversation runs; without one it's skipped (exit 77), as on the desktop.
# Needs ANDROID_NDK (for libomp.so) and ctest on the PATH (or CTEST=<path>).
set -u
export MSYS_NO_PATHCONV=1   # Git Bash on Windows: leave /data/local/tmp as it is...
host() { cygpath -m "$1" 2>/dev/null || echo "$1"; }   # ...and give adb.exe D:/ paths
BUILD=$(host "$(cd "$1" && pwd)")
MODELS=${2:-}
FILTER=${3:-.}
SOURCE=$(host "$(cd "$(dirname "$0")/.." && pwd)")
DEVICE=/data/local/tmp/vasdk
CTEST=${CTEST:-ctest}
ABI=$(adb shell getprop ro.product.cpu.abi | tr -d '\r')
case "$ABI" in
    x86_64) OMP_ARCH=x86_64 ;;
    arm64-v8a) OMP_ARCH=aarch64 ;;
    *) echo "unsupported ABI $ABI"; exit 2 ;;
esac
on_device() { echo "$DEVICE/$(echo "$1" | sed 's#\\#/#g; s#^/##')"; }   # a host path, as pushed

# The source tree as the build saw it (D:/GitHub/... on Windows).
SOURCE_SEEN=$(sed -n 's#^CMAKE_HOME_DIRECTORY:INTERNAL=\(.*\)/VASDK_ANDROID$#\1#p' "$BUILD/CMakeCache.txt")

adb shell "mkdir -p $DEVICE/lib"
adb push -q "$BUILD/_deps/onnxruntime-src/jni/$ABI/libonnxruntime.so" "$DEVICE/lib/" >/dev/null
omp=$(host "$(find "${ANDROID_NDK:?set ANDROID_NDK}" -path "*linux/$OMP_ARCH/libomp.so" | head -1)")
adb push -q "$omp" "$DEVICE/lib/" >/dev/null
for folder in VASDK/entities VASDK/tests VASDK_3P_IMPL/PhononTTS/tests/golden VASDK_3P_IMPL/MossTTS/tests/golden \
              VASDK_3P_IMPL/SmartTurnV3/tests/golden VASDK_3P_IMPL/Turn1Mini/tests/golden               VASDK_3P_IMPL/PyannoteSpeaker/tests/golden; do
    target=$(dirname "$(on_device "$SOURCE_SEEN/$folder")")
    adb shell "mkdir -p '$target'"
    adb push -q "$SOURCE/$folder" "$target/" >/dev/null
done

"$CTEST" --test-dir "$BUILD" -N -V 2>/dev/null | sed -n 's/^ *[0-9]*: Test command: //p' | tr -d '\r' > "$BUILD/device-tests.txt"
while IFS= read -r line; do
    executable=$(echo "$line" | awk '{print $1}' | sed 's#\\#/#g')
    arguments=$(echo "$line" | cut -s -d' ' -f2-)
    name=$(basename "$executable")
    echo "$name" | grep -Eq "$FILTER" || continue
    [ -f "$executable" ] || { echo "SKIP $name (not built)"; continue; }
    if [ "$name" = vasdk_offline_conversation_test ] && [ -n "$MODELS" ]; then
        target=$(on_device "$(echo "$arguments" | tr -d '"' | sed 's/^--vasdk-models-dir=//')")
        adb shell "mkdir -p '$target'" < /dev/null
        for model in silero_vad.onnx smart_turn_v3.onnx turn_1_mini.onnx whistle parakeet pocket_tts phonon_tts; do
            [ -e "$MODELS/$model" ] || continue
            adb shell "[ -e '$target/$model' ]" < /dev/null || adb push -q "$MODELS/$model" "$target/" < /dev/null >/dev/null
        done
    fi
    # turn-1-mini's own test takes the model's path (the build downloads no model for Android)
    if [ "$name" = turn1mini_test ] && [ -e "$MODELS/turn_1_mini.onnx" ]; then
        target=$(on_device "$(echo "$arguments" | awk '{print $1}' | tr -d '"')")
        adb shell "mkdir -p '$(dirname "$target")'" < /dev/null
        adb push -q "$MODELS/turn_1_mini.onnx" "$target" < /dev/null >/dev/null
    fi
    remote=$(on_device "$executable")
    adb shell "mkdir -p '$(dirname "$remote")'" < /dev/null
    adb push -q "$executable" "$remote" < /dev/null >/dev/null
    output=$(adb shell "cd $DEVICE && chmod 755 '$remote' && LD_LIBRARY_PATH=$DEVICE/lib '$remote' $arguments; echo exit=\$?" < /dev/null 2>&1 | tr -d '\r')
    adb shell "rm -f '$remote'" < /dev/null   # statically linked, tens of MB each: an emulator fills up
    code=$(echo "$output" | sed -n 's/^exit=//p' | tail -1)
    case "$code" in
        0) echo "PASS $name" ;;
        77) echo "SKIP $name: $(echo "$output" | grep -m1 skipped)" ;;
        *) echo "FAIL $name (exit $code)"; echo "$output" | grep -v '^exit=' | tail -15 | sed 's/^/    /' ;;
    esac
done < "$BUILD/device-tests.txt"
