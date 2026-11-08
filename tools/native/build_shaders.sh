#!/usr/bin/env bash
# Builds out/native (shader cache and vertex containers) from a shader dump:
#   tools/native/build_shaders.sh <folder gpu_shader_dump_dir wrote>
# Needs XenosRecomp in out/XenosRecomp with tools/native/xenosrecomp.patch applied, built in out/xrb.
set -euo pipefail
cd "$(dirname "$0")/../.."
DUMP="$1"
XR="$PWD/out/XenosRecomp"
WORK=out/native/containers
rm -rf "$WORK"; mkdir -p "$WORK"
python3 -I tools/native/make_containers.py "$DUMP" "$WORK"
XENOS_RECOMP_MICROCODE_KEY=1 XENOS_RECOMP_NO_ZSTD=1 LD_LIBRARY_PATH="$XR/thirdparty/dxc-bin/lib/x64" \
  out/xrb/XenosRecomp/XenosRecomp "$WORK" out/native/shader_cache.cpp "$XR/XenosRecomp/shader_common.h"
python3 -I tools/native/embed_containers.py "$WORK" out/native/shader_containers.cpp
