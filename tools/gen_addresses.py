"""Generate src/td/addresses_<build>.inc from the decrypted teardown.exe.

usage: python tools/gen_addresses.py [--binary research/re/bin/teardown_2_2_0_unpacked.exe]

Every code entry records its RVA and the first 16 bytes found there. At run time the plugin hooks a function only
when the live bytes match, so another game build (or code that is still encrypted) disables the hook instead of
corrupting it. Data entries carry no bytes. Names and addresses come from research/docs/re-notes.md.

The RVAs below are for GAME_VERSION. re-notes.md and many comments in src/ cite Teardown 2.1.0 addresses
(0x14xxxxxxx); tools/port_addresses.py maps them, and moves this table to the next game build.
"""
import argparse, hashlib, os

import pefile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..'))
GAME_VERSION = '2.2.0'  # Steam build 26320217, teardown.exe sha256 ea9ef798..., PE timestamp 0x6ae468bd

# (name, rva, kind, timed-event label or '')   kind: 'code' or 'data'
ENTRIES = [
    # frame / tick
    ('FrameDriver',          0x0af090, 'code', 'frame'),
    ('FrameUpdate',          0x072ab0, 'code', 'frame_update'),
    ('GameTick',             0x07d010, 'code', 'tick'),
    ('SceneUpdate',          0x0fa400, 'code', 'scene_update'),
    ('ScriptsUpdate',        0x4b96c0, 'code', 'scripts_update'),
    ('StateSwitch',          0x07a8f0, 'code', ''),
    ('ProfilerAdd',          0x469780, 'code', ''),
    # physics
    ('PhysicsUpdate',        0x0b9d50, 'code', 'physics_update'),
    ('PhysicsCollide',       0x0bc520, 'code', 'physics_collide'),
    ('JobBroadPhase',        0x0b55a0, 'code', 'job_broadphase'),
    ('JobNearPhaseCollect',  0x0b57c0, 'code', 'job_nearphase_collect'),
    ('JobNearPhaseAdd',      0x0b57a0, 'code', 'job_nearphase_add'),
    ('JobSolver',            0x608ca0, 'code', 'job_solver'),
    ('JobShapeInterp',       0x0dd9b0, 'code', 'job_shape_interp'),
    # destruction / split
    ('MakeHoleCore',         0x13ffd0, 'code', 'makehole_core'),
    ('DestrGather',          0x1415d0, 'code', 'destr_gather'),
    ('BreakageDriver',       0x1450c0, 'code', 'breakage_driver'),
    ('DestrCarve',           0x141ff0, 'code', 'destr_carve'),
    ('DestrFinish',          0x142470, 'code', 'destr_finish_split'),
    ('DestrMTUpdate',        0x13e580, 'code', 'destr_mt_update'),
    ('Destr_b060',           0x1432d0, 'code', 'destr_b060'),
    ('Destr_53a0',           0x13d600, 'code', 'destr_53a0'),
    ('Destr_c100',           0x144370, 'code', 'destr_c100'),
    ('Destr_de30',           0x1460c0, 'code', 'destr_de30'),
    ('BreakIslands',         0x146700, 'code', 'break_islands'),
    ('BreakIslandsMT',       0x1469a0, 'code', 'break_islands_mt'),
    ('JobBreakage',          0x13e0c0, 'code', 'job_breakage'),
    ('JobCclRelabel',        0x693890, 'code', 'job_ccl_relabel'),
    ('VoxBoundsScan',        0x113db0, 'code', 'vox_bounds_scan'),
    ('VoxWholeA',            0x10d740, 'code', 'vox_whole_a'),
    ('VoxWholeB',            0x10df00, 'code', 'vox_whole_b'),
    ('VoxRegionUpdate',      0x1141e0, 'code', 'vox_region_update'),
    ('VoxRegionFinish',      0x114460, 'code', 'vox_region_finish'),
    ('VoxRegionCcl',         0x6971f0, 'code', 'vox_region_ccl'),
    ('VoxFullRelabel',       0x697080, 'code', 'vox_full_relabel'),
    # inside DestrFinish (0x13a200): islands -> new bodies
    ('SplitBodyCheck',       0x02f9d0, 'code', 'split_body_check'),
    ('SplitPerBody',         0x140820, 'code', 'split_per_body'),
    # inside SplitPerBody: shape-level connectivity of multi-shape bodies (flood fill over touching shapes,
    # re-run on a growing region while more than one boundary-touching group remains)
    ('SplitCollect',         0x13b910, 'code', 'split_collect'),
    ('SplitFlood',           0x141190, 'code', 'split_flood'),
    ('SplitGroupBodies',     0x140dd0, 'code', 'split_group_bodies'),
    ('ShapeTouchTest',       0x0b8db0, 'code', 'shape_touch_test'),
    ('SceneQueryShapes',     0x0dcc20, 'code', 'scene_query_shapes'),
    ('SplitIslands',         0x0c20d0, 'code', 'split_islands'),
    ('SplitBodyOp',          0x031900, 'code', 'split_body_op'),
    ('SplitJobs',            0x138af0, 'code', 'split_jobs'),
    ('SplitNewBodyUpdate',   0x02eac0, 'code', 'split_newbody_update'),
    ('SplitWait',            0x147630, 'code', 'split_wait'),
    ('SplitBreakageJobs',    0x13b9d0, 'code', 'split_breakage_jobs'),
    ('SplitEntryFinish',     0x145c10, 'code', 'split_entry_finish'),
    ('PhysicsBodyUpdate',    0x0bd870, 'code', 'physics_body_update'),
    ('PhysicsBodiesUpdate',  0x0bd750, 'code', 'physics_bodies_update'),
    ('SceneShapesChanged',   0x0e6ae0, 'code', 'scene_shapes_changed'),
    ('JobCollisionData',     0x0dd5c0, 'code', 'job_collision_data'),
    ('JobPaintSlices',       0x0dd690, 'code', 'job_paint_slices'),
    ('LuaPaint',             0x492760, 'code', 'lua_paint'),
    # other subsystems
    ('ParticlesUpdate',      0x43d560, 'code', 'particles_update'),
    ('FireUpdate',           0x168460, 'code', 'fire_update'),
    # functions the plugin calls
    ('MakeHole',             0x1404d0, 'code', ''),
    ('PaintScene',           0x0e6f10, 'code', 'paint_scene'),
    ('BodyWake',             0x031cc0, 'code', ''),
    ('ShapeWorldTransform',  0x4ec4f0, 'code', ''),
    ('ShapeInterpTransform', 0x4ec670, 'code', ''),
    ('StartLevel',           0x07b180, 'code', ''),
    ('RequestState',         0x079990, 'code', ''),
    ('PlayerSetTransform',   0x0c9e60, 'code', ''),   # (player, const Transform* feet, bool withPitch)
    ('ModById',              0x401b70, 'code', ''),
    ('LevelDescCtor',        0x06d0f0, 'code', ''),
    ('PathResolveLevel',     0x470ee0, 'code', ''),
    ('TdStringFromCStr',     0x515710, 'code', ''),
    ('TdStringCopy',         0x5158d0, 'code', ''),
    ('TdStringDtor',         0x515880, 'code', ''),
    # input record/replay
    ('WndProc',              0x505e20, 'code', ''),
    ('MessagePump',          0x508c00, 'code', ''),
    ('TimerElapsed',         0x520580, 'code', ''),
    ('WindowIsActive',       0x507310, 'code', ''),
    ('GameLog',              0x6aa740, 'code', 'game_log'),
    ('SeedFromTime',         0x513570, 'code', ''),   # srand((int)time(NULL)): Game ctor and a generator at 0x6a61c0
    ('JobRun',               0x50fba0, 'code', ''),   # job manager: job->Run(), then marks the job done
    ('JobWait',              0x510710, 'code', ''),   # wait for a job (non-workers wake the extra worker, then block)
    ('JobEnqueue',           0x5100a0, 'code', ''),   # (&manager, job): push onto the global queue
    ('JobPop',               0x50ff90, 'code', ''),   # (&manager) -> job or null
    ('JobSignal',            0x50fff0, 'code', ''),   # (&manager, n): wake n workers
    ('JobRelease',           0x50fef0, 'code', ''),   # drop a reference (deletes the job at zero)
    ('JobIsWorker',          0x50ff50, 'code', ''),   # current thread is a job-manager worker
    ('JobWorkerCount',       0x50fd20, 'code', ''),   # number of worker threads
    # PhysX offload (Phase 2)
    ('BodyUpdateShapes',     0x02e510, 'code', ''),   # (body, swept, updateTree): shapes' world AABBs, tree placement
    ('NarrowWake',           0x0b58d0, 'code', ''),   # (physics, body): wake a body something touches, add to solver
    ('LuaIsBodyActive',      0x48d980, 'code', ''),   # Lua IsBodyActive(handle) = body+0x140 != 0
    ('LuaArgInt',            0x642c90, 'code', ''),   # (args, index, default) -> int
    ('LuaPushBool',          0x641740, 'code', ''),   # (ret, bool)
    ('SolverAddBody',        0x60cdf0, 'code', ''),   # TSolver::addBody(solver, pose, com, invMass, invInertia, ...)
    ('SolverIslands',        0x609260, 'code', ''),   # (solver): islands from the step's constraints, their first reader
    ('PlayerConstraints',    0x0c4aa0, 'code', ''),   # (player, solver): grab (held +0x788, anchor +0x7b8) and more
    ('MutexLock',            0x51f220, 'code', ''),   # std::mutex lock (NarrowWake: Physics+0xa2f80)
    ('MutexUnlock',          0x51f3a0, 'code', ''),   # std::mutex unlock (jmp thunk)
    ('ScenePointInWater',    0x0e3b00, 'code', ''),   # (scene, point, float* depth, Water** out) -> in water (Lua IsPointInWater)
    ('ShapeUpdateBounds',    0x4ebca0, 'code', ''),   # (shape, swept, updateTree): world AABB from the body pose
    ('TreeUpdateShape',      0x0b9bd0, 'code', ''),   # (physics, shape): place/refit the shape's AABB-tree node
    ('SoundListMerge',       0x0bd060, 'code', ''),   # (?, jobs): per-job impact sound events -> Physics+0xa3058
    ('VoxPalette',           0x110170, 'code', ''),   # (Voxels*) -> palette entries (40 bytes, material type at +4)
    ('EngineAlloc',          0x5137f0, 'code', ''),   # (size) -> memory (the engine's allocator)
    ('EngineFree',           0x513880, 'code', ''),   # (ptr)
    # entity lifetime (scalar deleting destructors, vftable slot 0)
    ('BodyDtor',             0x0bf190, 'code', ''),   # Body (vftable 0x1409a44c0)
    ('ShapeDtor',            0x4ebb90, 'code', ''),   # Shape (vftable 0x140a46868); frees the Voxels at +0xf0
    # data
    ('FrameDtReadReturn',    0x07366a, 'data', ''),   # return address of the frame-dt timer read in FrameUpdate
    ('GamePtr',              0x1ca7e70, 'data', ''),
    ('JobManagerPtr',        0x1e91aa8, 'data', ''),  # TJobManager* (queue ring +0x40, workers' semaphore +0x100)
    ('NoImpulseDesc',        0xc25070, 'data', ''),
    ('MaterialStrength',     0xc25708, 'data', ''),   # float per material type: contact break strength (x 1200)
    ('ShapeCtor',            0x4eb980, 'code', ''),   # Shape::Shape(this, parent): flags start at 0x30 (no static/dynamic bits)
    ('ShapeVftable',         0xa76418, 'data', ''),   # Shape's vftable: a live shape starts with it (dead: Entity's)
]


