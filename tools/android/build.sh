#!/usr/bin/env bash
# Android (arm64) APK of one game, for handhelds such as the AYN Odin 2:
#   tools/android/build.sh <trials_hd|trials_evolution|trials_fusion>  ->  out/android/<game>/package/<game>.apk
#
# Needs: Android NDK r29 ($ANDROID_NDK), the Android SDK with platform 35 and build-tools 35.0.0 ($ANDROID_SDK), a JDK,
# and games/<game>/generated from build-linux.sh (this never runs the code generation). Game files are not in the APK.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
GAME="${1:?usage: tools/android/build.sh <game>}"
case "$GAME" in
  trials_hd) APP_NAME="Trials HD" ;;
  trials_evolution) APP_NAME="Trials Evolution" ;;
  trials_fusion) APP_NAME="Trials Fusion" ;;
  *) echo "unknown game: $GAME" >&2; exit 1 ;;
esac
ANDROID_NDK="${ANDROID_NDK:-$HOME/Android/android-ndk-r29}"
ANDROID_SDK="${ANDROID_SDK:-$HOME/Android/sdk}"
BUILD_TOOLS="$ANDROID_SDK/build-tools/35.0.0"
PLATFORM_JAR="$ANDROID_SDK/platforms/android-35/android.jar"
LLVM="$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64"
SDL_PROJECT=third_party/rexglue-sdk/thirdparty/sdl3/android-project
OUT="out/android/$GAME"
BUILD="$OUT/build"
WORK="$OUT/apk"
PACKAGE="$OUT/package"
KEYSTORE="${KEYSTORE:-$HOME/Android/trials-debug.keystore}"
VERSION="$(cat VERSION)"
# ARMv8.2 (Snapdragon 845 and later): LSE atomics instead of LL/SC loops.
ARCH_FLAGS="-march=armv8.2-a"

if [[ ! -f games/$GAME/generated/default/sources.cmake ]]; then
  echo "games/$GAME/generated is missing: run build-linux.sh first" >&2
  exit 1
fi

# 1. Native libraries.
cmake -S tools/android -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DTRIALS_GAME="$GAME" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_shared \
  "-DCMAKE_C_FLAGS=$ARCH_FLAGS" "-DCMAKE_CXX_FLAGS=$ARCH_FLAGS" \
  -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_PERF_COUNTERS=OFF
cmake --build "$BUILD" --target main

rm -rf "$WORK" && mkdir -p "$WORK/lib/arm64-v8a" "$WORK/classes" "$WORK/res" "$WORK/assets" "$WORK/res-names/values"
LIBS="$WORK/lib/arm64-v8a"
cp "$BUILD/libmain.so" "$BUILD/librexgpu-xenos.so" "$LIBS/"
# The SDK writes the runtime into its own out/<platform> folder.
cp third_party/rexglue-sdk/out/linux-arm64/librexruntime.so "$LIBS/"
cp "$LLVM/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$LIBS/"
cp "$BUILD"/adrenotools/src/hook/*.so "$LIBS/"
"$LLVM/bin/llvm-strip" --strip-unneeded "$LIBS"/*.so

# 2. Java: SDL's activity and helpers, plus ours.
javac -source 11 -target 11 -nowarn -encoding UTF-8 -cp "$PLATFORM_JAR" -d "$WORK/classes" \
  $(find "$SDL_PROJECT/app/src/main/java" tools/android/java -name '*.java')
"$BUILD_TOOLS/d8" --release --min-api 29 --lib "$PLATFORM_JAR" --output "$WORK" $(find "$WORK/classes" -name '*.class')

# 3. Settings: the app writes them to the game folder if there are none.
cp tools/android/android-overrides.toml "$WORK/assets/settings.toml"

# 4. Resources (SDL's, with ours on top) and the manifest.
sed "s/@GAME@/$GAME/; s/@VERSION@/$VERSION/" tools/android/AndroidManifest.xml.in > "$WORK/AndroidManifest.xml"
sed "s/@APP_NAME@/$APP_NAME/" tools/android/res/values/strings.xml > "$WORK/res-names/values/strings.xml"
cp -r tools/android/res/values-v28 "$WORK/res-names/"
"$BUILD_TOOLS/aapt2" compile --dir "$SDL_PROJECT/app/src/main/res" -o "$WORK/res/sdl.zip"
"$BUILD_TOOLS/aapt2" compile --dir "$WORK/res-names" -o "$WORK/res/trials.zip"
"$BUILD_TOOLS/aapt2" link -o "$WORK/unsigned.apk" -I "$PLATFORM_JAR" -A "$WORK/assets" \
  --manifest "$WORK/AndroidManifest.xml" "$WORK/res/sdl.zip" -R "$WORK/res/trials.zip"

# 5. Code and native libraries into the APK, align, sign.
python3 - "$WORK" <<'PY'
import os, sys, zipfile
work = sys.argv[1]
with zipfile.ZipFile(os.path.join(work, 'unsigned.apk'), 'a', zipfile.ZIP_DEFLATED) as apk:
    apk.write(os.path.join(work, 'classes.dex'), 'classes.dex')
    for name in sorted(os.listdir(os.path.join(work, 'lib/arm64-v8a'))):
        apk.write(os.path.join(work, 'lib/arm64-v8a', name), 'lib/arm64-v8a/' + name)
PY
"$BUILD_TOOLS/zipalign" -f -p 4 "$WORK/unsigned.apk" "$WORK/aligned.apk"
if [[ ! -f "$KEYSTORE" ]]; then
  keytool -genkeypair -keystore "$KEYSTORE" -storepass android -keypass android \
    -alias trials -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=trials debug" >/dev/null
fi
rm -rf "$PACKAGE" && mkdir -p "$PACKAGE"
"$BUILD_TOOLS/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
  --out "$PACKAGE/$GAME.apk" "$WORK/aligned.apk"
cp tools/android/README.md "$PACKAGE/README.md"
echo "APK ready: $PACKAGE/$GAME.apk"
