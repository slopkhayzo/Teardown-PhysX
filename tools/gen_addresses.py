"""Generate src/td/addresses_<build>.inc from the decrypted teardown.exe.

usage: python tools/gen_addresses.py [--binary research/re/bin/teardown_<GAME_VERSION>_unpacked.exe]

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
GAME_VERSION = '2.2.0.20261118'  # the 2.2.0 hotfix of 2026-11-18: Steam build 26758720, teardown.exe sha256
#                                  b9aacc4e..., PE timestamp 0x6afda5ed (2.2.0 itself: build 26320217, 0x6ae468bd)

# (name, rva, kind, timed-event label or '')   kind: 'code' or 'data'
ENTRIES = [
    # frame / tick
    ('FrameDriver',          0x0aeac0, 'code', 'frame'),
    ('FrameUpdate',          0x072560, 'code', 'frame_update'),
    ('GameTick',             0x07caf0, 'code', 'tick'),
    ('SceneUpdate',          0x0f9bc0, 'code', 'scene_update'),
    ('ScriptsUpdate',        0x4b7b50, 'code', 'scripts_update'),
    ('StateSwitch',          0x07a3d0, 'code', ''),
    ('ProfilerAdd',          0x467d80, 'code', ''),
    # physics
    ('PhysicsUpdate',        0x0b9760, 'code', 'physics_update'),
    ('PhysicsCollide',       0x0bbf20, 'code', 'physics_collide'),
    ('JobBroadPhase',        0x0b4fc0, 'code', 'job_broadphase'),
    ('JobNearPhaseCollect',  0x0b51e0, 'code', 'job_nearphase_collect'),
    ('JobNearPhaseAdd',      0x0b51c0, 'code', 'job_nearphase_add'),
    ('JobSolver',            0x606db0, 'code', 'job_solver'),
    ('JobShapeInterp',       0x0dd1f0, 'code', 'job_shape_interp'),
    # destruction / split
    ('MakeHoleCore',         0x13f6c0, 'code', 'makehole_core'),
    ('DestrGather',          0x140cc0, 'code', 'destr_gather'),
    ('BreakageDriver',       0x1447b0, 'code', 'breakage_driver'),
    ('DestrCarve',           0x1416e0, 'code', 'destr_carve'),
    ('DestrFinish',          0x141b60, 'code', 'destr_finish_split'),
    ('DestrMTUpdate',        0x13dc80, 'code', 'destr_mt_update'),
    ('Destr_b060',           0x1429c0, 'code', 'destr_b060'),
    ('Destr_53a0',           0x13cd00, 'code', 'destr_53a0'),
    ('Destr_c100',           0x143a60, 'code', 'destr_c100'),
    ('Destr_de30',           0x1457b0, 'code', 'destr_de30'),
    ('BreakIslands',         0x145df0, 'code', 'break_islands'),
    ('BreakIslandsMT',       0x146090, 'code', 'break_islands_mt'),
    ('JobBreakage',          0x13d7c0, 'code', 'job_breakage'),
    ('JobCclRelabel',        0x691ab0, 'code', 'job_ccl_relabel'),
    ('VoxBoundsScan',        0x113550, 'code', 'vox_bounds_scan'),
    ('VoxWholeA',            0x10d000, 'code', 'vox_whole_a'),
    ('VoxWholeB',            0x10d7c0, 'code', 'vox_whole_b'),
    ('VoxRegionUpdate',      0x113980, 'code', 'vox_region_update'),
    ('VoxRegionFinish',      0x113c00, 'code', 'vox_region_finish'),
    ('VoxRegionCcl',         0x6953f0, 'code', 'vox_region_ccl'),
    ('VoxFullRelabel',       0x695280, 'code', 'vox_full_relabel'),
    # inside DestrFinish (0x13a200): islands -> new bodies
    ('SplitBodyCheck',       0x02f980, 'code', 'split_body_check'),
    ('SplitPerBody',         0x13ff10, 'code', 'split_per_body'),
    # inside SplitPerBody: shape-level connectivity of multi-shape bodies (flood fill over touching shapes,
    # re-run on a growing region while more than one boundary-touching group remains)
    ('SplitCollect',         0x13b010, 'code', 'split_collect'),
    ('SplitFlood',           0x140880, 'code', 'split_flood'),
    ('SplitGroupBodies',     0x1404c0, 'code', 'split_group_bodies'),
    ('ShapeTouchTest',       0x0b87c0, 'code', 'shape_touch_test'),
    ('SceneQueryShapes',     0x0dc480, 'code', 'scene_query_shapes'),
    ('SplitIslands',         0x0c1ab0, 'code', 'split_islands'),
    ('SplitBodyOp',          0x0318c0, 'code', 'split_body_op'),
    ('SplitJobs',            0x1381d0, 'code', 'split_jobs'),
    ('SplitNewBodyUpdate',   0x02ead0, 'code', 'split_newbody_update'),
    ('SplitWait',            0x146d20, 'code', 'split_wait'),
    ('SplitBreakageJobs',    0x13b0d0, 'code', 'split_breakage_jobs'),
    ('SplitEntryFinish',     0x145300, 'code', 'split_entry_finish'),
    ('PhysicsBodyUpdate',    0x0bd290, 'code', 'physics_body_update'),
    ('PhysicsBodiesUpdate',  0x0bd170, 'code', 'physics_bodies_update'),
    ('SceneShapesChanged',   0x0e62d0, 'code', 'scene_shapes_changed'),
    ('JobCollisionData',     0x0dce10, 'code', 'job_collision_data'),
    ('JobPaintSlices',       0x0dcee0, 'code', 'job_paint_slices'),
    ('LuaPaint',             0x490d00, 'code', 'lua_paint'),
    # other subsystems
    ('ParticlesUpdate',      0x43c540, 'code', 'particles_update'),
    ('FireUpdate',           0x167ac0, 'code', 'fire_update'),
    # functions the plugin calls
    ('MakeHole',             0x13fbc0, 'code', ''),
    ('PaintScene',           0x0e6700, 'code', 'paint_scene'),
    ('BodyWake',             0x031c80, 'code', ''),
    ('ShapeWorldTransform',  0x4ea970, 'code', ''),
    ('ShapeInterpTransform', 0x4eaaf0, 'code', ''),
    ('StartLevel',           0x07ac60, 'code', ''),
    ('RequestState',         0x079470, 'code', ''),
    ('PlayerSetTransform',   0x0c96d0, 'code', ''),   # (player, const Transform* feet, bool withPitch)
    ('ModById',              0x400b00, 'code', ''),
    ('LevelDescCtor',        0x06cba0, 'code', ''),
    ('PathResolveLevel',     0x46f510, 'code', ''),
    ('TdStringFromCStr',     0x513d80, 'code', ''),
    ('TdStringCopy',         0x513f40, 'code', ''),
    ('TdStringDtor',         0x513ef0, 'code', ''),
    # input record/replay
    ('WndProc',              0x5042c0, 'code', ''),
    ('MessagePump',          0x507090, 'code', ''),
    ('TimerElapsed',         0x51eb50, 'code', ''),
    ('WindowIsActive',       0x5057b0, 'code', ''),
    ('GameLog',              0x6a89a0, 'code', 'game_log'),
    ('SeedFromTime',         0x511be0, 'code', ''),   # srand((int)time(NULL)): Game ctor and a generator at 0x6a61c0
    ('JobRun',               0x50e100, 'code', ''),   # job manager: job->Run(), then marks the job done
    ('JobWait',              0x50ecf0, 'code', ''),   # wait for a job (non-workers wake the extra worker, then block)
    ('JobEnqueue',           0x50e6b0, 'code', ''),   # (&manager, job): push onto the global queue
    ('JobPop',               0x50e5a0, 'code', ''),   # (&manager) -> job or null
    ('JobSignal',            0x50e600, 'code', ''),   # (&manager, n): wake n workers
    ('JobRelease',           0x50e500, 'code', ''),   # drop a reference (deletes the job at zero)
    ('JobIsWorker',          0x50e560, 'code', ''),   # current thread is a job-manager worker
    ('JobWorkerCount',       0x50e330, 'code', ''),   # number of worker threads
    # PhysX offload (Phase 2)
    ('BodyUpdateShapes',     0x02e510, 'code', ''),   # (body, swept, updateTree): shapes' world AABBs, tree placement
    ('NarrowWake',           0x0b52f0, 'code', ''),   # (physics, body): wake a body something touches, add to solver
    ('LuaIsBodyActive',      0x48bf20, 'code', ''),   # Lua IsBodyActive(handle) = body+0x140 != 0
    ('LuaMakeHole',          0x48ef50, 'code', ''),   # Lua MakeHole(pos, r0, r1, r2, silent): (script, args, ret)
    ('LuaArgInt',            0x640d80, 'code', ''),   # (args, index, default) -> int
    ('LuaPushBool',          0x63f830, 'code', ''),   # (ret, bool)
    ('SolverAddBody',        0x60aec0, 'code', ''),   # TSolver::addBody(solver, pose, com, invMass, invInertia, ...)
    ('SolverIslands',        0x607370, 'code', ''),   # (solver): islands from the step's constraints, their first reader
    ('PlayerConstraints',    0x0c4440, 'code', ''),   # (player, solver): grab (held +0x788, anchor +0x7b8) and more
    ('MutexLock',            0x51d7f0, 'code', ''),   # std::mutex lock (NarrowWake: Physics+0xa2f80)
    ('MutexUnlock',          0x51d970, 'code', ''),   # std::mutex unlock (jmp thunk)
    ('ScenePointInWater',    0x0e32f0, 'code', ''),   # (scene, point, float* depth, Water** out) -> in water (Lua IsPointInWater)
    ('ShapeUpdateBounds',    0x4ea130, 'code', ''),   # (shape, swept, updateTree): world AABB from the body pose
    ('TreeUpdateShape',      0x0b95e0, 'code', ''),   # (physics, shape): place/refit the shape's AABB-tree node
    ('SoundListMerge',       0x0bca70, 'code', ''),   # (?, jobs): per-job impact sound events -> Physics+0xa3058
    ('VoxPalette',           0x10f900, 'code', ''),   # (Voxels*) -> palette entries (40 bytes, material type at +4)
    ('EngineAlloc',          0x511e60, 'code', ''),   # (size) -> memory (the engine's allocator)
    ('EngineFree',           0x511ef0, 'code', ''),   # (ptr)
    # entity lifetime (scalar deleting destructors, vftable slot 0)
    ('BodyDtor',             0x0bebb0, 'code', ''),   # Body (vftable 0x1409a44c0)
    ('ShapeDtor',            0x4ea020, 'code', ''),   # Shape (vftable 0x140a46868); frees the Voxels at +0xf0
    # data
    ('FrameDtReadReturn',    0x07311a, 'data', ''),   # return address of the frame-dt timer read in FrameUpdate
    ('GamePtr',              0x1c96e70, 'data', ''),
    ('JobManagerPtr',        0x1e80aa8, 'data', ''),  # TJobManager* (queue ring +0x40, workers' semaphore +0x100)
    ('NoImpulseDesc',        0xc14070, 'data', ''),
    ('MaterialStrength',     0xc14708, 'data', ''),   # float per material type: contact break strength (x 1200)
    ('ShapeCtor',            0x4e9e10, 'code', ''),   # Shape::Shape(this, parent): flags start at 0x30 (no static/dynamic bits)
    ('ShapeVftable',         0xa65408, 'data', ''),   # Shape's vftable: a live shape starts with it (dead: Entity's)
    # broadphase AABB trees (physics+8.. +0x30: two static, two per dynamic state; leaves hold the shapes' exact boxes)
    ('TreeInsert',           0x52f5d0, 'code', ''),   # (treeRef, &min, &max, shape) -> node
    ('TreeUpdateLeaf',       0x530450, 'code', ''),   # (treeRef, node, &min, &max)
    ('TreeRemove',           0x52f6a0, 'code', ''),   # (treeRef, node)
    ('TreeClear',            0x52f510, 'code', ''),   # (treeRef): frees every node, a new empty tree object
    ('ListReserve',          0x0e05e0, 'code', ''),   # (list {count, cap, data, inline}, capacity): engine-allocated
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
