#!/usr/bin/env bash
# Builds the collection from your own Xbox Live Arcade packages, on Linux (x86-64).
#
#   ./build-linux.sh --packages <folder>                   launcher and every game found, for Linux and the Steam Deck
#   ./build-linux.sh --packages <folder> --target android  one APK per game found
#   ./build-linux.sh --game trials_hd                      only this game (repeat it for more)
#   ./build-linux.sh --launcher-only                       only the launcher; needs no game
#   ./build-linux.sh --check                               only check what is needed
#
# Each package is one file from your console, Content/0000000000000000/<title ID>/000D0000. Only default.xex is taken
# from it for the code generation; game files never enter the repository or a package built here.
# Linux builds run in a container (podman or docker, tools/linux/Containerfile) so that they run on the Steam Deck.
# Android needs the NDK r29 and SDK listed in BUILDING.md.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
ROOT="$PWD"

TARGET=linux PACKAGES="" CHECK=0 LAUNCHER_ONLY=0 GAMES=()
usage() { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
while [[ $# -gt 0 ]]; do
  case "$1" in
    --packages) PACKAGES="$2"; shift ;;
    --target) TARGET="$2"; shift ;;
    --game) GAMES+=("$2"); shift ;;
    --launcher-only) LAUNCHER_ONLY=1 ;;
    --check) CHECK=1 ;;
    -h|--help) usage ;;
    *) echo "unknown option: $1" >&2; usage 1 ;;
  esac
  shift
done
[[ $TARGET == deck ]] && TARGET=linux
[[ $TARGET == linux || $TARGET == android ]] || { echo "unknown target: $TARGET (linux, deck, android)" >&2; exit 1; }

title_id() {
  case "$1" in
    trials_hd) echo 5841095A ;;
    trials_evolution) echo 58410AAD ;;
    trials_fusion) echo 58411401 ;;
    *) echo "unknown game: $1 (trials_hd, trials_evolution, trials_fusion)" >&2; exit 1 ;;
  esac
}
[[ ${#GAMES[@]} -eq 0 ]] && GAMES=(trials_hd trials_evolution trials_fusion)
for game in "${GAMES[@]}"; do title_id "$game" >/dev/null; done

ENGINE="$(command -v podman || command -v docker || true)"
IMAGE=trials-linux-build
missing=()
[[ -n $ENGINE ]] || missing+=("podman or docker")
command -v git >/dev/null || missing+=(git)
if [[ $TARGET == android ]]; then
  ANDROID_NDK="${ANDROID_NDK:-$HOME/Android/android-ndk-r29}"
  ANDROID_SDK="${ANDROID_SDK:-$HOME/Android/sdk}"
  [[ -d $ANDROID_NDK ]] || missing+=("Android NDK r29 (ANDROID_NDK)")
  [[ -d $ANDROID_SDK/platforms/android-35 ]] || missing+=("Android SDK platform 35 (ANDROID_SDK)")
  [[ -d $ANDROID_SDK/build-tools/35.0.0 ]] || missing+=("Android build-tools 35.0.0")
  command -v javac >/dev/null || missing+=("a JDK")
fi
if (( ${#missing[@]} )); then
  printf 'missing: %s\n' "${missing[@]}" >&2
  exit 1
fi
(( CHECK )) && { echo "ready to build for $TARGET"; exit 0; }

in_container() {
  "$ENGINE" run --rm --userns=keep-id -v "$ROOT:/work:Z" -w /work "$IMAGE" bash -euc "$1"
}

tools/setup_sdk.sh
"$ENGINE" image exists "$IMAGE" 2>/dev/null || "$ENGINE" build -t "$IMAGE" tools/linux

echo "== launcher and tools"
in_container '
  cmake -S . -B out/launcher -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >/dev/null
  cmake --build out/launcher'

echo "== SDK and code generator"
in_container '
  cmake -S third_party/rexglue-sdk -B out/sdk-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 \
    -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_PERF_COUNTERS=OFF >/dev/null
  cmake --build out/sdk-linux --target rexglue rexgpu-xenos'

DIST=dist/TrialsCollection-linux-x64
if [[ $TARGET == linux ]]; then
  rm -rf "$DIST" && mkdir -p "$DIST"
  cp out/launcher/trials_launcher VERSION "$DIST/"
fi
(( LAUNCHER_ONLY )) && { echo "launcher: $DIST/trials_launcher"; exit 0; }

# Finds a game's package by what it says it is, not by its name.
find_package() {
  [[ -n $PACKAGES ]] || return 1
  local wanted="$1" file
  while IFS= read -r file; do
    [[ $(out/launcher/trials_extract "$file" --title-id 2>/dev/null) == "$wanted" ]] && { printf '%s\n' "$file"; return 0; }
  done < <(find "$PACKAGES" -type f -size +100M 2>/dev/null)
  return 1
}

BUILT=()
for game in "${GAMES[@]}"; do
  id="$(title_id "$game")"
  if [[ ! -f games/$game/assets/default.xex ]]; then
    if ! package="$(find_package "$id")"; then
      echo "skipping $game: no package with title ID $id${PACKAGES:+ in $PACKAGES}"
      continue
    fi
    echo "== $game: extracting $package"
    out/launcher/trials_extract "$package" "games/$game/assets"
  fi
  if [[ ! -f games/$game/generated/rexglue.cmake ]]; then
    echo "== $game: recompiling the game's code (about half an hour)"
    in_container "
      cd games/$game
      LD_LIBRARY_PATH=/work/third_party/rexglue-sdk/out/linux-amd64 \
        /work/third_party/rexglue-sdk/out/linux-amd64/rexglue --force codegen ${game}_manifest.toml"
  fi
  if [[ $TARGET == android ]]; then
    tools/android/build.sh "$game"
  else
    echo "== $game: compiling"
    in_container "
      cmake -S games/$game -B out/$game -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 \
        -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_PERF_COUNTERS=OFF >/dev/null
      cmake --build out/$game --target $game"
    cp "out/$game/$game" out/"$game"/librexruntime*.so out/"$game"/librexgpu-xenos*.so "$DIST/"
  fi
  BUILT+=("$game")
done

(( ${#BUILT[@]} )) || { echo "no game built: pass --packages with your packages" >&2; exit 1; }
if [[ $TARGET == linux ]]; then
  echo "Built ${BUILT[*]}: run $DIST/trials_launcher"
else
  echo "Built ${BUILT[*]}: out/android/<game>/package/"
fi
