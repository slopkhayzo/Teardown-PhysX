// TeardownPhysX.ini: next to the plugin, or the file named by the TDPX_INI environment variable.
#pragma once
#include <string>

struct Config {
    std::string iniPath;
    std::string moduleDir;

    // [Trace]
    bool traceEnabled = true;
    std::string outDir;    // a timestamped run folder is created inside (default <module dir>\TeardownPhysX_runs)
    std::string runDir;    // when set, used as the run folder exactly (queue jobs set this)
    std::string label = "manual";
    bool samplerEnabled = false;  // sample the main thread's call stacks into <run>\samples.bin (src/trace/sampler.h)
    int samplerHz = 1000;
    int samplerDepth = 48;
    bool samplerPhysx = false;  // also PhysX's threads, while they run (tools/sample_report.py --threads physx)
    bool gpuKernels = false;    // GPU time per CUDA kernel in plugin.log, via the CUDA toolkit's CUPTI (src/trace/gpu_kernels.h)
    bool threadsEnabled = true;  // CPU per thread group, once a second, into <run>	hreads.csv (src/trace/threads.h)

    // [Mods]
    std::string modOverrides;  // a folder of mod file overrides (src/fix/overrides.h); empty = off

    // [Sandbox]
    std::string appDataRoot;  // when set, the game's LocalAppData (saves, options) is redirected here

    // [Session] what the plugin does with the game: none (trace only), bench (synthetic laser), record, replay,
    // browse (free play from the main menu; quitting terminates the game instead of its normal shutdown).
    // bench/record/replay skip the splash and start [Level] by themselves.
    std::string mode = "none";

    // [Level] (older inis: the same keys under [Bench])
    std::string mod;  // workshop/local mod id (e.g. steam-2658944037): Level is then a file inside that mod
    double modWaitSeconds = 40.0;  // how long to wait in the menu for the mod to be listed
    std::string mission = "lee_sandbox";
    std::string levelFile = "lee.xml";
    std::string layers = "sandbox";

    // [Record] input recording, from the first PLAY frame until the game leaves PLAY
    std::string recordFile;
    bool recordQuitAfter = true;  // close the game once the recording is saved
    unsigned recordSeed = 1;      // CRT rand() seed for the session, stored in the recording
    int recordStopAfterFrames = 0;  // tests: end the recording by itself after this many frames (0 = never)

    // [Replay]
    std::string replayFile;
    bool replayStopAtPause = true;  // end where the recording opened the pause menu
    int replayMaxFrames = 0;        // [Replay] Frames: stop after this many frames (0: the whole recording)
    std::string replayReRecordFile; // also record the replay itself (tests the recorder; replay that file next)
    bool replayPinPlayer = true;    // keep the player on the recorded path (debris can push it elsewhere otherwise)
    std::string dumpBodies;  // [Session] DumpBodies = "0-2,208-222": every body at those frames to bodies.csv
    bool serialJobs = false; // [Session] SerialJobs: all engine jobs on one worker, in order (src/fix/serial_jobs.h)
    int hangKillSeconds = 30; // [Session] HangKillSeconds: terminate the game when a level frame takes this long (0 = never)

    // [Bench] synthetic laser: carve holes into what the camera sees, quit
    bool benchEnabled = false;
    double splashSkipSeconds = 2.0;  // the splash waits for a key press; request the menu like Lua Menu() does
    double startDelaySeconds = 3.0;  // time in the main menu before starting the level
    double warmupSeconds = 6.0;
    double durationSeconds = 45.0;
    double holesPerSecond = 20.0;
    float r0 = 2.0f, r1 = 1.9f, r2 = 1.8f;
    bool sound = true;
    double sweepYawDeg = 35.0;
    double sweepPitchDeg = 12.0;
    double sweepPeriodSeconds = 6.0;
    double aimYawDeg = 0.0, aimPitchDeg = 0.0;  // the sweep's centre, from the spawn view (right / up positive)
    double turnYawDeg = 0.0, turnPitchDeg = 0.0;  // turn the player's view once in the warm-up (right / up positive)
    // View = "x y z dx dy dz": instead, move the player so the camera is at x y z looking along dx dy dz (the numbers
    // of a "tool: firing from camera (...) along (...)" log line)
    bool viewSet = false;
    float view[6] = {};
    double maxRayDistance = 80.0;
    // Gluon Plasma Diffuser side effects: for EffectSeconds after each hit, every dynamic body whose bounds centre
    // is within EffectRadius of the last hit is painted ("explosion", random radius PaintMin..PaintMax) each tick;
    // during the first 0.05 s it is also pushed away (PushLight / PushHeavy m/s, spun if lighter than LightMass).
    bool laserEffects = true;
    double effectRadius = 4.0;
    double effectSeconds = 1.0;
    double pushLight = 10.0, pushHeavy = 1.0, spinLight = 5.0, lightMass = 500.0;
    double paintMin = 0.25, paintMax = 1.0;
    unsigned seed = 12345;
    bool quitWhenDone = true;
    double loadTimeoutSeconds = 120.0;

