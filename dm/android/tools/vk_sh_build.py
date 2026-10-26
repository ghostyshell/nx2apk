#!/usr/bin/env python3
# DMC's NVN shaders for the Android Vulkan replay (nvn_vk.c): the package shader_split.py made from main → SPIR-V the Vulkan driver takes.
#   Maxwell code → SPIR-V (dmsh_xlate, the Ryujinx translator, as for the Metal route) → Vulkan GLSL (spirv-cross) → the fixups below → SPIR-V (the NDK's glslc)
# Fixups (the GLSL counterparts of dmsh_build.py's MSL ones):
#   · every vertex output starts as (0, 0, 0, 1), what an unwritten attribute reads on the hardware (vs_init)
#   · vertex shaders declare outputs 0-7, the unwritten ones (0, 0, 0, 1): DMC's shaders are single-stage BNSH with no pair table, and a Vulkan pipeline needs every pixel shader
#     input written by the vertex shader (Metal's vs_pad, for every possible partner)
#   · a vertex input the shader reads only through floatBitsToInt / floatBitsToUint (the translator, with no type answer, takes every attribute as float and reinterprets the
#     bits, as Maxwell hands an integer attribute's raw value) is declared uvec4 and cast back to the float the body expects: DMC's skinned characters read their bone indices
#     and weights from RGBA8UI attributes this way, and a float input over an integer format read garbage (bodies missing, only heads drawn). The reinterpretations are
#     replaced by the integer itself. Listed as <location>u in the table
#   · pixel shaders use descriptor set 1, vertex shaders set 0: the translator numbers each shader's bindings from 0 in set 0
#   · vertex shaders get a second entry, <key>_z: <key> takes NVN's default GL depth convention (z' = z/2 + w/2), <key>_z leaves z as written (near = 0) (vs_depth)
# Output in <out>: <key>.spv (and <key>_z.spv), and vk_shaders.tsv, one row per shader:
#   key <TAB> stage (vs / ps) <TAB> c1 hex <TAB> resources, space-separated kind:reg=binding <TAB> vertex input locations (comma-separated, - for none)
#     u<reg>  uniform buffer slot reg (constant buffer 3 + reg), as u<reg>=<binding>:<bytes the shader reads, 0 = all>
#     c1  the shader's own constants      d<bank>  driver constant bank (0, 2)
#     t<slot> texture slot (0-127 combined, 128+ separate; sampler from the same handle)   T<slot>.<samp>: texture slot with a separate sampler slot
#     b<slot> storage buffer slot (address in c0)                     i<slot> image slot                  sup  the translator's support buffer
# Usage: vk_sh_build.py <package dir> <out dir>      Environment: XLATE (dmsh_xlate), DOTNET_ROOT, GLSLC, SPIRV_CROSS
import json, os, re, struct, subprocess, sys, concurrent.futures as cf

pkg, out = sys.argv[1:3]
os.makedirs(out, exist_ok=True); work = os.path.join(out, 'work'); os.makedirs(work, exist_ok=True)
HERE = os.path.dirname(os.path.abspath(__file__))
XLATE = os.environ.get('XLATE', os.path.join(HERE, '../../gfx/tools/dmsh_xlate_bin/dmsh_xlate'))
GLSLC = os.environ.get('GLSLC', '/opt/homebrew/share/android-commandlinetools/ndk/28.2.13676358/shader-tools/darwin-x86_64/glslc')
SPVC = os.environ.get('SPIRV_CROSS', '/opt/homebrew/bin/spirv-cross')
TEX_BASE, IMG_BASE, IMG_SLOTS, SEPTEX_BASE, SEPTEX_SLOT0, SAMP_BASE = 0x20, 0x120, 8, 0x168, 128, 0x568
SSBO = {'vs': 0x110, 'ps': 0x510}

def records(d):
    r = [m.start() for m in re.finditer(b'BTCDBTCD', d)]
    return [d[x:(r[k + 1] if k + 1 < len(r) else len(d))] for k, x in enumerate(r)]

keys = {}   # (package, index) -> key
for ln in open(os.path.join(pkg, 'shader_map.tsv')):
    c = ln.rstrip('\n').split('\t')
    if len(c) > 3 and c[0].endswith('.bin'): keys[(c[0], int(c[1]))] = c[3]

