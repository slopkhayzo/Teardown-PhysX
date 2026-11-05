"""Carry the plugin's engine addresses from one game build to the next.

usage: python -I tools/port_addresses.py --new research/re/bin/teardown_<next>_unpacked.exe [--json out.json]
                                          [--old research/re/bin/teardown_2_2_0_unpacked.exe]

Both inputs are decrypted copies (dump_unpacked.py or steamstub_decrypt.py). Every function chunk in .pdata is
disassembled and hashed with what moves between builds masked out: rel32 call/jump targets leaving the chunk and
RIP-relative displacements. Struct offsets, immediates and the code's shape stay in, so an "identical" chunk is the
same code at another address, with the same struct offsets and the same in-function sites.

Matching: unique hashes first; then the call and RIP-reference edges of identical chunks (the n-th call of an
identical chunk calls the n-th call's counterpart: catches changed functions called from unchanged ones); then
gaps between matched neighbours with the same number of chunks on both sides. Data addresses come from the
RIP references of identical chunks (votes). Sites inside functions (patch sites, return addresses) move with their
chunk; in a changed chunk they are aligned instruction by instruction and reported with their context.

The targets are gen_addresses.py's ENTRIES plus SITES below (the constants in src/fix and src/px); both hold the
current build's addresses (2.2.0). After a port: update ENTRIES and run gen_addresses.py, update the constants in
src/fix and src/px (patch-site byte checks contain rel32 fields: compare them by hand), update SITES, and check
the changed functions the plugin depends on with research/scripts/port_inspect.py (diff, paths: struct offsets).
Leaf functions without .pdata are found through what identical callers call; JobRun-style rewrites need a look.
"""
import argparse, bisect, collections, difflib, hashlib, json, os, pickle, struct, sys

import capstone
from capstone import x86
import pefile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..'))
sys.path.insert(0, HERE)
from gen_addresses import ENTRIES  # noqa: E402

# Addresses inside functions that the C++ code holds as constants (2.2.0): (name, rva, where it is used)
SITES = [
    ('KeepDebris.kSite', 0x0f8d32, 'src/fix/keep_debris.cpp'),
    ('KeepDebris.kLimit', 0x0f8d4c, 'src/fix/keep_debris.cpp'),
    ('SplitJobs.kSite', 0x142e99, 'src/fix/split_jobs.cpp'),
    ('SplitJobs.kMinBodies', 0x142eaa, 'src/fix/split_jobs.cpp'),
    ('StaticBodyUpdate.kSplitFinish', 0x142ec8, 'src/fix/static_body_update.cpp'),
    ('StaticBodyUpdate.kSplitTwin', 0x143539, 'src/fix/static_body_update.cpp'),
    ('VisibleEarlyOut.kSite', 0x4eccd6, 'src/fix/visible_early_out.cpp'),
    ('VisibleEarlyOut.kPatch', 0x4eccdf, 'src/fix/visible_early_out.cpp'),
    ('VisibleEarlyOut.kNext', 0x4ecce4, 'src/fix/visible_early_out.cpp'),
    ('VisibleEarlyOut.kExit', 0x4eccf3, 'src/fix/visible_early_out.cpp'),
    ('ContactWake.ret0', 0x0b482e, 'src/px/shadow.cpp kContactWakeReturns'),
    ('ContactWake.ret1', 0x0b4845, 'src/px/shadow.cpp kContactWakeReturns'),
    ('ContactWake.ret2', 0x0bc6b3, 'src/px/shadow.cpp kContactWakeReturns'),
    ('ContactWake.ret3', 0x0bc6de, 'src/px/shadow.cpp kContactWakeReturns'),
    ('ContactWake.ret4', 0x0bd6e3, 'src/px/shadow.cpp kContactWakeReturns'),
    ('ContactWake.ret5', 0x0bd70d, 'src/px/shadow.cpp kContactWakeReturns'),
    ('PlayerGroundWakeReturn', 0x0c5d0c, 'src/px/shadow.cpp kPlayerGroundWakeReturn'),
]

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True


class Image:
    def __init__(self, path):
        self.path = path
        self.pe = pefile.PE(path, fast_load=True)
        self.img = self.pe.get_memory_mapped_image()
        self.secs = [(s.Name.rstrip(b'\0').decode(), s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize)
                     for s in self.pe.sections]
        pd = next(s for s in self.pe.sections if s.Name.startswith(b'.pdata'))
        raw = self.img[pd.VirtualAddress:pd.VirtualAddress + pd.Misc_VirtualSize]
        chunks = set()
        for i in range(0, len(raw) - 11, 12):
            b, e, _ = struct.unpack_from('<III', raw, i)
            if b and e > b:
                chunks.add((b, e))
        self.chunks = sorted(chunks)
        self.starts = [b for b, _ in self.chunks]

    def section(self, rva):
        for n, b, e in self.secs:
            if b <= rva < e:
                return n
        return None

    def chunk_of(self, rva):
        i = bisect.bisect_right(self.starts, rva) - 1
        if i >= 0 and self.chunks[i][0] <= rva < self.chunks[i][1]:
            return i
        return None