    // [Tool] the debris tool (src/bench/bench.h): while Key is held it carves where the camera aims, with the
    // engine's own MakeHoleCore, no particles or paint. Mode=spray: each tick HolesPerTick holes at random points of
    // a ball Spread metres into the surface (radii R0/R1/R2 for soft/medium/hard materials). Key: mmb, xbutton1,
    // xbutton2, a letter or a digit.
    bool toolEnabled = false;
    std::string toolKey = "mmb";
    int toolHolesPerTick = 4;
    float toolSpread = 1.2f;
    float toolR0 = 0.45f, toolR1 = 0.4f, toolR2 = 0.35f;
    bool toolSound = true;
    float toolRange = 120.0f;
    // bench mode: fire without the key for AutoFireSeconds from the run's start, aim sweeping like the synthetic
    // laser; the rest of the run the pieces pile up and settle (the pile benchmark)
    bool toolAutoFire = false;
    double toolAutoFireSeconds = 30.0;
    double toolAimPitchDeg = 0.0;  // auto-fire: aim this far below (negative) or above the camera's direction
    double toolAimYawDeg = 0.0;    // auto-fire: ... and this far right (positive) or left of it
    // Mode=fragment: every FragmentTicks ticks of firing, cut a cube of FragmentSize metres just behind the surface
    // the aim hits into PieceSize cubes: planes of overlapping small holes, so each cube comes loose as one piece
    std::string toolMode = "fragment";  // fragment | spray
    float toolFragmentSize = 3.0f, toolPieceSize = 0.75f;
    int toolFragmentTicks = 1;  // a cut every tick (~17 ms) while the key is held
    // fragment cuts are carved in batches of CarveBatch x CarveBatch neighbouring holes, CarveMsPerTick per frame
    // (one engine call with thousands of holes spread over metres stalls the game for seconds). A 3 m cut costs
    // ~8-11 ms, mostly the engine making bodies of the pieces: 12 ms keeps up with a cut every tick. 16x16 batches
    // carve ~22% cheaper per hole than 8x8.
    int toolCarveBatch = 16;
    float toolCarveMsPerTick = 12.0f;

