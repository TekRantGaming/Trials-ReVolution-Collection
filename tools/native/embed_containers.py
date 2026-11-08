#!/usr/bin/python3 -I
"""Embeds the vertex shader containers make_containers.py wrote: the native renderer reads the
vertex elements from them. Usage: embed_containers.py <containers folder> <output.cpp>"""
import os
import sys

folder, output = sys.argv[1:3]
names = sorted(n for n in os.listdir(folder) if n.endswith('.v.bin'))
lines = ['#include <cstddef>', '#include <cstdint>',
         'struct NativeContainer { uint64_t hash; const uint8_t* data; uint32_t size; };']
table = []
for i, name in enumerate(names):
    data = open(os.path.join(folder, name), 'rb').read()
    lines.append(f'static const uint8_t c{i}[] = {{' + ','.join(map(str, data)) + '};')
    table.append(f'{{0x{name[:16]}ull, c{i}, {len(data)}}},')
lines += ['extern const NativeContainer g_nativeContainers[] = {'] + table + ['};']
lines.append(f'extern const size_t g_nativeContainerCount = {len(names)};')
open(output, 'w').write('\n'.join(lines) + '\n')
print(len(names), 'containers')
