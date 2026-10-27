"""Where the game's main thread spends its time, from the plugin's stack sampler ([Trace] Sampler=1).

usage: python tools/sample_report.py <run dir> [--from S] [--to S] [--top 30] [--tree NAME] [--exe PATH]
                                     [--threads main|physx]

Reads <run>/samples.bin and modules.txt (and meta.json and trace.bin, for times and frames). Names come from the
decrypted exe (function ranges, the Lua API registrations, tools/gen_addresses.py) and the plugin's map file in the
run folder; other modules are named by file. Times are milliseconds per frame (frames from the trace's frame_update
events in the window). "Lua API" charges each sample to the outermost Lua API function on its stack.
--tree NAME prints who calls NAME and what NAME calls (inclusive).
--threads physx: PhysX's threads instead of the main thread ([Trace] SamplerPhysX=1). Their samples are taken only
while a thread runs and weigh the CPU time it used, so the times are CPU milliseconds per frame, summed over threads
(work a thread did just before it blocked is charged to the wait it is found in). --busy keeps only the samples of
threads caught working, one interval each: how the work divides between functions. PhysXGpu_64.dll is named from
the map of the GPU build (third_party/physx-build).
"""
import argparse, bisect, collections, json, os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..'))
LUA_REGISTER = 0x4a9050  # 0x1404a9050(kind, "Name", impl, ...)