def main():
    ap = argparse.ArgumentParser()
    tag_ = GAME_VERSION.replace('.', '_')
    ap.add_argument('--binary', default=os.path.join(REPO, 'research', 're', 'bin', f'teardown_{tag_}_unpacked.exe'))
    a = ap.parse_args()
    pe = pefile.PE(a.binary, fast_load=True)
    img = pe.get_memory_mapped_image()
    stamp = pe.FILE_HEADER.TimeDateStamp
    size = pe.OPTIONAL_HEADER.SizeOfImage
    tag = hashlib.sha256(open(a.binary, 'rb').read()).hexdigest()[:8]

    lines = [
        '// Generated by tools/gen_addresses.py from the decrypted teardown.exe. Do not edit.',
        f'// PE timestamp {stamp:#x}, SizeOfImage {size:#x}, decrypted-copy sha256 prefix {tag}.',
        f'TD_BUILD({stamp:#x}, {size:#x}, "{GAME_VERSION}")',
    ]
    for name, rva, kind, label in ENTRIES:
        if kind == 'code':
            b = img[rva:rva + 16]
            hexbytes = ', '.join(f'0x{x:02x}' for x in b)
            lines.append(f'TD_CODE({name}, {rva:#x}, "{label}", {{{hexbytes}}})')
        else:
            lines.append(f'TD_DATA({name}, {rva:#x})')
    out = os.path.join(REPO, 'src', 'td', f'addresses_{tag_}.inc')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, 'w', newline='\n').write('\n'.join(lines) + '\n')
    print('wrote', out, len(ENTRIES), 'entries')


if __name__ == '__main__':
    main()
