#!/usr/bin/env python3
# The host's Mach-O assembly (entry.S, nvn_entry.S, nvn_thunks.S) as ELF assembly for Android:
#   · C symbols lose their leading underscore (_dm_gate → dm_gate); local labels (Ln12, 1:) are untouched
#   · adrp x, _s@PAGE / add x, x, _s@PAGEOFF → adrp x, s / add x, x, :lo12:s
#   · mach_absolute_time (an inline function in the Android shim) is called as dm_mach_absolute_time (android/compat.c)
#   · every .globl is also .hidden: in the app's shared library (libdm.so) a preemptible symbol cannot be reached with adrp / add, and nothing outside the library uses them
#   · nvn_thunks.S keeps its table of name pointers (_nvn_names: .quad Ln…) in .text on Apple; ELF puts it in .data.rel.ro and the strings in .rodata,
#     since absolute addresses in .text would need text relocations, which Android's linker refuses
# Usage: elfasm.py <in.S> <out.S>
import re, sys

src, dst = sys.argv[1], sys.argv[2]
out = []
for line in open(src).read().splitlines():
    code, sep, comment = line.partition('//')
    code = code.replace('_mach_absolute_time', '_dm_mach_absolute_time')
    code = re.sub(r'\b_([A-Za-z][A-Za-z0-9_]*)@PAGEOFF\b', r':lo12:\1', code)
    code = re.sub(r'\b_([A-Za-z][A-Za-z0-9_]*)@PAGE\b', r'\1', code)
    code = re.sub(r'(?<![A-Za-z0-9_.])_([A-Za-z][A-Za-z0-9_]*)', r'\1', code)
    if re.match(r'\s*\.globl\s+nvn_names\b', code):
        out.append('    .section .data.rel.ro, "aw"')
        out.append('    .p2align 3')
    if re.match(r'\s*\.cstring\b', code):   # Mach-O's C string section
        out.append('    .section .rodata.str1.1, "aMS", @progbits, 1')
        continue
    out.append(code + (sep + comment if sep else ''))
    g = re.match(r'(\s*)\.globl\s+([A-Za-z_][A-Za-z0-9_]*)', code)
    if g: out.append(f'{g.group(1)}.hidden {g.group(2)}')
open(dst, 'w').write('\n'.join(out) + '\n')