def disasm(img, b, e):
    """[(off, size, key, rip_target, branch_target)] with key = the instruction's bytes, moving parts zeroed."""
    code = img[b:e]
    out = []
    end = 0
    for i in md.disasm(code, b):
        raw = bytearray(i.bytes)
        rip = br = None
        if i.disp_size:
            for o in i.operands:
                if o.type == x86.X86_OP_MEM and o.mem.base == x86.X86_REG_RIP:
                    rip = i.address + i.size + o.mem.disp
                    raw[i.disp_offset:i.disp_offset + i.disp_size] = bytes(i.disp_size)
                    break
        if (i.group(capstone.CS_GRP_JUMP) or i.group(capstone.CS_GRP_CALL)) and i.imm_size and \
                i.operands and i.operands[0].type == x86.X86_OP_IMM:
            t = i.operands[0].imm
            if not (b <= t < e):
                br = t
                raw[i.imm_offset:i.imm_offset + i.imm_size] = bytes(i.imm_size)
        out.append((i.address - b, i.size, bytes(raw), rip, br))
        end = i.address - b + i.size
    if end < len(code):  # undecodable tail: keep as raw bytes
        out.append((end, len(code) - end, bytes(code[end:]), None, None))
    return out


def analyze(im, cache):
    if os.path.exists(cache):
        return pickle.load(open(cache, 'rb'))
    hashes, refs = [], []
    for n, (b, e) in enumerate(im.chunks):
        ins = disasm(im.img, b, e)
        h = hashlib.sha1(b''.join(k for _, _, k, _, _ in ins)).digest()
        hashes.append(h)
        refs.append([(j, r, t) for j, (_, _, _, r, t) in enumerate(ins) if r is not None or t is not None])
        if n % 5000 == 0:
            print(f'  {os.path.basename(im.path)}: {n}/{len(im.chunks)}', flush=True)
    data = (hashes, refs)
    pickle.dump(data, open(cache, 'wb'))
    return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--old', default=os.path.join(REPO, 'research', 're', 'bin', 'teardown_2_2_0_unpacked.exe'))
    ap.add_argument('--new', required=True)
    ap.add_argument('--json', default='')
    ap.add_argument('--cache', default=os.path.join(REPO, 'research', 're', 'port'))
    a = ap.parse_args()
    os.makedirs(a.cache, exist_ok=True)
    old, new = Image(a.old), Image(a.new)
    tag = lambda p: hashlib.sha256(open(p, 'rb').read()).hexdigest()[:12]
    ho, ro = analyze(old, os.path.join(a.cache, tag(a.old) + '.pkl'))
    hn, rn = analyze(new, os.path.join(a.cache, tag(a.new) + '.pkl'))
    print(f'chunks: old {len(ho)}, new {len(hn)}')

    # 1. unique identical chunks
    co, cn = collections.Counter(ho), collections.Counter(hn)
    idx_new = {h: i for i, h in enumerate(hn) if cn[h] == 1}
    match = {}   # old chunk index -> new chunk index
    how = {}     # old chunk index -> 'identical' | 'call edge' | 'gap'
    for i, h in enumerate(ho):
        if co[h] == 1 and h in idx_new:
            match[i] = idx_new[h]
            how[i] = 'identical'
    print(f'unique identical: {len(match)}')

    # 2. edges of identical chunks (same instruction index: same callee / same data), repeated until stable
    def edges(i, j):
        for (k1, r1, t1), (k2, r2, t2) in zip(ro[i], rn[j]):
            if k1 != k2:
                break
            for x, y in ((r1, r2), (t1, t2)):
                if x is not None and y is not None:
                    yield x, y

    done = set()
    changed = True
    while changed:
        changed = False
        for i, j in list(match.items()):
            if ho[i] != hn[j] or i in done:
                continue
            done.add(i)
            for x, y in edges(i, j):
                ci, cj = old.chunk_of(x), new.chunk_of(y)
                if ci is not None and cj is not None and ci not in match and \
                        old.chunks[ci][0] == x and new.chunks[cj][0] == y:
                    match[ci] = cj
                    how[ci] = 'identical' if ho[ci] == hn[cj] else 'call edge'
                    changed = True
    print(f'after edges: {len(match)} ({sum(1 for v in how.values() if v == "call edge")} changed, via calls)')

    # 3. gaps: same number of unmatched chunks between two matched neighbours with the same spacing
    ms = sorted(match.items())
    for (i0, j0), (i1, j1) in zip(ms, ms[1:]):
        if i1 - i0 > 1 and i1 - i0 == j1 - j0:
            for d in range(1, i1 - i0):
                if i0 + d not in match:
                    match[i0 + d] = j0 + d
                    how[i0 + d] = 'identical' if ho[i0 + d] == hn[j0 + d] else 'gap'
    ident = sum(1 for i, j in match.items() if ho[i] == hn[j])
    print(f'after gaps: {len(match)} of {len(ho)} old chunks matched, {ident} identical')

    # data addresses: what the same instruction of an identical chunk refers to, outside any code chunk
    data_votes = collections.defaultdict(collections.Counter)
    for i, j in match.items():
        if ho[i] == hn[j]:
            for x, y in edges(i, j):
                if old.chunk_of(x) is None and new.chunk_of(y) is None:
                    data_votes[x][y] += 1

    def data_target(rva, near_ok=True):
        if rva in data_votes:
            (y, c), *rest = data_votes[rva].most_common()
            note = f'{c} refs agree' + (f', {sum(n for _, n in rest)} disagree' if rest else '')
            return y, note
        if not near_ok:
            return None, 'no refs'
        # nearest referenced neighbour in the same section, same delta
        sec = old.section(rva)
        near = sorted((abs(x - rva), x) for x in data_votes if old.section(x) == sec and abs(x - rva) < 0x400)
        if near:
            x = near[0][1]
            y, c = data_votes[x].most_common(1)[0]
            return y + (rva - x), f'no direct refs; neighbour {x:#x} ({c} refs) at {rva - x:+#x}'
        return None, 'no refs'

    def code_target(rva):
        i = old.chunk_of(rva)
        if i is None:  # a leaf function without unwind data: what identical callers call instead
            t, note = data_target(rva, near_ok=False)
            return t, ('leaf (no .pdata): ' + note) if t is not None else 'leaf (no .pdata), not called from identical code'
        if i not in match:
            return None, 'chunk not matched'
        j = match[i]
        ob, oe = old.chunks[i]
        nb, ne = new.chunks[j]
        if ho[i] == hn[j]:
            return nb + (rva - ob), f'identical chunk {ob:#x}->{nb:#x}'
        # align instructions of the changed chunk
        io, inn = disasm(old.img, ob, oe), disasm(new.img, nb, ne)
        ko, kn = [k for _, _, k, _, _ in io], [k for _, _, k, _, _ in inn]
        sm = difflib.SequenceMatcher(None, ko, kn, autojunk=False)
        pos = next((n for n, (off, sz, _, _, _) in enumerate(io) if off <= rva - ob < off + sz), None)
        if pos is None:
            return None, 'site not on an instruction'
        for tagx, a0, a1, b0, b1 in sm.get_opcodes():
            if tagx == 'equal' and a0 <= pos < a1:
                n = b0 + (pos - a0)
                inner = rva - ob - io[pos][0]
                ctx = min(pos - a0, a1 - 1 - pos)
                return nb + inn[n][0] + inner, (f'{how[i]} chunk {ob:#x}->{nb:#x} ({oe - ob:#x}->{ne - nb:#x} bytes, '
                                                f'{sm.ratio():.0%} same), site in an equal run, +-{ctx} insns')
        return None, f'{how[i]} chunk {ob:#x}->{nb:#x}: site falls in a changed part ({sm.ratio():.0%} same)'

    rows = []
    for name, rva, kind, label in ENTRIES:
        if kind == 'code' or old.section(rva) == '.text':
            t, note = code_target(rva)
            if t is not None and kind == 'code' and old.chunk_of(rva) is not None and old.chunks[old.chunk_of(rva)][0] != rva:
                note += ' (not a chunk start)'
        else:
            t, note = data_target(rva)
        rows.append({'name': name, 'kind': kind, 'old': rva, 'new': t, 'note': note})
    for name, rva, where in SITES:
        t, note = code_target(rva)
        rows.append({'name': name, 'kind': 'site', 'old': rva, 'new': t, 'note': note + f'  [{where}]'})

    for r in rows:
        nw = f'{r["new"]:#09x}' if r['new'] is not None else '   ???   '
        print(f'{r["name"]:32s} {r["kind"]:4s} {r["old"]:#09x} -> {nw}  {r["note"]}')
    pairs = [(old.chunks[i][0], new.chunks[j][0], how[i], ho[i] == hn[j]) for i, j in sorted(match.items())]
    pickle.dump(pairs, open(os.path.join(a.cache, f'match_{tag(a.old)}_{tag(a.new)}.pkl'), 'wb'))
    if a.json:
        json.dump(rows, open(a.json, 'w'), indent=1)
        print('wrote', a.json)


if __name__ == '__main__':
    main()
