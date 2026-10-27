"""Summarize a TeardownPhysX trace: frame times, where the time went, and what ran in the slow frames.

usage: python tools/trace_report.py <run dir> [--spike-ms 0] [--top 10] [--perfetto spikes|all|none]

Writes <run>/summary.txt (also printed), <run>/frames.csv and <run>/perfetto.json (open in ui.perfetto.dev).
Times: "self" = a hooked function's own time minus hooked functions it called on the same thread;
"jobs" = time spent in job_* functions on all threads (worker threads + the main thread helping).
"""
import argparse, bisect, collections, csv, json, os, statistics, struct, sys

INSTANT = {'profiler_category', 'state_switch', 'bench_marker', 'trace_drops'}
MARKERS = {1: 'level running', 2: 'carving starts', 3: 'carving ends', 4: 'level requested', 9: 'bench failed'}
STATES = ['NONE', 'SPLASH', 'MENU', 'UI', 'LOADING', 'MENU_LOADING', 'PLAY', 'EDIT', 'QUIT']


def load(run):
    meta = json.load(open(os.path.join(run, 'meta.json')))
    names = {int(k): v for k, v in meta['events'].items()}
    raw = open(os.path.join(run, 'trace.bin'), 'rb').read()
    magic, ver, rsize, freq, qstart = struct.unpack_from('<8sIIQQ', raw, 0)
    if magic != b'TDPXTRC1':
        sys.exit('not a TeardownPhysX trace')
    n = (len(raw) - 32) // 32
    recs = list(struct.iter_unpack('<QQIIQ', raw[32:32 + n * 32]))
    recs.sort(key=lambda r: (r[0], -r[1]))
    return meta, names, freq, qstart, recs


def load_gpu(run, meta):
    """nvidia-smi samples from run_bench.ps1 -GpuLog: [(seconds since the trace start, util %, MHz, W)]."""
    path = os.path.join(run, 'gpu.csv')
    if 'wall_start_ms' not in meta or not os.path.exists(path):
        return []
    import datetime
    out = []
    for row in csv.reader(open(path, encoding='utf-8', errors='replace')):
        try:
            t = datetime.datetime.strptime(row[0].strip(), '%Y/%m/%d %H:%M:%S.%f').timestamp()
            out.append((t - meta['wall_start_ms'] / 1000.0, float(row[1]), float(row[2]), float(row[3])))
        except (ValueError, IndexError):
            continue  # header, or a sample the driver could not fill
    return out


def load_threads(run, meta):
    """The plugin's thread census (threads.csv): {second since the trace start: row}, CPU ms per second per group."""
    path = os.path.join(run, 'threads.csv')
    if not os.path.exists(path):
        return {}
    out = {}
    for row in csv.DictReader(open(path, encoding='utf-8', errors='replace')):
        try:
            t = (int(row['qpc']) - meta['qpc_start']) / meta['qpc_freq']
            out[int(t) - 1] = {k: float(v) for k, v in row.items() if k != 'qpc'}  # the second it measured
        except (ValueError, KeyError):
            continue
    return out