    // [PhysX] src/px/shadow.h. Shadow=1|2 (Phase 1) only watch; Shadow=3 (Phase 2) hands free debris to PhysX.
    int pxShadow = 0;            // 0 off, 1 sync (each tick from the native state; compare), 2 free-running copy,
                                 // 3 offload (PhysX simulates free debris, the game draws and uses the result)
    int pxThreads = 0;           // PhysX CPU dispatcher threads (0: logical processors - 4, at least 2)
    int pxMaxBodyVoxels = 20000; // dynamic bodies up to this many solid voxels get a PhysX actor (larger: proxy)
    int pxBoxBudget = 128;       // boxes per shape before the grid is coarsened
    bool pxHull = true;          // [PhysX] Collider=hull (default) | boxes: one convex hull per debris shape
    int pxHullVertices = 32;
    int pxSplitShapes = 16;      // proxies with more shapes than this get a PhysX actor per shape
    // a hull this many times a piece's own volume (an irregular piece: a staircase of voxels, an L) overlaps its
    // neighbours by centimetres and keeps the pile vibrating: such a piece is split into up to HullParts hulls that
    // each fit, or gets its exact boxes if that is not enough (0: always one hull). Was 1.5 with boxes at once: on
    // Acacia's pile ~6,000 pieces then had ~9 boxes each; 2 and 8 parts: a third fewer colliders and contact pairs,
    // PhysX step 11.6 -> 7.1 ms late in acacia1, no more rattling.
    float pxHullMaxBloat = 2.0f;
    int pxHullParts = 8;  // (1: its exact boxes at once)
    int pxBlockBoxes = 8192;     // boxes per 32^3 static block
    int pxStaticAggregates = -1; // each static block one broadphase entry (aggregate); -1: unless the broadphase
                                 // runs on the GPU (PhysX 5.12's GPU aggregates break when blocks are rebuilt)
    float pxMaxDepenetration = 3.0f;  // m/s: how fast PhysX may push overlapping bodies apart
    double pxBuildMsPerTick = 8.0;  // static block building per tick; the rest waits for the next tick
    double pxBodyBuildMsPerTick = 6.0;  // debris colliders per tick (new asleep debris waits)
    int pxBlockSize = 32;        // static blocks, voxels per side
    float pxGravity = -10.0f;
    float pxFriction = 0.6f, pxRestitution = 0.1f;
    bool pxTgs = false;
    int pxLogEvery = 60;         // ticks between shadow.csv rows / log lines
    int pxDumpFrom = -1, pxDumpTo = -1;  // [PhysX] DumpTicks=a-b: every compared body to shadow_bodies.csv
    // offload (Shadow=3)
    bool pxPlayer = true;              // a kinematic capsule follows the player and pushes debris
    float pxPlayerRadius = 0.35f, pxPlayerHeight = 1.8f;
    // The piece of debris the player stands on stays PhysX's (the game sees a static obstacle, as for other bodies
    // touching debris) and gets the player's weight there while it moves; 0: the game takes it while stood on (its
    // stand-in in PhysX then shoves the debris under it aside: the player sinks into a pile).
    bool pxPlayerGround = true;
    float pxPlayerWeight = 80.0f;      // kg pressing on that piece (0: none)
    int pxRegainTicks = 30;            // a body handed back to the game stays native at least this long
    bool pxCcd = false;                // speculative CCD on owned debris (made piles of fresh debris explode)
    float pxMaxSpeed = 150.0f;         // watchdog: owned debris faster than this (m/s) goes back to the game
    float pxProxyMaxSpeed = 20.0f;     // proxies moving faster (m/s, fastest point) teleport instead of sweeping
    // a proxy pushes debris at most as hard as its own mass moving at this speed (m/s; 0: no limit)
    float pxProxyImpulseSpeed = 5.0f;
    bool pxAsync = true;               // PhysX steps during the rest of the frame (results one tick later)
    bool pxCatchUp = true;             // async: a tick whose PhysX step isn't done yet passes without PhysX, and
                                       // the next step covers both (debris at 30 Hz when the game runs 2 ticks/frame)
    int pxCatchUpMax = 2;              // ticks one PhysX step may cover
    float pxCatchUpWaitMs = 0.5f;      // wait this long for a step that is almost done before skipping
    // solver iterations per owned debris actor (PhysX's default is 4/1). With 4, the pieces deep in a pile of
    // thousands sink centimetres into each other under the load and vibrate for ever once woken; 8 lets it settle
    // and sleep (~11% more PhysX step time while the pile is moving; 12 costs ~28%).
    int pxPositionIters = 8, pxVelocityIters = 1;
    // mass-normalised kinetic energy below which debris may sleep (0: PhysX's, ~0.005). With PhysX's, a resting pile
    // of ~4,000 pieces never sleeps (a few slow sliders keep its island awake); with 0.05 (~0.3 m/s) it all sleeps.
    float pxSleepThreshold = 0.05f;
    // m/s: a piece whose centre got no further than this over the last ~5 s counts as quiet for the settle check,
    // however fast it shakes: a big pile presses its pieces centimetres into each other, and PhysX pushing them apart
    // keeps hundreds creeping at 1-5 cm/s for ever (Volkograd). 0: only the one-second rules.
    float pxSettleDrift = 0.05f;
    // Settled debris is frozen (kinematic in PhysX: in no island, no step cost) after this many settle checks (one
    // a second) in a row in which it got nowhere or never went faster than FreezeSpeed (m/s: a big pile creeps at
    // 5-20 cm/s for ever); one more check for each time in a row it thawed within a minute of freezing. It thaws
    // when something reaches it: a piece faster than ThawSpeed (m/s) that travelled ThawDistance (metres) since it
    // last thawed its surroundings (jiggling or creeping doesn't count), a proxy the game moved, the player; when
    // what holds it up changes, or when the game moves, pushes or wakes it. Each switch costs PhysX a broadphase
    // reinsert of the piece's colliders (~0.1 ms on the main thread), so at most FreezePerTick freeze per tick.
    // FreezeAfter 0: off.
    int pxFreezeAfter = 0;
    float pxFreezeSpeed = 0.3f;
    // Settle check (once a second): awake pieces close together that have been quiet for two checks (got nowhere, or
    // stayed slower than FreezeSpeed) sleep together, a whole quiet pile at once, even with up to 1 in 100 still
    // moving slower than 1.5 m/s (wedged pieces rattling in place: they stop where they are). 0: piece by piece only.
    bool pxSettleClusters = true;
    float pxThawSpeed = 0.3f;
    float pxThawDistance = 0.03f;
    int pxFreezePerTick = 64;
    // PhysX drops kinematic-kinematic and kinematic-static pairs in the broadphase (PxPairFilteringMode::eKILL)
    // instead of keeping them without contacts: frozen pieces, proxies and statics have no pairs among themselves.
    bool pxKillKinematicPairs = true;
    // PhysX follows the game's collision filters (layer, mask and group, SetShapeCollisionFilter) except for one: the
    // game lets small debris (each shape at most 7^3 voxels and 50 solid ones) hit only static geometry, so small
    // pieces fall through each other and through moving bodies. 1 (default) keeps them colliding with everything,
    // which is what makes piles; 0 does as the game does (piles of small pieces flatten, and PhysX has fewer pairs).
    bool pxSmallDebrisCollide = true;
    bool pxStabilization = false;      // PxSceneFlag::eENABLE_STABILIZATION: damps a creeping pile, but its frozen
                                       // bodies never sleep (3,400 stay active where the pile otherwise sleeps)
    // A resting pile keeps creeping when native bodies around it jiggle: their proxies push it, and its creep makes
    // the game's bodies touching it move again. Moves below these (metres, at the body's fastest point) are left out:
    float pxProxyDeadband = 0.005f;    // a native body's move smaller than this doesn't move its proxy
    float pxWriteDeadband = 0.001f;    // a PhysX body's move smaller than this isn't written to the game (it rests)
    bool pxDiagOverlaps = false;       // the creep line measures the fastest pieces' overlaps (slow: diagnostics)
    bool pxDebugNoKinematicContacts = false;  // diagnostics: proxies and the player's capsule touch no debris
    // diagnostics: after a settle check, PhysX's reason for keeping an island of at least this many bodies awake
    // (our PhysX build records it in its island sim; 0: off)
    int pxDebugIslands = 0;
    int pxDebugSettle = 0;  // N > 0: every 5 s, how the largest awake group's movers move, and the N that get furthest
    float pxLinearDamping = 0.0f;      // per owned debris actor (0: free flight like the game's)
    float pxAngularDamping = 0.0f;     // (PhysX's own default is 0.05)
    bool pxImpacts = true;             // offload: PhysX impacts become the game's impact sounds
    float pxImpactSpeed = 1.0f;        // m/s: weaker contacts are not reported
    bool pxDamage = true;              // offload: PhysX impacts break voxels the way the game's contacts do
    int pxDamageMax = 64;              // at most this many holes per tick from PhysX impacts
    // GPU rigid bodies (CUDA; an NVIDIA GPU and PhysXGpu_64.dll next to the plugin, else the CPU). The buffer sizes
    // are PhysX's GPU pre-allocations; plugin.log names the one to raise when a step needs more.
    bool pxGpu = false;
    bool pxGpuBroadphase = true;       // the broadphase on the GPU too
    int pxGpuContacts = 1 << 20, pxGpuPatches = 1 << 19;  // (Acacia piles need ~260k patches)
    int pxGpuPairs = 1 << 20, pxGpuAggregatePairs = 1 << 20;
    int pxGpuPartitions = 8;
    bool pxGpuSync = false;            // debugging: every kernel launch waits for the kernel (names a faulting one)

    // [Fix] track B patches
    bool touchCache = true;         // memoise the shape-touch test (src/fix/touch_cache.h): 0 mismatches in 56k checks
    int touchCacheValidate = 16;    // re-check every Nth hit against the engine (0 = never)
    bool touchCacheSymmetric = true;
    // a split doesn't refresh the static body it cut from (src/fix/static_body_update.h); every Nth skip is checked
    bool staticBodyUpdate = true;
    int staticBodyUpdateValidate = 64;
    bool visibleEarlyOut = true;    // IsBodyVisible stops at the first visible point (src/fix/visible_early_out.h)
    bool keepDebris = false;        // the rubble cleanup deletes no small pieces (src/fix/keep_debris.h)
    int splitJobsMin = 16;          // new bodies from which a split's setup runs on workers (src/fix/split_jobs.h)

    void Load(const std::string& path);
};

extern Config g_config;