def exe_symbols(path):
    """Function ranges (pdata) and names for the exe: Lua API implementations, gen_addresses entries."""
    import pefile, capstone
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']])
    img = pe.get_memory_mapped_image()
    base = pe.OPTIONAL_HEADER.ImageBase
    raw = sorted({(e.struct.BeginAddress, e.struct.EndAddress, e.struct.UnwindData) for e in pe.DIRECTORY_ENTRY_EXCEPTION})
    # A fragment (cold code, a split epilogue) has chained unwind info pointing at its primary function: charge it there.
    def primary(begin, unwind, depth=0):
        flags = img[unwind] >> 3
        if not (flags & 4) or depth > 8:  # UNW_FLAG_CHAININFO
            return begin
        codes = img[unwind + 2]
        at = unwind + 4 + 2 * (codes + (codes & 1))
        b, e, u = struct.unpack_from('<III', img, at)
        return primary(b, u, depth + 1)
    funcs = [(b, e, primary(b, u)) for b, e, u in raw]
    names = {}
    sys.path.insert(0, HERE)
    try:
        import gen_addresses
        for name, rva, kind, _ in gen_addresses.ENTRIES:
            if kind == 'code':
                names[rva] = name
    except Exception:
        pass
    text = [s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text'][0]
    tlo = text.VirtualAddress
    data = img[tlo:tlo + text.Misc_VirtualSize]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    for m in re.finditer(rb'\xe8', data):
        i = m.start()
        if i + 5 > len(data) or tlo + i + 5 + struct.unpack_from('<i', data, i + 1)[0] != LUA_REGISTER:
            continue
        call = tlo + i
        name = impl = None
        for back in range(0x40, 0, -1):
            ins = list(md.disasm(img[call - back:call], base + call - back))
            if not ins or ins[-1].address + ins[-1].size != base + call:
                continue
            for x in ins:
                if x.mnemonic == 'lea' and 'rip' in x.op_str:
                    reg = x.op_str.split(',')[0].strip()
                    t = x.address - base + x.size + int(x.op_str.split('rip')[1].strip(' ]').replace(' ', ''), 16)
                    if reg == 'rdx':
                        name = t
                    elif reg == 'r8':
                        impl = t
            break
        if name is not None and impl is not None:
            s = img[name:name + 64].split(b'\0')[0].decode('latin1')
            names[impl] = 'lua:' + s
    return funcs, names


def map_symbols(path):
    """(rva, name) sorted, from an MSVC /MAP file (Rva+Base column minus the preferred load address)."""
    out = []
    pref = 0x180000000
    for line in open(path, encoding='latin1'):
        m = re.match(r'\s*Preferred load address is ([0-9a-fA-F]+)', line)
        if m:
            pref = int(m.group(1), 16)
            continue
        m = re.match(r'\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s', line)
        if m and int(m.group(2), 16) >= pref:
            out.append((int(m.group(2), 16) - pref, demangle(m.group(1))))
    out.sort()
    return out


def demangle(s):
    m = re.match(r'\?([^@?]+)@([^@]*)@', s)
    if m and m.group(2) and not m.group(2).startswith('?'):
        return f'{m.group(2)}::{m.group(1)}'
    m = re.match(r'\?([^@?]+)@', s)
    return m.group(1) if m else s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--from', dest='t0', type=float, default=None)
    ap.add_argument('--to', dest='t1', type=float, default=None)
    ap.add_argument('--top', type=int, default=30)
    ap.add_argument('--tree', default=None)
    ap.add_argument('--exe', default=os.path.join(REPO, 'research', 're', 'bin', 'teardown_unpacked.exe'))
    ap.add_argument('--threads', choices=['main', 'physx'], default='main')
    ap.add_argument('--busy', action='store_true', help='with --threads physx: only samples caught working')
    a = ap.parse_args()

    raw = open(os.path.join(a.run, 'samples.bin'), 'rb').read()
    magic, ver, hz, qfreq, exe_base, tid, depth = struct.unpack_from('<8sIIQQII', raw, 0)
    if magic != b'TDPXSMP1':
        sys.exit('not a sampler file')
    meta = json.load(open(os.path.join(a.run, 'meta.json')))
    q0 = meta['qpc_start']
    lo = q0 + int((a.t0 or 0) * qfreq)
    hi = q0 + int(a.t1 * qfreq) if a.t1 is not None else 1 << 63

    mods = []
    for line in open(os.path.join(a.run, 'modules.txt')):
        b, size, name = line.split(None, 2)
        mods.append((int(b, 16), int(b, 16) + int(size, 16), name.strip()))
    mods.sort()
    mod_starts = [m[0] for m in mods]

    funcs, exe_names = exe_symbols(a.exe)
    f_starts = [f[0] for f in funcs]
    asi = None
    for b, e, name in mods:
        if name.lower() == 'teardownphysx.asi':
            asi = (b, e)
    asi_syms = []
    mp = os.path.join(a.run, 'TeardownPhysX.map')
    if asi and os.path.exists(mp):
        asi_syms = map_symbols(mp)
    asi_starts = [s[0] for s in asi_syms]
    # PhysXGpu_64.dll: the linker map of the GPU build it was copied from (tools\build_physx_gpu.cmd)
    gpu = next(((b, e) for b, e, name in mods if name.lower() == 'physxgpu_64.dll'), None)
    gmap = os.path.join(REPO, 'third_party', 'physx-build', 'release-gpu', 'lib', 'bin', 'win.x86_64.vc143.mt',
                        'release', 'PhysXGpu.map')
    gpu_syms = map_symbols(gmap) if gpu and os.path.exists(gmap) else []
    gpu_starts = [s[0] for s in gpu_syms]
    cache = {}

    def sym(addr):
        r = cache.get(addr)
        if r is not None:
            return r
        i = bisect.bisect_right(mod_starts, addr) - 1
        if i < 0 or addr >= mods[i][1]:
            r = f'?{addr:x}'
        elif mods[i][0] == exe_base:
            rva = addr - exe_base
            j = bisect.bisect_right(f_starts, rva) - 1
            if j >= 0 and rva < funcs[j][1]:
                st = funcs[j][2]
                r = exe_names.get(st) or f'td+{st:x}'
            else:
                r = f'td+{rva:x}?'
        elif asi and mods[i][0] == asi[0] and asi_syms:
            rva = addr - asi[0]
            j = bisect.bisect_right(asi_starts, rva) - 1
            r = 'px:' + (asi_syms[j][1] if j >= 0 else f'+{rva:x}')
        elif gpu and mods[i][0] == gpu[0] and gpu_syms:
            rva = addr - gpu[0]
            j = bisect.bisect_right(gpu_starts, rva) - 1
            r = 'gpu:' + (gpu_syms[j][1] if j >= 0 else f'+{rva:x}')
        else:
            r = mods[i][2]
        cache[addr] = r
        return r

    samples = []
    weights = []  # CPU microseconds per sample (version 2)
    threads_seen = set()
    first = last = None
    off = struct.calcsize('<8sIIQQII')
    head = 24 if ver >= 2 else 16
    while off + head <= len(raw):
        if ver >= 2:
            t, meta_n, us = struct.unpack_from('<QQQ', raw, off)
            n, stid = meta_n & 0xffffffff, meta_n >> 32
        else:
            t, n = struct.unpack_from('<QQ', raw, off)
            stid, us = tid, 0
        if off + head + 8 * n > len(raw):
            break
        mine = (stid == tid) if a.threads == 'main' else (stid != tid)
        if lo <= t < hi and mine:
            samples.append([sym(x) for x in struct.unpack_from(f'<{n}Q', raw, off + head)])
            weights.append(us)
            threads_seen.add(stid)
        if lo <= t < hi and stid == tid:  # the window's span comes from the main thread's samples
            first = t if first is None else first
            last = t
        off += head + 8 * n
    if not samples:
        sys.exit('no samples in the window')
    if a.threads == 'physx' and a.busy:
        # only threads caught working (not in the dispatcher's wait or another blocking call), one interval each:
        # the share of each function in the work, without the weighting's charge to waits
        keep = [i for i, st in enumerate(samples)
                if not any(s == 'px:PxSyncImpl::wait' for s in st[:8])
                and not (st[0] == 'ntdll.dll' and len(st) > 1 and st[1] == 'KERNELBASE.dll')]
        samples = [samples[i] for i in keep]
        weights = [1e6 / hz] * len(samples)
        if not samples:
            sys.exit('no running samples in the window')

    frames = None
    tb = os.path.join(a.run, 'trace.bin')
    if os.path.exists(tb):
        names = {int(k): v for k, v in meta['events'].items()}
        fid = next((k for k, v in names.items() if v == 'frame_update'), None)
        tr = open(tb, 'rb').read()
        cnt = 0
        for r0 in range(32, len(tr) - 31, 32):
            t0, t1, eid, ttid, arg = struct.unpack_from('<QQIIQ', tr, r0)
            if eid == fid and lo <= t0 < hi:
                cnt += 1
        frames = cnt or None
    if a.threads == 'main':
        n_main = len(samples)
        ms_per_sample = 1000.0 * (last - first) / qfreq / max(1, n_main - 1) if n_main > 1 else 1000.0 / hz
        per = (lambda c: c * ms_per_sample / frames) if frames else (lambda c: 100.0 * c / len(samples))
        unit = 'ms/frame' if frames else '% of samples'
        print(f'{len(samples)} samples, one per {ms_per_sample:.2f} ms (asked {hz} Hz), '
              f'{len(samples) * ms_per_sample / 1000:.1f} s of main thread, '
              f'{frames or "?"} frames' + (f', window {a.t0 or 0:.0f}-{a.t1:.0f} s' if a.t1 else ''))
    else:
        total_us = sum(weights)
        span_s = (last - first) / qfreq if first is not None and last is not None and last > first else 0.0
        per = (lambda c: c / 1000.0 / frames) if frames else (lambda c: 100.0 * c / max(1, total_us))
        unit = 'CPU ms/frame' if frames else '% of CPU time'
        print(f'{len(samples)} samples of {len(threads_seen)} PhysX threads, {total_us / 1e6:.1f} s of CPU'
              + (f' in {span_s:.1f} s ({total_us / 1e4 / span_s:.0f}% of one core)' if span_s else '')
              + f', {frames or "?"} frames' + (f', window {a.t0 or 0:.0f}-{a.t1:.0f} s' if a.t1 else ''))

    inc = collections.Counter()
    slf = collections.Counter()
    lua = collections.Counter()
    for st, us in zip(samples, weights):
        w = 1 if a.threads == 'main' else us
        slf[st[0]] += w
        for s in set(st):
            inc[s] += w
        outer = next((s for s in reversed(st) if s.startswith('lua:')), None)
        if outer:
            lua[outer] += w

    whole = len(samples) if a.threads == 'main' else max(1, sum(weights))

    def table(title, c, n):
        print(f'\n{title} ({unit}):')
        for k, v in c.most_common(n):
            print(f'  {per(v):8.2f}  {100.0 * v / whole:5.1f}%  {k}')

    table('inclusive', inc, a.top)
    table('self', slf, a.top)
    table('Lua API calls (outermost on the stack)', lua, a.top)
    if a.tree:
        callers, callees = collections.Counter(), collections.Counter()
        for st, us in zip(samples, weights):
            w = 1 if a.threads == 'main' else us
            idx = [i for i, s in enumerate(st) if s == a.tree]
            if not idx:
                continue
            i = max(idx)  # outermost occurrence
            callers[st[i + 1] if i + 1 < len(st) else '(root)'] += w
            callees[st[i - 1] if i > 0 else '(self)'] += w
        table(f'callers of {a.tree}', callers, a.top)
        table(f'{a.tree} calls', callees, a.top)


if __name__ == '__main__':
    main()