def pct(values, p):
    if not values:
        return 0.0
    s = sorted(values)
    k = min(len(s) - 1, max(0, int(round(p / 100.0 * (len(s) - 1)))))
    return s[k]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--spike-ms', type=float, default=0.0, help='0 = max(33.4, 2.5 x median interval)')
    ap.add_argument('--top', type=int, default=10)
    ap.add_argument('--perfetto', default='spikes', choices=['spikes', 'all', 'none'])
    a = ap.parse_args()
    meta, names, freq, qstart, recs = load(a.run)
    gpu = load_gpu(a.run, meta)
    thr = load_threads(a.run, meta)
    gpu_by_sec = collections.defaultdict(list)
    for s, u, _, _ in gpu:
        gpu_by_sec[int(s)].append(u)
    cats = meta.get('profiler_categories', [])
    ms = 1000.0 / freq
    name_of = lambda i: names.get(i, f'ev{i}')
    out = []
    say = out.append

    # ---- threads, frames ------------------------------------------------------------------------------
    # Frame boundary: FrameUpdate runs once per frame in every state (FrameDriver only in some loops).
    frame_id = next((k for k, v in names.items() if v == 'frame_update'), None)
    if frame_id is None or not any(r[2] == frame_id for r in recs):
        frame_id = next((k for k, v in names.items() if v == 'frame'), None)
    frames = [r for r in recs if r[2] == frame_id]
    if len(frames) < 3:
        sys.exit('too few frames in the trace')
    main_tid = collections.Counter(r[3] for r in frames).most_common(1)[0][0]
    starts = [f[0] for f in frames]
    intervals = [(starts[i + 1] - starts[i]) * ms for i in range(len(starts) - 1)]

    # ---- self time per thread -------------------------------------------------------------------------
    self_time = {}  # record index -> self ticks
    by_tid = collections.defaultdict(list)
    for idx, r in enumerate(recs):
        if name_of(r[2]) in INSTANT or r[1] <= r[0]:
            continue
        by_tid[r[3]].append(idx)
    for tid, idxs in by_tid.items():
        stack = []
        child = collections.defaultdict(int)
        for idx in idxs:
            r = recs[idx]
            while stack and recs[stack[-1]][1] <= r[0]:
                stack.pop()
            if stack:
                child[stack[-1]] += r[1] - r[0]
            stack.append(idx)
        for idx in idxs:
            r = recs[idx]
            self_time[idx] = max(0, (r[1] - r[0]) - child[idx])

    # ---- per-frame aggregation ------------------------------------------------------------------------
    nF = len(intervals)
    f_self = [collections.Counter() for _ in range(nF)]   # main thread self ms by name
    f_jobs = [collections.Counter() for _ in range(nF)]   # job ms (all threads) by name
    f_cat = [collections.Counter() for _ in range(nF)]    # engine profiler categories (ms)
    f_holes = [[0, 0, 0.0] for _ in range(nF)]            # bench holes, voxels removed, ms
    f_mh = [[0, 0, 0.0, 0] for _ in range(nF)]            # MakeHoleCore calls, voxels, ms, impact calls
    f_state = [frames[i][4] & 0xffffffff for i in range(nF)]
    f_bodies = [frames[i][4] >> 32 for i in range(nF)]
    f_sim = [0] * nF
    totals_self = collections.Counter()
    totals_jobs = collections.Counter()
    calls = collections.Counter()
    markers = []
    mh_calls = []  # (ms, voxels, spheres, impact, t0, t1, frame)
    drops = 0
    for idx, r in enumerate(recs):
        t0, t1, eid, tid, arg = r
        nm = name_of(eid)
        fi = bisect.bisect_right(starts, t0) - 1
        inframe = 0 <= fi < nF
        if nm == 'profiler_category':
            if inframe:
                c = arg & 0xffffffff
                sec = struct.unpack('<f', struct.pack('<I', arg >> 32))[0]
                f_cat[fi][cats[c] if c < len(cats) else f'cat{c}'] += sec * 1000.0
            continue
        if nm == 'bench_marker':
            markers.append((t0, MARKERS.get(arg, f'marker {arg}')))
            continue
        if nm == 'state_switch':
            o, n = arg & 0xffffffff, arg >> 32
            markers.append((t0, f'state {STATES[o] if o < 9 else o} -> {STATES[n] if n < 9 else n}'))
            continue
        if nm == 'trace_drops':
            drops += arg
            continue
        calls[nm] += 1
        dur = (t1 - t0) * ms
        if nm.startswith('job_'):
            totals_jobs[nm] += dur
            if inframe:
                f_jobs[fi][nm] += dur
        if tid == main_tid:
            s = self_time.get(idx, 0) * ms
            totals_self[nm] += s
            if inframe:
                f_self[fi][nm] += s
        if not inframe:
            continue
        if nm == 'bench_hole' and (arg >> 32) & 1:
            f_holes[fi][0] += 1
            f_holes[fi][1] += arg & 0xffffffff
            f_holes[fi][2] += dur
        elif nm == 'makehole_core':
            vox = arg & 0xffffffff
            spheres = (arg >> 32) & 0x7fffffff
            impact = arg >> 63
            f_mh[fi][0] += 1
            f_mh[fi][1] += vox
            f_mh[fi][2] += dur
            f_mh[fi][3] += impact
            mh_calls.append((dur, vox, spheres, impact, t0, t1, fi))
        elif nm == 'physics_update':
            f_sim[fi] = max(f_sim[fi], arg & 0xffffffff)

    # ---- summary --------------------------------------------------------------------------------------
    med = statistics.median(intervals)
    spike_ms = a.spike_ms or max(33.4, 2.5 * med)
    play = [i for i in range(nF) if f_state[i] == 6]
    say(f'run: {a.run}')
    say(f'frames: {nF}, main thread {main_tid}, trace records {len(recs)}, dropped {drops}')
    for t, m in markers:
        say(f'  {(t - qstart) * ms / 1000.0:8.2f} s  {m}')
    ints_play = [intervals[i] for i in play] or intervals
    say(f'frame interval (PLAY frames: {len(play)}): median {statistics.median(ints_play):.2f} ms, '
        f'p90 {pct(ints_play, 90):.2f}, p99 {pct(ints_play, 99):.2f}, max {max(ints_play):.2f} ms')
    spikes = [i for i in range(nF) if intervals[i] > spike_ms]
    say(f'spikes (> {spike_ms:.1f} ms): {len(spikes)}')
    play_set = set(play)
    mh_play = [c for c in mh_calls if c[6] in play_set]
    if mh_play:
        # impact damage: the game's own (end of Physics::update) and the PhysX offload's, both pass a body
        imp = sorted(c[1] for c in mh_play if c[3])
        oth = [c for c in mh_play if not c[3]]
        line = (f'MakeHole core in PLAY: {len(oth)} script/tool calls ({sum(c[1] for c in oth)} voxels), '
                f'{len(imp)} impact-damage calls ({sum(imp)} voxels')
        if imp:
            line += f'; per call p50 {imp[len(imp) // 2]}, p90 {pct(imp, 90):.0f}, max {imp[-1]}'
        say(line + f'), {sum(c[0] for c in mh_play):.0f} ms in all')

    run_win = None
    rs = [t for t, m in markers if m == 'carving starts']
    re_ = [t for t, m in markers if m == 'carving ends']
    if rs:
        t_end = re_[0] if re_ else frames[-1][0]
        run_win = [i for i in range(nF) if rs[0] <= starts[i] < t_end]
        ints = [intervals[i] for i in run_win]
        holes = sum(f_holes[i][0] for i in run_win)
        vox = sum(f_holes[i][1] for i in run_win)
        secs = (t_end - rs[0]) * ms / 1000.0
        say(f'bench window: {secs:.1f} s, {len(run_win)} frames ({len(run_win) / max(secs, 1e-9):.1f} fps), '
            f'{holes} holes, {vox} voxels removed')
        say(f'  frame interval: median {statistics.median(ints):.2f} ms, p90 {pct(ints, 90):.2f}, '
            f'p99 {pct(ints, 99):.2f}, max {max(ints):.2f} ms; spikes {sum(1 for x in ints if x > spike_ms)}')
        cat_tot = collections.Counter()
        for i in run_win:
            cat_tot.update(f_cat[i])
        say('  engine profiler, mean ms per frame in the window: ' +
            ', '.join(f'{k} {v / len(run_win):.2f}' for k, v in cat_tot.most_common() if v > 0))
        win_set = set(run_win)
        mh_win = [c for c in mh_calls if c[6] in win_set]
        if mh_win:
            d = [c[0] for c in mh_win]
            say(f'  MakeHole core: {len(mh_win)} calls ({sum(c[3] for c in mh_win)} impact-damage), '
                f'mean {statistics.mean(d):.2f} ms, p95 {pct(d, 95):.2f}, max {max(d):.2f} ms, '
                f'{sum(c[1] for c in mh_win)} voxels')

    # ---- timeline of the last PLAY stretch: what grows per frame as the scene gets heavier --------------
    play_runs = []  # contiguous PLAY frame index ranges
    i = 0
    while i < nF:
        if f_state[i] == 6:
            j = i
            while j + 1 < nF and f_state[j + 1] == 6:
                j += 1
            play_runs.append((i, j))
            i = j + 1
        else:
            i += 1
    tick_id = next((k for k, v in names.items() if v == 'scene_update'), None)
    tick_starts = sorted(r[0] for r in recs if r[2] == tick_id) if tick_id is not None else []
    if play_runs:
        lo, hi = max(play_runs, key=lambda x: x[1] - x[0])
        t_lo = (starts[lo] - qstart) * ms / 1000.0
        say(f'timeline of the longest PLAY stretch ({t_lo:.1f}-{(starts[hi] - qstart) * ms / 1000.0:.1f} s), '
            f'per-frame means per second:')
        # px: everything the PhysX plugin adds on the main thread (prepare, sync, step, compare/write-back);
        # physics_update is the native step alone, the engine profiler's "phys" includes both.
        px_names = ('px_prepare', 'px_sync', 'px_step', 'px_compare', 'px_writeback')
        cols = ['physics_update', 'px', 'makehole_core', 'paint_scene', 'scripts_update', 'particles_update',
                'fire_update', 'frame_update']
        gpu_hdr = ' | gpu% gpu ms' if gpu else ''
        # CPU per second from the thread census: machine busy %, then logical processors kept busy by the game's
        # main thread, its workers, PhysX's workers and the graphics driver
        gpu_hdr += ' | cpu: busy% main game physx gfx' if thr else ''
        say('     t   fps  ticks  frame(max)  bodies  sim  holes | ' + '  '.join(f'{c[:10]:>10s}' for c in cols) +
            ' | jobs: solver nearph breakage paint | engine: destr phys partic script render' + gpu_hdr)
        buckets = collections.OrderedDict()
        for k in range(lo, hi + 1):
            sec = int((starts[k] - qstart) * ms / 1000.0)
            buckets.setdefault(sec, []).append(k)
        for sec, idxs in buckets.items():
            n = len(idxs)
            if n == 0:
                continue
            fr = [intervals[k] for k in idxs]
            s = collections.Counter()
            jb = collections.Counter()
            ct = collections.Counter()
            for k in idxs:
                s.update(f_self[k])
                jb.update(f_jobs[k])
                ct.update(f_cat[k])
            mh_ms = sum(f_mh[k][2] for k in idxs)
            t0s = int(sec * freq / 1000.0 * 1000.0) + qstart
            t1s = t0s + int(freq)
            ticks = bisect.bisect_left(tick_starts, t1s) - bisect.bisect_left(tick_starts, t0s)
            vals = []
            for c in cols:
                if c == 'makehole_core':
                    v = mh_ms / n
                elif c == 'px':
                    v = sum(s[p] for p in px_names) / n
                else:
                    v = s[c] / n
                vals.append(f'{v:10.2f}')
            gpu_col = ''
            if gpu:
                g = gpu_by_sec.get(sec)
                # busy share of the second x mean frame interval: the GPU time one frame needs
                gpu_col = (f' | {statistics.mean(g):4.0f} {statistics.mean(g) / 100.0 * statistics.mean(fr):6.1f}'
                           if g else ' |    -      -')
            if thr:
                r = thr.get(sec)
                gpu_col += (f' | {r["machine_busy_pct"]:4.0f}% {r["main_ms"] / 1000:4.2f} {r["game_ms"] / 1000:4.1f} '
                            f'{r["physx_ms"] / 1000:5.1f} {r["graphics_ms"] / 1000:4.1f}' if r else ' |    -')
            say(f'  {sec:4d} {n:5d} {ticks:5d} {statistics.mean(fr):6.1f}({max(fr):5.1f}) '
                f'{max(f_bodies[k] for k in idxs):6d} {max(f_sim[k] for k in idxs):4d} '
                f'{sum(f_mh[k][0] for k in idxs):5d} | ' + '  '.join(vals) +
                f' | {jb["job_solver"] / n:6.1f} {jb["job_nearphase_collect"] / n:6.1f} '
                f'{jb["job_breakage"] / n:6.1f} {jb["job_paint_slices"] / n:5.1f}'
                f' | {ct["destruction"] / n:5.1f} {ct["physics"] / n:5.1f} {ct["particle"] / n:5.1f} '
                f'{ct["script"] / n + ct["script_server"] / n:5.1f} {ct["rendering"] / n:5.1f}' + gpu_col)

    def table(counter, title, n):
        say(title)
        for k, v in counter.most_common(n):
            say(f'  {k:28s} {v:10.1f} ms  ({calls[k]} calls)')

    win = run_win if run_win else list(range(nF))
    ws, wj = collections.Counter(), collections.Counter()
    for i in win:
        ws.update(f_self[i])
        wj.update(f_jobs[i])
    table(ws, f'main-thread self time ({"bench window" if run_win else "whole run"}):', a.top + 6)
    table(wj, f'job time, all threads ({"bench window" if run_win else "whole run"}):', a.top)

    say(f'worst {min(a.top, len(spikes))} spikes:')
    for i in sorted(spikes, key=lambda i: -intervals[i])[:a.top]:
        tsec = (starts[i] - qstart) * ms / 1000.0
        top_self = ', '.join(f'{k} {v:.1f}' for k, v in f_self[i].most_common(6) if v >= 0.3)
        top_jobs = ', '.join(f'{k} {v:.1f}' for k, v in f_jobs[i].most_common(4) if v >= 0.3)
        top_cat = ', '.join(f'{k} {v:.1f}' for k, v in f_cat[i].most_common(4) if v >= 0.3)
        say(f'  {tsec:8.2f} s  frame {intervals[i]:6.1f} ms  state {STATES[f_state[i]] if f_state[i] < 9 else f_state[i]}'
            f'  bodies {f_bodies[i]} simulated {f_sim[i]}  holes {f_holes[i][0]} ({f_holes[i][1]} vox)'
            f'  makehole {f_mh[i][0]}x {f_mh[i][2]:.1f} ms')
        say(f'             self: {top_self}')
        if top_jobs:
            say(f'             jobs: {top_jobs}')
        if top_cat:
            say(f'             engine: {top_cat}')

    if mh_calls:
        say(f'slowest MakeHole core calls (ms, voxels removed, spheres, impact):')
        for c in sorted(mh_calls, key=lambda c: -c[0])[:a.top]:
            # what ran inside this call: main-thread children + jobs on any thread
            inside = collections.Counter()
            for idx in range(bisect.bisect_left(recs, (c[4],)), len(recs)):
                r = recs[idx]
                if r[0] > c[5]:
                    break
                nm = name_of(r[2])
                if nm in INSTANT or nm == 'makehole_core':
                    continue
                if r[3] == main_tid:
                    inside[nm] += self_time.get(idx, 0) * ms
                elif nm.startswith('job_') or nm.startswith('vox_'):
                    inside['[w] ' + nm] += (r[1] - r[0]) * ms
            say(f'  {c[0]:7.2f} ms  {c[1]:6d} vox  {c[2]} sph  {"impact" if c[3] else "hole"}  '
                f'at {(c[4] - qstart) * ms / 1000.0:.2f} s: ' +
                ', '.join(f'{k} {v:.2f}' for k, v in inside.most_common(7) if v >= 0.05))

    # ---- split anatomy: what one split_per_body call (shape-level connectivity) consists of --------------
    spb = [(idx, r) for idx, r in enumerate(recs) if name_of(r[2]) == 'split_per_body']
    if spb:
        say('slowest split_per_body calls (shape-level connectivity of a multi-shape body):')
        say('       ms  rounds  floods  queries (ms)  touch tests (unique, ms)')
        rows = []
        for idx, r in spb:
            t0, t1, tid = r[0], r[1], r[3]
            cnt = collections.Counter()
            tms = collections.Counter()
            pairs = set()
            for j in range(idx + 1, len(recs)):
                q = recs[j]
                if q[0] > t1:
                    break
                if q[3] != tid or q[1] > t1:
                    continue
                nm = name_of(q[2])
                cnt[nm] += 1
                tms[nm] += (q[1] - q[0]) * ms
                if nm == 'shape_touch_test':
                    pairs.add(q[4] & ((1 << 62) - 1))
                    if q[4] >> 63:
                        cnt['touch_hit'] += 1
            rows.append(((t1 - t0) * ms, cnt, tms, len(pairs)))
        for d, cnt, tms, up in sorted(rows, key=lambda x: -x[0])[:a.top]:
            say(f'  {d:7.2f}  {cnt["split_group_bodies"]:6d}  {cnt["split_flood"]:6d}  '
                f'{cnt["scene_query_shapes"]:6d} ({tms["scene_query_shapes"]:6.2f})  '
                f'{cnt["shape_touch_test"]:7d} ({up}, {tms["shape_touch_test"]:6.2f})'
                + (f'  cache hits {cnt["touch_hit"]}' if cnt['touch_hit'] else ''))
        tot = sum(x[0] for x in rows)
        tt = sum(x[2]['shape_touch_test'] for x in rows)
        tq = sum(x[2]['scene_query_shapes'] for x in rows)
        ntt = sum(x[1]['shape_touch_test'] for x in rows)
        nhit = sum(x[1]['touch_hit'] for x in rows)
        nup = sum(x[3] for x in rows)
        say(f'  all {len(rows)} calls: {tot:.1f} ms, touch tests {ntt} ({nup} unique pairs per call summed, '
            f'{nhit} cache hits) {tt:.1f} ms, shape queries {tq:.1f} ms')

    text = '\n'.join(out)
    print(text)
    open(os.path.join(a.run, 'summary.txt'), 'w').write(text + '\n')

    # ---- frames.csv -------------------------------------------------------------------------------------
    keys_self = [k for k, _ in totals_self.most_common(14)]
    keys_jobs = [k for k, _ in totals_jobs.most_common(8)]
    with open(os.path.join(a.run, 'frames.csv'), 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['frame', 't_s', 'interval_ms', 'state', 'bodies', 'simulated', 'holes', 'hole_voxels',
                    'makehole_calls', 'makehole_ms'] + [f'self_{k}' for k in keys_self] +
                   [f'jobs_{k}' for k in keys_jobs] + [f'cat_{c}' for c in cats])
        for i in range(nF):
            w.writerow([i, f'{(starts[i] - qstart) * ms / 1000.0:.4f}', f'{intervals[i]:.3f}', f_state[i],
                        f_bodies[i], f_sim[i], f_holes[i][0], f_holes[i][1], f_mh[i][0], f'{f_mh[i][2]:.3f}'] +
                       [f'{f_self[i][k]:.3f}' for k in keys_self] + [f'{f_jobs[i][k]:.3f}' for k in keys_jobs] +
                       [f'{f_cat[i][c]:.3f}' for c in cats])

    # ---- perfetto ---------------------------------------------------------------------------------------
    if a.perfetto == 'none':
        return
    keep = None
    if a.perfetto == 'spikes':
        keep = []
        for i in sorted(spikes, key=lambda i: -intervals[i])[:12]:
            keep.append((starts[i] - int(0.25 * freq), starts[i + 1] + int(0.1 * freq)))
    ev = [{'name': 'process_name', 'ph': 'M', 'pid': 1, 'args': {'name': 'teardown'}},
          {'name': 'thread_name', 'ph': 'M', 'pid': 1, 'tid': main_tid, 'args': {'name': 'main'}}]
    for r in recs:
        t0, t1, eid, tid, arg = r
        if keep is not None and not any(lo <= t0 <= hi for lo, hi in keep):
            continue
        nm = name_of(eid)
        ts = (t0 - qstart) * ms * 1000.0
        if nm in INSTANT:
            if nm == 'bench_marker':
                ev.append({'name': MARKERS.get(arg, nm), 'ph': 'i', 's': 'g', 'ts': ts, 'pid': 1, 'tid': tid})
            continue
        ev.append({'name': nm, 'ph': 'X', 'ts': ts, 'dur': max(0.01, (t1 - t0) * ms * 1000.0), 'pid': 1,
                   'tid': tid, 'args': {'arg': arg}})
    json.dump({'traceEvents': ev, 'displayTimeUnit': 'ms'}, open(os.path.join(a.run, 'perfetto.json'), 'w'))


if __name__ == '__main__':
    main()
