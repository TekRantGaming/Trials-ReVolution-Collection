#!/usr/bin/env bash
# Prepares third_party/rexglue-sdk for Linux and Android builds: ReXGlue SDK v0.10.0, SDL 3.4.14 and the patches in
# sdk-patches/linux-android. Windows builds keep using the SDK BUILDING.md describes. Nothing to do when it is ready.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
SDK_COMMIT=c94f5ebdcb3c9d1a460ca48e04f9758448f8d518  # Release v0.10.0
SDL_TAG=release-3.4.14
SDK=third_party/rexglue-sdk
STAMP="$SDK/.trials-patches"
PATCHES=(sdk-patches/linux-android/*.patch)
WANTED="$(cat "${PATCHES[@]}" | sha256sum | cut -d' ' -f1)"

[[ -f $STAMP && $(cat "$STAMP") == "$WANTED" ]] && { echo "SDK ready: $SDK"; exit 0; }

if [[ ! -d $SDK/.git ]]; then
  git clone --filter=blob:none --no-checkout https://github.com/rexglue/rexglue-sdk.git "$SDK"
fi
git -C "$SDK" checkout -q --force "$SDK_COMMIT"
git -C "$SDK" clean -fdq -e out
git -C "$SDK" submodule update --init --recursive --depth 1
git -C "$SDK/thirdparty/sdl3" fetch -q --depth 1 origin tag "$SDL_TAG"
git -C "$SDK/thirdparty/sdl3" checkout -q "$SDL_TAG"
for patch in "${PATCHES[@]}"; do
  git -C "$SDK" apply --whitespace=nowarn --binary "$PWD/$patch"
done
echo "$WANTED" > "$STAMP"
echo "SDK ready: $SDK"
