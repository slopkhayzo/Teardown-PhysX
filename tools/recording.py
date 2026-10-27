"""Inspect, compare and synthesize TeardownPhysX input recordings (*.tdrec, written by src/input/recorder.cpp).

  recording.py info    <file.tdrec>                  frames, duration, events per kind, pauses, seed
  recording.py compare <A> <B>                       frame-by-frame checkpoints of two runs; A and B are each a
                                                     .tdrec, a track.csv or a run folder (its track.csv)
  recording.py synth   <file.tdrec> --script "..." [--fps 60] [--mod ID] [--level FILE] [--layers L] [--mission M]

synth scripts are ';'-separated steps, times in seconds from the first PLAY frame:
  wait 1; hold w 2; turn 300 0 1; click left 1; key space; wait 1
  hold KEY SECONDS     key down now, key up after SECONDS (keys: a-z, 0-9, space, shift, ctrl, esc, e, ...)
  key KEY              press and release in one frame
  turn DX DY SECONDS   raw mouse motion: DX/DY counts per second over SECONDS
  click BUTTON SECONDS left/right/middle mouse button held for SECONDS
  fire BUTTON SECONDS  like click, while 'turn' steps may follow in parallel: the button stays down in the background
  wait SECONDS
Layout (version 3): header (608 bytes), then per frame a 96-byte record
{u32 frame, f32 rawDt, u8 state, u8 paused, u8 active, u8 flags, u32 events, u32 rand, u32 bodies, u32 hash,
 f32 cam[7], f32 eye[3], f32 feet[3], f32 vel[3], f32 health} followed by its events
{u32 msg, u32 rawSize, u64 wParam, u64 lParam, raw bytes}. Version 2 frames are the first 56 bytes (no player).
"""
import argparse, collections, csv, math, os, struct, sys

HEADER = struct.Struct('<8sIIII64s256s128s64sffIIII48s')
FRAME2 = struct.Struct('<IfBBBBIIII3f4f')
FRAME = struct.Struct('<IfBBBBIIII3f4f3f3f3ff')
EVENT = struct.Struct('<IIQQ')
assert HEADER.size == 608 and FRAME2.size == 56 and FRAME.size == 96
VERSION = 3
FLAG_SYNTHETIC = 1

WM = {0x00ff: 'WM_INPUT', 0x0100: 'WM_KEYDOWN', 0x0101: 'WM_KEYUP', 0x0102: 'WM_CHAR', 0x0104: 'WM_SYSKEYDOWN',
      0x0105: 'WM_SYSKEYUP', 0x0200: 'WM_MOUSEMOVE', 0x0201: 'WM_LBUTTONDOWN', 0x0202: 'WM_LBUTTONUP',
      0x0204: 'WM_RBUTTONDOWN', 0x0205: 'WM_RBUTTONUP', 0x0207: 'WM_MBUTTONDOWN', 0x0208: 'WM_MBUTTONUP',
      0x020a: 'WM_MOUSEWHEEL'}
VK = {chr(c): (ord(chr(c).upper()), 0) for c in range(ord('a'), ord('z') + 1)}
VK.update({str(d): (0x30 + d, 0) for d in range(10)})
VK.update({'space': (0x20, 0x39), 'shift': (0x10, 0x2a), 'ctrl': (0x11, 0x1d), 'esc': (0x1b, 0x01),
           'enter': (0x0d, 0x1c), 'tab': (0x09, 0x0f)})
SCAN = {'w': 0x11, 'a': 0x1e, 's': 0x1f, 'd': 0x20, 'e': 0x12, 'q': 0x10, 'r': 0x13, 'f': 0x21, 'c': 0x2e,
        'x': 0x2d, 'z': 0x2c, 'g': 0x22, 'v': 0x2f, 'b': 0x30, 'm': 0x32, 'n': 0x31, 'h': 0x23, 'j': 0x24,
        'k': 0x25, 'l': 0x26, 'i': 0x17, 'o': 0x18, 'p': 0x19, 't': 0x14, 'y': 0x15, 'u': 0x16,
        '1': 0x02, '2': 0x03, '3': 0x04, '4': 0x05, '5': 0x06, '6': 0x07, '7': 0x08, '8': 0x09, '9': 0x0a, '0': 0x0b}
BUTTON = {'left': (0x201, 0x202, 1), 'right': (0x204, 0x205, 2), 'middle': (0x207, 0x208, 0x10)}
cstr = lambda b: b.split(b'\0')[0].decode('latin-1')


