"""Generate src/td/addresses_<build>.inc from the decrypted teardown.exe.

usage: python tools/gen_addresses.py [--binary research/re/bin/teardown_unpacked.exe]

Every code entry records its RVA and the first 16 bytes found there. At run time the plugin hooks a function only
when the live bytes match, so another game build (or code that is still encrypted) disables the hook instead of
corrupting it. Data entries carry no bytes. Names and addresses come from research/docs/re-notes.md.
"""
import argparse, hashlib, os

import pefile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..'))

# (name, rva, kind, timed-event label or '')   kind: 'code' or 'data'
ENTRIES = [
    # frame / tick
    ('FrameDriver',          0x0a7420, 'code', 'frame'),
    ('FrameUpdate',          0x071ce0, 'code', 'frame_update'),
    ('GameTick',             0x07b810, 'code', 'tick'),
    ('SceneUpdate',          0x0f2640, 'code', 'scene_update'),
    ('ScriptsUpdate',        0x4ab880, 'code', 'scripts_update'),
    ('StateSwitch',          0x0792f0, 'code', ''),
    ('ProfilerAdd',          0x45bcf0, 'code', ''),
    # physics
    ('PhysicsUpdate',        0x0b20a0, 'code', 'physics_update'),
    ('PhysicsCollide',       0x0b4850, 'code', 'physics_collide'),
    ('JobBroadPhase',        0x0ad920, 'code', 'job_broadphase'),
    ('JobNearPhaseCollect',  0x0adb40, 'code', 'job_nearphase_collect'),
    ('JobNearPhaseAdd',      0x0adb20, 'code', 'job_nearphase_add'),
    ('JobSolver',            0x5f7ae0, 'code', 'job_solver'),
    ('JobShapeInterp',       0x0d5a50, 'code', 'job_shape_interp'),
    # destruction / split
    ('MakeHoleCore',         0x137d60, 'code', 'makehole_core'),
    ('DestrGather',          0x139360, 'code', 'destr_gather'),
    ('BreakageDriver',       0x13ce30, 'code', 'breakage_driver'),
    ('DestrCarve',           0x139d80, 'code', 'destr_carve'),
    ('DestrFinish',          0x13a200, 'code', 'destr_finish_split'),
    ('DestrMTUpdate',        0x136320, 'code', 'destr_mt_update'),
    ('Destr_b060',           0x13b060, 'code', 'destr_b060'),
    ('Destr_53a0',           0x1353a0, 'code', 'destr_53a0'),
    ('Destr_c100',           0x13c100, 'code', 'destr_c100'),
    ('Destr_de30',           0x13de30, 'code', 'destr_de30'),
    ('BreakIslands',         0x13e470, 'code', 'break_islands'),
    ('BreakIslandsMT',       0x13e710, 'code', 'break_islands_mt'),
    ('JobBreakage',          0x135e60, 'code', 'job_breakage'),
    ('JobCclRelabel',        0x6828b0, 'code', 'job_ccl_relabel'),
    ('VoxBoundsScan',        0x10bfd0, 'code', 'vox_bounds_scan'),
    ('VoxWholeA',            0x105a80, 'code', 'vox_whole_a'),
    ('VoxWholeB',            0x106240, 'code', 'vox_whole_b'),
    ('VoxRegionUpdate',      0x10c400, 'code', 'vox_region_update'),
    ('VoxRegionFinish',      0x10c680, 'code', 'vox_region_finish'),
    ('VoxRegionCcl',         0x686210, 'code', 'vox_region_ccl'),
    ('VoxFullRelabel',       0x6860a0, 'code', 'vox_full_relabel'),
    # inside DestrFinish (0x13a200): islands -> new bodies
    ('SplitBodyCheck',       0x02f030, 'code', 'split_body_check'),
    ('SplitPerBody',         0x1385b0, 'code', 'split_per_body'),
    # inside SplitPerBody: shape-level connectivity of multi-shape bodies (flood fill over touching shapes,
    # re-run on a growing region while more than one boundary-touching group remains)
    ('SplitCollect',         0x1336b0, 'code', 'split_collect'),
    ('SplitFlood',           0x138f20, 'code', 'split_flood'),
    ('SplitGroupBodies',     0x138b60, 'code', 'split_group_bodies'),
    ('ShapeTouchTest',       0x0b1100, 'code', 'shape_touch_test'),
    ('SceneQueryShapes',     0x0d4ce0, 'code', 'scene_query_shapes'),
    ('SplitIslands',         0x0ba3c0, 'code', 'split_islands'),
    ('SplitBodyOp',          0x030f70, 'code', 'split_body_op'),
    ('SplitJobs',            0x130880, 'code', 'split_jobs'),
    ('SplitNewBodyUpdate',   0x02e180, 'code', 'split_newbody_update'),
    ('SplitWait',            0x13f3a0, 'code', 'split_wait'),
    ('SplitBreakageJobs',    0x133770, 'code', 'split_breakage_jobs'),
    ('SplitEntryFinish',     0x13d980, 'code', 'split_entry_finish'),
    ('PhysicsBodyUpdate',    0x0b5ba0, 'code', 'physics_body_update'),
    ('PhysicsBodiesUpdate',  0x0b5a80, 'code', 'physics_bodies_update'),
    ('SceneShapesChanged',   0x0deaa0, 'code', 'scene_shapes_changed'),
    ('JobCollisionData',     0x0d5670, 'code', 'job_collision_data'),
    ('JobPaintSlices',       0x0d5740, 'code', 'job_paint_slices'),
    ('LuaPaint',             0x484d50, 'code', 'lua_paint'),
    # other subsystems
    ('ParticlesUpdate',      0x4304b0, 'code', 'particles_update'),
    ('FireUpdate',           0x15c280, 'code', 'fire_update'),
    # functions the plugin calls
    ('MakeHole',             0x138260, 'code', ''),
    ('PaintScene',           0x0deed0, 'code', 'paint_scene'),
    ('BodyWake',             0x031330, 'code', ''),
    ('ShapeWorldTransform',  0x4de540, 'code', ''),
    ('ShapeInterpTransform', 0x4de6c0, 'code', ''),
    ('StartLevel',           0x079b80, 'code', ''),
    ('RequestState',         0x078420, 'code', ''),
    ('PlayerSetTransform',   0x0c1fc0, 'code', ''),   # (player, const Transform* feet, bool withPitch)
    ('ModById',              0x3f5280, 'code', ''),
    ('LevelDescCtor',        0x06bf80, 'code', ''),
    ('PathResolveLevel',     0x4635f0, 'code', ''),
    ('TdStringFromCStr',     0x5075a0, 'code', ''),
    ('TdStringCopy',         0x507760, 'code', ''),
    ('TdStringDtor',         0x507710, 'code', ''),
    # input record/replay
    ('WndProc',              0x4f7f60, 'code', ''),
    ('MessagePump',          0x4fad30, 'code', ''),
    ('TimerElapsed',         0x512370, 'code', ''),
    ('WindowIsActive',       0x4f9450, 'code', ''),
    ('GameLog',              0x6997c0, 'code', 'game_log'),
    ('SeedFromTime',         0x505400, 'code', ''),   # srand((int)time(NULL)): Game ctor and a generator at 0x6a61c0
    ('JobRun',               0x501b20, 'code', ''),   # job manager: job->Run(), then marks the job done
    ('JobWait',              0x5025e0, 'code', ''),   # wait for a job (non-workers wake the extra worker, then block)
    ('JobEnqueue',           0x501ee0, 'code', ''),   # (&manager, job): push onto the global queue
    ('JobPop',               0x501dd0, 'code', ''),   # (&manager) -> job or null
    ('JobSignal',            0x501e30, 'code', ''),   # (&manager, n): wake n workers
    ('JobRelease',           0x501d30, 'code', ''),   # drop a reference (deletes the job at zero)
    ('JobIsWorker',          0x501d90, 'code', ''),   # current thread is a job-manager worker
    ('JobWorkerCount',       0x501c10, 'code', ''),   # number of worker threads
    # PhysX offload (Phase 2)
    ('BodyUpdateShapes',     0x02dbc0, 'code', ''),   # (body, swept, updateTree): shapes' world AABBs, tree placement
    ('NarrowWake',           0x0adc50, 'code', ''),   # (physics, body): wake a body something touches, add to solver
    ('LuaIsBodyActive',      0x47ffc0, 'code', ''),   # Lua IsBodyActive(handle) = body+0x140 != 0
    ('LuaArgInt',            0x631b80, 'code', ''),   # (args, index, default) -> int
    ('LuaPushBool',          0x630630, 'code', ''),   # (ret, bool)
    ('SolverAddBody',        0x5fbbf0, 'code', ''),   # TSolver::addBody(solver, pose, com, invMass, invInertia, ...)
    ('SolverIslands',        0x5f80a0, 'code', ''),   # (solver): islands from the step's constraints, their first reader
    ('PlayerConstraints',    0x0bcd50, 'code', ''),   # (player, solver): grab (held +0x788, anchor +0x7b8) and more
    ('MutexLock',            0x511010, 'code', ''),   # std::mutex lock (NarrowWake: Physics+0xa2f80)
    ('MutexUnlock',          0x511190, 'code', ''),   # std::mutex unlock (jmp thunk)
    ('ScenePointInWater',    0x0dbb80, 'code', ''),   # (scene, point, float* depth, Water** out) -> in water (Lua IsPointInWater)
    ('ShapeUpdateBounds',    0x4ddd00, 'code', ''),   # (shape, swept, updateTree): world AABB from the body pose
    ('TreeUpdateShape',      0x0b1f20, 'code', ''),   # (physics, shape): place/refit the shape's AABB-tree node
    ('SoundListMerge',       0x0b5390, 'code', ''),   # (?, jobs): per-job impact sound events -> Physics+0xa3058
    ('VoxPalette',           0x108380, 'code', ''),   # (Voxels*) -> palette entries (40 bytes, material type at +4)
    ('EngineAlloc',          0x505680, 'code', ''),   # (size) -> memory (the engine's allocator)
    ('EngineFree',           0x505710, 'code', ''),   # (ptr)
    # entity lifetime (scalar deleting destructors, vftable slot 0)
    ('BodyDtor',             0x0b74c0, 'code', ''),   # Body (vftable 0x1409a44c0)
    ('ShapeDtor',            0x4ddbf0, 'code', ''),   # Shape (vftable 0x140a46868); frees the Voxels at +0xf0
    # data
    ('FrameDtReadReturn',    0x0727da, 'data', ''),   # return address of the frame-dt timer read in FrameUpdate
    ('GamePtr',              0x1c6e270, 'data', ''),
    ('JobManagerPtr',        0x1e57488, 'data', ''),  # TJobManager* (queue ring +0x40, workers' semaphore +0x100)
    ('NoImpulseDesc',        0xbef070, 'data', ''),
    ('MaterialStrength',     0xbef708, 'data', ''),   # float per material type: contact break strength (x 1200)
    ('ShapeCtor',            0x4dd9e0, 'code', ''),   # Shape::Shape(this, parent): flags start at 0x30 (no static/dynamic bits)
    ('ShapeVftable',         0xa46868, 'data', ''),   # Shape's vftable: a live shape starts with it (dead: Entity's)
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--binary', default=os.path.join(REPO, 'research', 're', 'bin', 'teardown_unpacked.exe'))
    a = ap.parse_args()
    pe = pefile.PE(a.binary, fast_load=True)
    img = pe.get_memory_mapped_image()
    stamp = pe.FILE_HEADER.TimeDateStamp
    size = pe.OPTIONAL_HEADER.SizeOfImage
    tag = hashlib.sha256(open(a.binary, 'rb').read()).hexdigest()[:8]

    lines = [
        '// Generated by tools/gen_addresses.py from the decrypted teardown.exe. Do not edit.',
        f'// PE timestamp {stamp:#x}, SizeOfImage {size:#x}, decrypted-copy sha256 prefix {tag}.',
        f'TD_BUILD({stamp:#x}, {size:#x}, "2.1.0")',
    ]
    for name, rva, kind, label in ENTRIES:
        if kind == 'code':
            b = img[rva:rva + 16]
            hexbytes = ', '.join(f'0x{x:02x}' for x in b)
            lines.append(f'TD_CODE({name}, {rva:#x}, "{label}", {{{hexbytes}}})')
        else:
            lines.append(f'TD_DATA({name}, {rva:#x})')
    out = os.path.join(REPO, 'src', 'td', 'addresses_2_1_0.inc')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, 'w', newline='\n').write('\n'.join(lines) + '\n')
    print('wrote', out, len(ENTRIES), 'entries')


if __name__ == '__main__':
    main()
