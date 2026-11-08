#!/usr/bin/python3 -I
"""Wraps raw Xenos microcode in the D3DX shader containers XenosRecomp reads.

Trials Fusion has no containers: its engine loads bare microcode. Takes the folder
gpu_shader_dump_dir wrote (<hash>.v, <hash>.p, meta.txt) and writes one container for each
shader. What a container would say is worked out from the microcode and the dump:
  constants   none declared: XenosRecomp reads undeclared float constants from the buffer
  samplers    s<fetch constant>, from the shader's texture bindings
  bools       declared by index, for conditional jumps
  vfetches    one element per fetch instruction, found by walking the control flow
  interpolators  TEXCOORDn for output n, as many as the vertex shader exports
"""
import os
import re
import struct
import sys

# Usages with no special case in XenosRecomp, in the order vfetches take them.
TEXCOORD, COLOR, BLENDWEIGHT, POSITION = 5, 10, 1, 0
ELEMENT_USAGES = [(POSITION, 0)] + [(TEXCOORD, i) for i in range(4)] + [(COLOR, 0), (BLENDWEIGHT, 0), (COLOR, 1)] + [
    (TEXCOORD, i) for i in range(4, 8)]
EXEC_OPCODES = {1, 2, 3, 4, 5, 6, 13, 14}


def vfetch_addresses(words):
    """Instruction indices of the vertex fetches, walking exec clauses."""
    found = []
    end = len(words) // 3
    pair = 0
    while pair * 3 < end * 3 and pair < end:
        w0, w1, w2 = words[pair * 3:pair * 3 + 3]
        for lo, hi in ((w0, w1 & 0xFFFF), ((w1 >> 16) | ((w2 & 0xFFFF) << 16), w2 >> 16)):
            opcode = hi >> 12
            if opcode in EXEC_OPCODES:
                address, count, sequence = lo & 0xFFF, (lo >> 12) & 7, (lo >> 16) & 0xFFF
                if address:
                    end = min(end, address)
                for i in range(count):
                    at = (address + i) * 3
                    if (sequence >> (2 * i)) & 1 and at + 2 < len(words) and words[at] & 0x1F == 0:
                        found.append(address + i)
        pair += 1
    return found


def container(is_vertex, code, samplers, elements, interpolators, param_gen, outputs):
    names = b''
    infos = []
    constants = [(f's{s}'.encode(), 3, s) for s in samplers] + [(f'xb{i}'.encode(), 0, i) for i in range(16)]
    table_header = 28
    info_size = 20 * len(constants)
    for name, register_set, index in constants:
        offset = table_header + info_size + len(names)
        names += name + b'\0'
        infos.append(struct.pack('>IHHHHII', offset, register_set, index, 1, 0, 0, 0))
    table = struct.pack('>7I', table_header + info_size + len(names), 0, 0, len(constants), table_header, 0, 0)
    table += b''.join(infos) + names
    table = struct.pack('>I', len(table)) + table
    table += b'\0' * (-len(table) & 3)

    interp = [i | (TEXCOORD << 4) | (i << 8) for i in range(interpolators)]
    head = struct.pack('>6I', 0, len(code), 0, (param_gen & 0xFF) << 8, 0, interpolators << 5)
    if is_vertex:
        shader = head + struct.pack('>3I', 0, len(elements), 0)
        shader += b''.join(struct.pack('>I', address | (usage << 12) | (index << 16)) for address, usage, index in elements)
    else:
        shader = head + struct.pack('>2I', 0, outputs)
    shader += b''.join(struct.pack('>I', v) for v in interp)

    table_offset = 36
    shader_offset = table_offset + len(table)
    virtual_size = shader_offset + len(shader)
    virtual_size += -virtual_size & 15
    header = struct.pack('>9I', 0x102A1100 | int(is_vertex), virtual_size, len(code), 0, table_offset, 0, shader_offset, 0, 0)
    blob = header + table + shader
    return blob + b'\0' * (virtual_size - len(blob)) + code


def main():
    source, destination = sys.argv[1:3]
    os.makedirs(destination, exist_ok=True)
    meta = [line.split() for line in open(os.path.join(source, 'meta.txt'))]
    exports = 1
    for fields in meta:
        program = int(next(f for f in fields if f.startswith('program_cntl='))[13:], 16)
        exports = max(exports, ((program >> 20) & 0xF) + 1)
    written = skipped = 0
    for fields in meta:
        kind, name = fields[0], fields[1]
        info = dict(f.split('=', 1) for f in fields if '=' in f)
        raw = open(os.path.join(source, f'{name}.{kind}'), 'rb').read()
        words = list(struct.unpack(f'<{len(raw) // 4}I', raw))
        code = struct.pack(f'>{len(words)}I', *words)
        samplers = sorted({int(m.group(1)) for f in fields for m in [re.match(r'tex(\d+):', f)] if m})
        program = int(info['program_cntl'], 16)
        misc = int(info['context_misc'], 16)
        if kind == 'v':
            fetches = vfetch_addresses(words)
            if len(fetches) > len(ELEMENT_USAGES):
                print('too many vertex fetches', name, len(fetches))
                skipped += 1
                continue
            elements = [(a, *ELEMENT_USAGES[i]) for i, a in enumerate(fetches)]
            blob = container(True, code, samplers, elements, exports, 0xFF, 0)
        else:
            param_gen = (misc >> 8) & 0xFF if (program >> 18) & 1 else 0xFF
            colors = int(info['colors'], 16)
            outputs = (colors & 0xF) | (0x10 if info['depth'] == '1' else 0)
            count = min(exports, param_gen) if param_gen != 0xFF else exports
            blob = container(False, code, samplers, [], count, param_gen, outputs)
        open(os.path.join(destination, f'{name}.{kind}.bin'), 'wb').write(blob)
        written += 1
    print(f'{written} containers, {skipped} skipped, {exports} interpolators')


main()