def read(path):
    data = open(path, 'rb').read()
    h = HEADER.unpack_from(data, 0)
    if h[0] != b'TDPXREC1':
        sys.exit(f'{path}: not a TeardownPhysX recording')
    fmt = {(2, FRAME2.size): FRAME2, (3, FRAME.size): FRAME}.get((h[1], h[14]))
    if h[2] != HEADER.size or fmt is None:
        sys.exit(f'{path}: recording version {h[1]} (frame {h[14]} bytes); this tool reads versions 2 and 3')
    pad = (0.0,) * (len(FRAME.unpack(bytes(FRAME.size))) - len(FRAME2.unpack(bytes(FRAME2.size))))
    off = HEADER.size
    frames = []
    while off + fmt.size <= len(data):
        fr = fmt.unpack_from(data, off) + (pad if fmt is FRAME2 else ())
        off += fmt.size
        evs = []
        for _ in range(fr[6]):
            msg, raw, wp, lp = EVENT.unpack_from(data, off)
            off += EVENT.size
            evs.append((msg, wp, lp, data[off:off + raw]))
            off += raw
        frames.append((fr, evs))
    return h, frames


def checkpoints(src):
    """frame -> (dt, state, paused, active, events, rand, bodies, hash, x, y, z) from a .tdrec, track.csv or run dir."""
    if os.path.isdir(src):
        src = os.path.join(src, 'track.csv')
    out = {}
    if src.lower().endswith('.tdrec'):
        h, frames = read(src)
        if h[4] & FLAG_SYNTHETIC:
            sys.exit(f'{src}: synthetic recording, it has no checkpoints')
        for fr, _ in frames:
            out[fr[0]] = (fr[1], fr[2], fr[3], fr[4], fr[6], fr[7], fr[8], fr[9], fr[10], fr[11], fr[12], None)
    else:
        f32 = lambda s: struct.unpack('<f', struct.pack('<f', float(s)))[0]  # the CSV holds float32 values (%.9g)
        with open(src, newline='') as f:
            for r in csv.DictReader(f):
                holes = ((int(r['holes']), float(r['hole_x']), float(r['hole_y']), float(r['hole_z']))
                         if r.get('holes') not in (None, '') else None)
                out[int(r['frame'])] = (f32(r['dt']), int(r['state']), int(r['paused']), int(r['active']),
                                        int(r['events']), int(r['rand']), int(r['bodies']), int(r['hash'], 16),
                                        f32(r['x']), f32(r['y']), f32(r['z']), holes)
    return out


def info(path):
    h, frames = read(path)
    dur = sum(f[0][1] for f in frames)
    kinds = collections.Counter(WM.get(e[0], hex(e[0])) for f in frames for e in f[1])
    paused = sum(1 for f in frames if f[0][3])
    inactive = sum(1 for f in frames if not f[0][4])
    synth = ' (synthetic)' if h[4] & FLAG_SYNTHETIC else ''
    print(f'{path}: {len(frames)} frames, {dur:.1f} s recorded time, game build {h[3]:08x}{synth}')
    print(f'level: mod={cstr(h[5])} level={cstr(h[6])} layers={cstr(h[7])} mission={cstr(h[8])}; '
          f'seed {h[11]}; window {h[12]}x{h[13]}')
    print(f'paused frames: {paused}; window inactive: {inactive}; frame dt min/avg/max: '
          f'{min(f[0][1] for f in frames) * 1000:.2f} / {dur / len(frames) * 1000:.2f} / '
          f'{max(f[0][1] for f in frames) * 1000:.2f} ms')
    if not synth:
        print(f'bodies at start/end: {frames[0][0][8]} / {frames[-1][0][8]}; '
              f'main-thread rand() calls: {sum(f[0][7] for f in frames)}')
        if frames[0][0][5] & 1:
            hp = [f[0][26] for f in frames]
            print(f'player state recorded (version 3); health min {min(hp):.2f}')
        else:
            print('no player state (version 2): replays pin the player through the camera')
    for k, v in kinds.most_common():
        print(f'  {k:16s} {v}')


def compare(a, b):
    A, B = checkpoints(a), checkpoints(b)
    common = sorted(set(A) & set(B))
    if not common:
        sys.exit('no frames in common')
    first = {}
    maxcam, sumcam = 0.0, 0.0
    for f in common:
        x, y = A[f], B[f]
        cam = math.dist(x[8:11], y[8:11])
        sumcam += cam
        maxcam = max(maxcam, cam)
        for name, diff in (('camera', cam > 1e-4), ('dt', x[0] != y[0]), ('paused', x[2] != y[2]),
                           ('active', x[3] != y[3]), ('rand', x[5] != y[5]), ('bodies', x[6] != y[6]),
                           ('hash', x[6] != y[6] or x[7] != y[7])):
            if diff and name not in first:
                first[name] = f
    print(f'{len(common)} frames in common (A {len(A)}, B {len(B)}); camera distance mean {sumcam / len(common):.5f} m, '
          f'max {maxcam:.5f} m')
    for name in ('dt', 'active', 'paused', 'rand', 'camera', 'bodies', 'hash'):
        if name in first:
            f = first[name]
            print(f'  {name:7s} first differs at frame {f}: A {A[f]} / B {B[f]}' if name in ('rand', 'bodies') else
                  f'  {name:7s} first differs at frame {f}')
        else:
            print(f'  {name:7s} identical')
    hashes = sum(1 for f in common if A[f][6:8] == B[f][6:8])
    print(f'body transforms identical in {hashes} of {len(common)} frames')
    with_holes = [f for f in common if A[f][11] is not None and B[f][11] is not None]
    if with_holes:
        # Lua MakeHole calls per frame (the Gluon's shots): same count and positions within 1 cm (sums per frame)
        same = lambda a, b: a[0] == b[0] and all(abs(x - y) <= 0.01 * max(1, a[0]) for x, y in zip(a[1:], b[1:]))
        made = [f for f in with_holes if A[f][11][0] or B[f][11][0]]
        ok = [f for f in made if same(A[f][11], B[f][11])]
        bad = [f for f in made if f not in set(ok)]
        na, nb = sum(A[f][11][0] for f in with_holes), sum(B[f][11][0] for f in with_holes)
        print(f'holes (Lua MakeHole): A {na}, B {nb}; frames with holes {len(made)}, identical in {len(ok)}'
              + (f', first difference at frame {bad[0]}' if bad else ''))