jobs, c1s = [], {}
for name in sorted(f for f in os.listdir(pkg) if f.endswith('.bin')):
    for i, rec in enumerate(records(open(os.path.join(pkg, name), 'rb').read())):
        key = keys[(name, i)]; stage = name[:2]
        ci = rec.find(bytes.fromhex('34127698')); code = rec[0x38:ci]
        c1o, c1t = struct.unpack_from('<2I', rec, ci + 0x700)
        c1s[key] = rec[0x38 + c1o:0x38 + c1t]
        cp = os.path.join(work, key + '.bin'); open(cp, 'wb').write(code[0x30:])
        jobs.append({'key': key, 'stage': stage, 'code': cp, 'tex': {}, 'img': {}, 'attr': {}, 'fout': {}, 'c1': c1s[key].hex(), 'debug': False, 'reduced': True})
jp = os.path.join(work, 'jobs.json'); json.dump(jobs, open(jp, 'w'))
r = subprocess.run([XLATE, jp, work], capture_output=True, text=True)
sys.stdout.write(r.stdout); sys.stderr.write(r.stderr)

def tex_slot(handle, image):
    """texture / image slot of a c2 handle (as dmsh_build.py decode_tex): → (slot, separate sampler slot or None)"""
    o0, o1, ty = handle & 0x3fff, (handle >> 14) & 0x3fff, handle >> 28
    def slot(w, base): return (w * 4 - base) // 8 if w * 4 >= base and (w * 4 - base) % 8 == 0 else None
    words = [o0] + ([o1] if ty else [])
    if image:
        return next((slot(x, IMG_BASE) for x in words if IMG_BASE <= x * 4 < IMG_BASE + 8 * IMG_SLOTS), None), None
    tw = [o0] if ty == 3 else words
    t = next((slot(x, TEX_BASE) for x in tw if x * 4 < SEPTEX_BASE and slot(x, TEX_BASE) is not None), None)
    if t is None: t = next((SEPTEX_SLOT0 + slot(x, SEPTEX_BASE) for x in tw if SEPTEX_BASE <= x * 4 < SAMP_BASE and slot(x, SEPTEX_BASE) is not None), None)
    s = next((slot(x, SAMP_BASE) for x in words if x * 4 >= SAMP_BASE and slot(x, SAMP_BASE) is not None), None) if ty not in (0, 3) else None
    return t, s

def ubo_bytes(src, binding):
    """bytes of the uniform block at binding the shader can read: up to its highest constant vec4 index, 0 (all) when it indexes the block with a computed index (bone matrices)"""
    m = re.search(r'binding = %d(?:, std140)?\) uniform \w+\s*\{[^}]*\}\s*(\w+);' % binding, src)
    if not m: return 0
    idx = re.findall(r'\b%s\._m0\[([^\]]+)\]' % m.group(1), src)
    if not idx or not all(i.isdigit() for i in idx): return 0
    return (max(int(i) for i in idx) + 1) * 16

def vs_init(src):
    outs = re.findall(r'^layout\(location = (\d+)\) out vec4 (\w+);', src, re.M)
    have = {int(l) for l, _ in outs}
    pad = [l for l in range(8) if l not in have]
    k = src.find('\nvoid main()')
    src = src[:k] + ''.join(f'\nlayout(location = {l}) out vec4 dm_pad{l};' for l in pad) + src[k:]
    m = re.search(r'void main\(\)\s*\{\n', src)
    return src[:m.end()] + ''.join(f'    {n} = vec4(0.0, 0.0, 0.0, 1.0);\n' for n in [n for _, n in outs] + ['dm_pad%d' % l for l in pad]) + src[m.end():]

def depth_gl(src):
    """z' = z/2 + w/2 before every return and at the end of main"""
    fix = 'gl_Position.z = fma(gl_Position.z, 0.5, gl_Position.w * 0.5);'
    m = re.search(r'void main\(\)\s*\{\n', src); body = src[m.end():]
    body = body.replace('return;', '{ ' + fix + ' return; }')
    k = body.rstrip().rfind('}')
    return src[:m.end()] + body[:k] + '    ' + fix + '\n' + body[k:]

def glslc(src, stage, dst):
    gp = dst[:-4] + '.glsl'; open(gp, 'w').write(src)
    r = subprocess.run([GLSLC, '-fshader-stage=' + ('vert' if stage == 'vs' else 'frag'), '--target-env=vulkan1.1', '-O', gp, '-o', dst], capture_output=True, text=True)
    return r.returncode == 0, r.stderr

