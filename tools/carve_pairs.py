"""Where the engine's hole x shape bookkeeping goes, from a traced run (DestrCarve's argument: shapes | holes << 32).

Every MakeHoleCore call gathers the shapes in the bounds of all its holes and keeps a 960-byte record per hole for
each of them; the carve then visits every pair. This splits the carve and gather time by caller and by holes per
call, so a fix can aim at the calls that make the pairs. Callers with a target body: impact damage, and the plugin's
debris tool (its fragment cuts are tiles of 16x16 holes); without one: scripts and weapons.

usage: python -I carve_pairs.py <run folder> [--top 10]
"""
import argparse, bisect, collections, json, os, struct


def load(run):
    meta = json.load(open(os.path.join(run, 'meta.json')))
    names = {int(k): v for k, v in meta['events'].items()}
    raw = open(os.path.join(run, 'trace.bin'), 'rb').read()
    magic, ver, rsize, freq, qstart = struct.unpack_from('<8sIIQQ', raw, 0)
    n = (len(raw) - 32) // 32
    return names, freq, list(struct.iter_unpack('<QQIIQ', raw[32:32 + n * 32]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--top', type=int, default=10)
    a = ap.parse_args()
    names, freq, recs = load(a.run)
    ids = {v: k for k, v in names.items()}
    us = 1e6 / freq
    want = {ids.get('makehole_core'): 'mh', ids.get('destr_carve'): 'carve', ids.get('destr_gather'): 'gather'}
    ev = collections.defaultdict(list)  # kind -> (t0, t1, thread, arg)
    for t0, t1, eid, tid, arg in recs:
        k = want.get(eid)
        if k: ev[k].append((t0, t1, tid, arg))
    for k in ev: ev[k].sort()
    # each carve / gather inside the MakeHoleCore call that contains it (same thread)
    calls = []
    by_thread = collections.defaultdict(list)
    for t0, t1, tid, arg in ev['mh']:
        c = {'t0': t0, 'dur': (t1 - t0) * us, 'spheres': (arg >> 32) & 0x7fffffff, 'impact': bool(arg >> 63),
             'voxels': arg & 0xffffffff, 'carve': 0.0, 'gather': 0.0, 'shapes': 0, 'holes': 0}
        calls.append(c)
        by_thread[tid].append((t0, t1, c))
    starts = {tid: [x[0] for x in v] for tid, v in by_thread.items()}
    def owner(t0, tid):
        lst = by_thread.get(tid)
        if not lst: return None
        i = bisect.bisect_right(starts[tid], t0) - 1
        return lst[i][2] if i >= 0 and lst[i][1] >= t0 else None
    for t0, t1, tid, arg in ev['carve']:
        c = owner(t0, tid)
        if c:
            c['carve'] += (t1 - t0) * us
            c['shapes'] = max(c['shapes'], arg & 0xffffffff)
            c['holes'] = max(c['holes'], arg >> 32)
    for t0, t1, tid, arg in ev['gather']:
        c = owner(t0, tid)
        if c: c['gather'] += (t1 - t0) * us
    if not calls:
        print('no MakeHoleCore calls in this trace')
        return
    span = (max(r[1] for r in recs) - min(r[0] for r in recs)) * us / 1e6
    print('%s: %d MakeHoleCore calls over %.0f s' % (os.path.basename(a.run.rstrip('/\\')), len(calls), span))
    def summary(label, cs):
        if not cs: return
        mh = sum(c['dur'] for c in cs) / 1e3
        carve = sum(c['carve'] for c in cs) / 1e3
        gather = sum(c['gather'] for c in cs) / 1e3
        pairs = sum(c['shapes'] * c['holes'] for c in cs)
        print('  %-26s %6d calls  MakeHoleCore %8.1f ms  carve %7.1f ms  gather %6.1f ms  pairs %9d  %5.2f us/pair  '
              'holes/call %5.1f  shapes/call %6.1f' % (
                  label, len(cs), mh, carve, gather, pairs, carve * 1e3 / max(1, pairs),
                  sum(c['holes'] for c in cs) / len(cs), sum(c['shapes'] for c in cs) / len(cs)))
    summary('all', calls)
    summary('target body (impact, tool)', [c for c in calls if c['impact']])
    summary('no target (scripts, weapons)', [c for c in calls if not c['impact']])
    print('by holes per call:')
    for lo, hi in ((0, 1), (2, 8), (9, 64), (65, 256), (257, 10 ** 9)):
        summary('%d-%s holes' % (lo, hi if hi < 10 ** 9 else 'more'), [c for c in calls if lo <= c['holes'] <= hi])
    print('by shapes gathered per call:')
    for lo, hi in ((0, 8), (9, 64), (65, 512), (513, 10 ** 9)):
        summary('%d-%s shapes' % (lo, hi if hi < 10 ** 9 else 'more'), [c for c in calls if lo <= c['shapes'] <= hi])
    print('top calls by carve time:')
    for c in sorted(calls, key=lambda c: -c['carve'])[:a.top]:
        print('  carve %7.2f ms, gather %6.2f ms, MakeHoleCore %7.2f ms: %s, %d holes x %d shapes = %d pairs, %d voxels'
              % (c['carve'] / 1e3, c['gather'] / 1e3, c['dur'] / 1e3, 'target body' if c['impact'] else 'no target',
                 c['holes'], c['shapes'], c['holes'] * c['shapes'], c['voxels']))


if __name__ == '__main__':
    main()