def raw_mouse(dx, dy, flags=0, data=0):
    header = struct.pack('<IIQQ', 0, 48, 0, 0)          # RIM_TYPEMOUSE, size, hDevice, wParam
    mouse = struct.pack('<HHHHIiiI', 0, 0, flags, data, 0, dx, dy, 0)
    return header + mouse


def synth(a):
    fps = a.fps
    steps = [s.strip() for s in a.script.split(';') if s.strip()]
    events = collections.defaultdict(list)   # frame -> [(msg, wp, lp, raw)]
    t = 0.0
    frame_of = lambda sec: int(round(sec * fps))
    for s in steps:
        p = s.split()
        if p[0] == 'wait':
            t += float(p[1])
        elif p[0] in ('hold', 'key'):
            k = p[1]
            vk, sc = VK[k]
            sc = sc or SCAN.get(k, 0)
            dur = float(p[2]) if p[0] == 'hold' else 1.0 / fps
            events[frame_of(t)].append((0x100, vk, 1 | (sc << 16), b''))
            events[frame_of(t + dur)].append((0x101, vk, 1 | (sc << 16) | (3 << 30), b''))
            t += dur
        elif p[0] == 'turn':
            dx, dy, dur = float(p[1]), float(p[2]), float(p[3])
            n = max(1, frame_of(dur))
            for i in range(n):
                events[frame_of(t) + i].append((0x00ff, 0, 0, raw_mouse(int(round(dx / fps)), int(round(dy / fps)))))
            t += dur
        elif p[0] in ('click', 'fire'):
            down, up, mk = BUTTON[p[1]]
            dur = float(p[2])
            events[frame_of(t)].append((down, mk, (540 << 16) | 960, b''))
            events[frame_of(t + dur)].append((up, 0, (540 << 16) | 960, b''))
            if p[0] == 'click':
                t += dur
        else:
            sys.exit(f'unknown step: {s}')
    total = max([frame_of(t)] + list(events)) + 1
    enc = lambda s, n: s.encode('latin-1')[:n - 1].ljust(n, b'\0')
    out = [HEADER.pack(b'TDPXREC1', VERSION, HEADER.size, 0x6aa7b1de, FLAG_SYNTHETIC, enc(a.mod, 64),
                       enc(a.level, 256), enc(a.layers, 128), enc(a.mission, 64), 0.0, 0.0, a.seed, 1920, 1080,
                       FRAME.size, b'\0' * 48)]
    for f in range(total):
        evs = events.get(f, [])
        out.append(FRAME.pack(f, 1.0 / fps, 6, 0, 1, 0, len(evs), 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, *([0.0] * 10)))
        for msg, wp, lp, raw in evs:
            out.append(EVENT.pack(msg, len(raw), wp, lp) + raw)
    open(a.file, 'wb').write(b''.join(out))
    print(f'wrote {a.file}: {total} frames ({total / fps:.1f} s at {fps} fps)')


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    i = sub.add_parser('info')
    i.add_argument('file')
    c = sub.add_parser('compare')
    c.add_argument('a')
    c.add_argument('b')
    s = sub.add_parser('synth')
    s.add_argument('file')
    s.add_argument('--script', required=True)
    s.add_argument('--fps', type=float, default=60.0)
    s.add_argument('--seed', type=int, default=1)
    s.add_argument('--mod', default='')
    s.add_argument('--level', default='lee.xml')
    s.add_argument('--layers', default='sandbox')
    s.add_argument('--mission', default='lee_sandbox')
    a = ap.parse_args()
    if a.cmd == 'info':
        info(a.file)
    elif a.cmd == 'compare':
        compare(a.a, a.b)
    else:
        synth(a)


if __name__ == '__main__':
    main()