def finish(j):
    key, st = j['key'], j['stage']
    spv = os.path.join(work, key + '.spv'); rp = os.path.join(work, key + '.res.json')
    if not os.path.exists(spv): return key, None, 'not translated'
    res = json.load(open(rp))
    r = subprocess.run([SPVC, spv, '--version', '450', '--vulkan-semantics'], capture_output=True, text=True)
    if r.returncode: return key, None, 'spirv-cross: ' + r.stderr.strip()[:200]
    src = r.stdout
    binds = []
    for b in res['cbufs']:
        s = b['slot']; binds.append(('u%d' % (s - 3) if s >= 3 else 'c1' if s == 1 else 'd%d' % s, b['binding']))
        if s >= 3: binds[-1] = (binds[-1][0], '%d:%d' % (b['binding'], ubo_bytes(src, b['binding'])))
    for b in res['sbufs']:
        off = b['cb_offset'] * 4; base = SSBO[st]
        if b['cb_slot'] != 0 or not (0 <= off - base < 0x100 and (off - base) % 16 == 0): return key, None, 'storage buffer address outside c0 tables: %r' % b
        binds.append(('b%d' % ((off - base) // 16), b['binding']))
    for kind, lst in (('t', res['textures']), ('i', res['images'])):
        for t in lst:
            if t['cbuf_slot'] != 2 and t['cbuf_slot'] >= 0: return key, None, 'bindless texture %r' % t
            slot, samp = tex_slot(t['handle'], kind == 'i')
            if slot is None: return key, None, 'texture handle %x has no slot' % t['handle']
            binds.append((('T%d.%d' % (slot, samp)) if samp is not None else '%s%d' % (kind, slot), t['binding']))
    m = re.search(r'layout\(set = 0, binding = (\d+)(?:, std140)?\) uniform support_buffer', src) or re.search(r'binding = (\d+)[^\n]*\) uniform\s+support_buffer', src)
    if m: binds.append(('sup', int(m.group(1))))
    ins = sorted(int(x) for x in re.findall(r'^layout\(location = (\d+)\) in ', src, re.M)) if st == 'vs' else []
    ints = set()
    if st == 'vs':
        for l, n in re.findall(r'^layout\(location = (\d+)\) in vec4 (\w+);', src, re.M):
            uses = re.findall(r'(floatBitsTo(?:Int|Uint)\()?\b%s\b(?!_u)' % n, src)
            body = uses[1:]   # the first is the declaration
            if body and all(u for u in body):
                ints.add(int(l))
                src = src.replace(f'layout(location = {l}) in vec4 {n};', f'layout(location = {l}) in uvec4 {n}_u;')
                # floatBitsToInt(n.yz) → ivec2(n_u.yz): the integer read directly (a uint → float → uint round trip can flush small values, as denormals, to zero)
                def cast(m, n=n):
                    sw = m.group(2); k = len(sw)
                    t = ('int' if m.group(1) == 'Int' else 'uint') if k == 1 else ('ivec%d' if m.group(1) == 'Int' else 'uvec%d') % k
                    return f'{t}({n}_u.{sw})'
                src = re.sub(r'floatBitsTo(Int|Uint)\(%s\.([xyzw]+)\)' % n, cast, src)
                if re.search(r'\b%s\b' % n, src): sys.exit(f'[RED] {key}: input {n} has a use the integer rewrite does not cover')
    if st == 'ps': src = src.replace('layout(set = 0,', 'layout(set = 1,').replace('layout(binding', 'layout(set = 1, binding')
    if st == 'vs':
        src = vs_init(src)
        ok, e = glslc(depth_gl(src), st, os.path.join(out, key + '.spv')); ok2, e2 = glslc(src, st, os.path.join(out, key + '_z.spv'))
        if not (ok and ok2): return key, None, 'glslc: ' + (e or e2)[:300]
    else:
        ok, e = glslc(src, st, os.path.join(out, key + '.spv'))
        if not ok: return key, None, 'glslc: ' + e[:300]
    return key, '%s\t%s\t%s\t%s\t%s' % (key, st, c1s[key].hex() or '-', ' '.join('%s=%s' % b for b in binds), ','.join(str(i) + ('u' if i in ints else '') for i in ins) or '-'), None

rows, bad = [], []
with cf.ThreadPoolExecutor(8) as ex:
    for key, row, err in ex.map(finish, jobs):
        (rows.append(row) if row else bad.append((key, err)))
open(os.path.join(out, 'vk_shaders.tsv'), 'w').write('# key\tstage\tc1\tresources\tinputs (dm/android/tools/vk_sh_build.py)\n' + '\n'.join(sorted(rows)) + '\n')
for k, e in bad: print('[RED]', k, e)
print('vk shaders: %d ok, %d failed' % (len(rows), len(bad)))
sys.exit(1 if bad else 0)
