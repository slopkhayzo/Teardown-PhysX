#include "core/config.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>

Config g_config;

static std::string ReadStr(const std::string& ini, const char* sec, const char* key, const std::string& def) {
    char buf[1024];
    GetPrivateProfileStringA(sec, key, def.c_str(), buf, sizeof(buf), ini.c_str());
    return buf;
}

static double ReadNum(const std::string& ini, const char* sec, const char* key, double def) {
    std::string s = ReadStr(ini, sec, key, "");
    return s.empty() ? def : std::atof(s.c_str());
}

static bool ReadBool(const std::string& ini, const char* sec, const char* key, bool def) {
    return ReadNum(ini, sec, key, def ? 1.0 : 0.0) != 0.0;
}

void Config::Load(const std::string& path) {
    iniPath = path;
    traceEnabled = ReadBool(path, "Trace", "Enabled", traceEnabled);
    outDir = ReadStr(path, "Trace", "OutDir", outDir);
    runDir = ReadStr(path, "Trace", "RunDir", runDir);
    label = ReadStr(path, "Trace", "Label", label);
    samplerEnabled = ReadBool(path, "Trace", "Sampler", samplerEnabled);
    samplerHz = int(ReadNum(path, "Trace", "SamplerHz", samplerHz));
    samplerDepth = int(ReadNum(path, "Trace", "SamplerDepth", samplerDepth));
    samplerPhysx = ReadBool(path, "Trace", "SamplerPhysX", samplerPhysx);
    gpuKernels = ReadBool(path, "Trace", "GpuKernels", gpuKernels);
    threadsEnabled = ReadBool(path, "Trace", "Threads", threadsEnabled);

    appDataRoot = ReadStr(path, "Sandbox", "AppDataRoot", appDataRoot);
    modOverrides = ReadStr(path, "Mods", "Overrides", modOverrides);

    benchEnabled = ReadBool(path, "Bench", "Enabled", benchEnabled);
    mode = ReadStr(path, "Session", "Mode", benchEnabled ? "bench" : mode);
    if (mode == "bench") benchEnabled = true;
    // level parameters: [Level], falling back to the [Bench] keys of older inis
    mod = ReadStr(path, "Level", "Mod", ReadStr(path, "Bench", "Mod", mod));
    modWaitSeconds = ReadNum(path, "Level", "ModWaitSeconds", ReadNum(path, "Bench", "ModWaitSeconds", modWaitSeconds));
    mission = ReadStr(path, "Level", "Mission", ReadStr(path, "Bench", "Mission", mission));
    levelFile = ReadStr(path, "Level", "Level", ReadStr(path, "Bench", "Level", levelFile));
    layers = ReadStr(path, "Level", "Layers", ReadStr(path, "Bench", "Layers", layers));

    recordFile = ReadStr(path, "Record", "File", recordFile);
    recordQuitAfter = ReadBool(path, "Record", "QuitAfter", recordQuitAfter);
    recordSeed = unsigned(ReadNum(path, "Record", "Seed", recordSeed));
    recordStopAfterFrames = int(ReadNum(path, "Record", "StopAfterFrames", recordStopAfterFrames));
    replayFile = ReadStr(path, "Replay", "File", replayFile);
    replayStopAtPause = ReadBool(path, "Replay", "StopAtPause", replayStopAtPause);
    replayMaxFrames = int(ReadNum(path, "Replay", "Frames", replayMaxFrames));
    replayReRecordFile = ReadStr(path, "Replay", "ReRecordFile", replayReRecordFile);
    replayPinPlayer = ReadBool(path, "Replay", "PinPlayer", replayPinPlayer);
    dumpBodies = ReadStr(path, "Session", "DumpBodies", dumpBodies);
    serialJobs = ReadBool(path, "Session", "SerialJobs", serialJobs);
    hangKillSeconds = int(ReadNum(path, "Session", "HangKillSeconds", hangKillSeconds));
    splashSkipSeconds = ReadNum(path, "Bench", "SplashSkipSeconds", splashSkipSeconds);
    startDelaySeconds = ReadNum(path, "Bench", "StartDelaySeconds", startDelaySeconds);
    warmupSeconds = ReadNum(path, "Bench", "WarmupSeconds", warmupSeconds);
    durationSeconds = ReadNum(path, "Bench", "DurationSeconds", durationSeconds);
    holesPerSecond = ReadNum(path, "Bench", "HolesPerSecond", holesPerSecond);
    r0 = float(ReadNum(path, "Bench", "R0", r0));
    r1 = float(ReadNum(path, "Bench", "R1", r1));
    r2 = float(ReadNum(path, "Bench", "R2", r2));
    sound = ReadBool(path, "Bench", "Sound", sound);
    toolEnabled = ReadBool(path, "Tool", "Enabled", toolEnabled);
    toolKey = ReadStr(path, "Tool", "Key", toolKey);
    toolHolesPerTick = int(ReadNum(path, "Tool", "HolesPerTick", toolHolesPerTick));
    toolSpread = float(ReadNum(path, "Tool", "Spread", toolSpread));
    toolR0 = float(ReadNum(path, "Tool", "R0", toolR0));
    toolR1 = float(ReadNum(path, "Tool", "R1", toolR1));
    toolR2 = float(ReadNum(path, "Tool", "R2", toolR2));
    toolSound = ReadBool(path, "Tool", "Sound", toolSound);
    toolRange = float(ReadNum(path, "Tool", "Range", toolRange));
    toolAutoFire = ReadBool(path, "Tool", "AutoFire", toolAutoFire);
    toolAutoFireSeconds = ReadNum(path, "Tool", "AutoFireSeconds", toolAutoFireSeconds);
    toolAimPitchDeg = ReadNum(path, "Tool", "AimPitchDeg", toolAimPitchDeg);
    toolAimYawDeg = ReadNum(path, "Tool", "AimYawDeg", toolAimYawDeg);
    toolMode = ReadStr(path, "Tool", "Mode", toolMode);
    toolFragmentSize = float(ReadNum(path, "Tool", "FragmentSize", toolFragmentSize));
    toolPieceSize = float(ReadNum(path, "Tool", "PieceSize", toolPieceSize));
    toolFragmentTicks = int(ReadNum(path, "Tool", "FragmentTicks", toolFragmentTicks));
    toolCarveBatch = int(ReadNum(path, "Tool", "CarveBatch", toolCarveBatch));
    toolCarveMsPerTick = float(ReadNum(path, "Tool", "CarveMsPerTick", toolCarveMsPerTick));
    sweepYawDeg = ReadNum(path, "Bench", "SweepYawDeg", sweepYawDeg);
    sweepPitchDeg = ReadNum(path, "Bench", "SweepPitchDeg", sweepPitchDeg);
    sweepPeriodSeconds = ReadNum(path, "Bench", "SweepPeriodSeconds", sweepPeriodSeconds);
    aimYawDeg = ReadNum(path, "Bench", "AimYawDeg", aimYawDeg);
    aimPitchDeg = ReadNum(path, "Bench", "AimPitchDeg", aimPitchDeg);
    turnYawDeg = ReadNum(path, "Bench", "TurnYawDeg", turnYawDeg);
    turnPitchDeg = ReadNum(path, "Bench", "TurnPitchDeg", turnPitchDeg);
    {
        std::string v = ReadStr(path, "Bench", "View", "");
        viewSet = std::sscanf(v.c_str(), "%f %f %f %f %f %f", &view[0], &view[1], &view[2], &view[3], &view[4],
                              &view[5]) == 6;
    }
    maxRayDistance = ReadNum(path, "Bench", "MaxRayDistance", maxRayDistance);
    laserEffects = ReadBool(path, "Bench", "LaserEffects", laserEffects);
    effectRadius = ReadNum(path, "Bench", "EffectRadius", effectRadius);
    effectSeconds = ReadNum(path, "Bench", "EffectSeconds", effectSeconds);
    pushLight = ReadNum(path, "Bench", "PushLight", pushLight);
    pushHeavy = ReadNum(path, "Bench", "PushHeavy", pushHeavy);
    spinLight = ReadNum(path, "Bench", "SpinLight", spinLight);
    lightMass = ReadNum(path, "Bench", "LightMass", lightMass);
    paintMin = ReadNum(path, "Bench", "PaintMin", paintMin);
    paintMax = ReadNum(path, "Bench", "PaintMax", paintMax);

    pxShadow = int(ReadNum(path, "PhysX", "Shadow", pxShadow));
    pxThreads = int(ReadNum(path, "PhysX", "Threads", pxThreads));
    pxMaxBodyVoxels = int(ReadNum(path, "PhysX", "MaxBodyVoxels", pxMaxBodyVoxels));
    pxBoxBudget = int(ReadNum(path, "PhysX", "BoxBudget", pxBoxBudget));
    pxHull = ReadStr(path, "PhysX", "Collider", pxHull ? "hull" : "boxes") == "hull";
    pxHullVertices = int(ReadNum(path, "PhysX", "HullVertices", pxHullVertices));
    pxSplitShapes = int(ReadNum(path, "PhysX", "SplitShapes", pxSplitShapes));
    pxHullMaxBloat = float(ReadNum(path, "PhysX", "HullMaxBloat", pxHullMaxBloat));
    pxHullParts = int(ReadNum(path, "PhysX", "HullParts", pxHullParts));
    pxBlockBoxes = int(ReadNum(path, "PhysX", "BlockBoxes", pxBlockBoxes));
    pxStaticAggregates = int(ReadNum(path, "PhysX", "StaticAggregates", pxStaticAggregates));
    pxBlockSize = int(ReadNum(path, "PhysX", "BlockSize", pxBlockSize));
    pxBuildMsPerTick = ReadNum(path, "PhysX", "BuildMsPerTick", pxBuildMsPerTick);
    pxBodyBuildMsPerTick = ReadNum(path, "PhysX", "BodyBuildMsPerTick", pxBodyBuildMsPerTick);
    pxGravity = float(ReadNum(path, "PhysX", "Gravity", pxGravity));
    pxMaxDepenetration = float(ReadNum(path, "PhysX", "MaxDepenetration", pxMaxDepenetration));
    pxFriction = float(ReadNum(path, "PhysX", "Friction", pxFriction));
    pxRestitution = float(ReadNum(path, "PhysX", "Restitution", pxRestitution));
    pxTgs = ReadBool(path, "PhysX", "TGS", pxTgs);
    pxLogEvery = int(ReadNum(path, "PhysX", "LogEvery", pxLogEvery));
    {
        std::string d = ReadStr(path, "PhysX", "DumpTicks", "");
        if (std::sscanf(d.c_str(), "%d-%d", &pxDumpFrom, &pxDumpTo) != 2) pxDumpFrom = pxDumpTo = -1;
    }
    pxPlayer = ReadBool(path, "PhysX", "Player", pxPlayer);
    pxPlayerRadius = float(ReadNum(path, "PhysX", "PlayerRadius", pxPlayerRadius));
    pxPlayerHeight = float(ReadNum(path, "PhysX", "PlayerHeight", pxPlayerHeight));
    pxPlayerGround = ReadBool(path, "PhysX", "PlayerGround", pxPlayerGround);
    pxPlayerWeight = float(ReadNum(path, "PhysX", "PlayerWeight", pxPlayerWeight));
    pxRegainTicks = int(ReadNum(path, "PhysX", "RegainTicks", pxRegainTicks));
    pxCcd = ReadBool(path, "PhysX", "CCD", pxCcd);
    pxMaxSpeed = float(ReadNum(path, "PhysX", "MaxSpeed", pxMaxSpeed));
    pxProxyMaxSpeed = float(ReadNum(path, "PhysX", "ProxyMaxSpeed", pxProxyMaxSpeed));
    pxProxyImpulseSpeed = float(ReadNum(path, "PhysX", "ProxyImpulseSpeed", pxProxyImpulseSpeed));
    pxAsync = ReadBool(path, "PhysX", "Async", pxAsync);
    pxCatchUp = ReadBool(path, "PhysX", "CatchUp", pxCatchUp);
    pxCatchUpMax = int(ReadNum(path, "PhysX", "CatchUpMax", pxCatchUpMax));
    pxCatchUpWaitMs = float(ReadNum(path, "PhysX", "CatchUpWaitMs", pxCatchUpWaitMs));
    pxPositionIters = int(ReadNum(path, "PhysX", "PositionIters", pxPositionIters));
    pxVelocityIters = int(ReadNum(path, "PhysX", "VelocityIters", pxVelocityIters));
    pxSleepThreshold = float(ReadNum(path, "PhysX", "SleepThreshold", pxSleepThreshold));
    pxSettleDrift = float(ReadNum(path, "PhysX", "SettleDrift", pxSettleDrift));
    pxFreezeAfter = int(ReadNum(path, "PhysX", "FreezeAfter", pxFreezeAfter));
    pxFreezeSpeed = float(ReadNum(path, "PhysX", "FreezeSpeed", pxFreezeSpeed));
    pxSettleClusters = ReadBool(path, "PhysX", "SettleClusters", pxSettleClusters);
    pxThawSpeed = float(ReadNum(path, "PhysX", "ThawSpeed", pxThawSpeed));
    pxThawDistance = float(ReadNum(path, "PhysX", "ThawDistance", pxThawDistance));
    pxFreezePerTick = int(ReadNum(path, "PhysX", "FreezePerTick", pxFreezePerTick));
    pxKillKinematicPairs = ReadBool(path, "PhysX", "KillKinematicPairs", pxKillKinematicPairs);
    pxSmallDebrisCollide = ReadBool(path, "PhysX", "SmallDebrisCollide", pxSmallDebrisCollide);
    pxStabilization = ReadBool(path, "PhysX", "Stabilization", pxStabilization);
    pxImpacts = ReadBool(path, "PhysX", "Impacts", pxImpacts);
    pxLinearDamping = float(ReadNum(path, "PhysX", "LinearDamping", pxLinearDamping));
    pxDiagOverlaps = ReadBool(path, "PhysX", "DiagOverlaps", pxDiagOverlaps);
    pxDebugNoKinematicContacts = ReadBool(path, "PhysX", "DebugNoKinematicContacts", pxDebugNoKinematicContacts);
    pxDebugIslands = int(ReadNum(path, "PhysX", "DebugIslands", pxDebugIslands));
    pxDebugSettle = int(ReadNum(path, "PhysX", "DebugSettle", pxDebugSettle));
    pxAngularDamping = float(ReadNum(path, "PhysX", "AngularDamping", pxAngularDamping));
    pxProxyDeadband = float(ReadNum(path, "PhysX", "ProxyDeadband", pxProxyDeadband));
    pxWriteDeadband = float(ReadNum(path, "PhysX", "WriteDeadband", pxWriteDeadband));
    pxImpactSpeed = float(ReadNum(path, "PhysX", "ImpactSpeed", pxImpactSpeed));
    pxDamage = ReadBool(path, "PhysX", "Damage", pxDamage);
    pxDamageMax = int(ReadNum(path, "PhysX", "DamageMax", pxDamageMax));
    pxGpu = ReadBool(path, "PhysX", "Gpu", pxGpu);
    pxGpuBroadphase = ReadBool(path, "PhysX", "GpuBroadphase", pxGpuBroadphase);
    pxGpuContacts = int(ReadNum(path, "PhysX", "GpuContacts", pxGpuContacts));
    pxGpuPatches = int(ReadNum(path, "PhysX", "GpuPatches", pxGpuPatches));
    pxGpuPairs = int(ReadNum(path, "PhysX", "GpuPairs", pxGpuPairs));
    pxGpuAggregatePairs = int(ReadNum(path, "PhysX", "GpuAggregatePairs", pxGpuAggregatePairs));
    pxGpuPartitions = int(ReadNum(path, "PhysX", "GpuPartitions", pxGpuPartitions));
    pxGpuSync = ReadBool(path, "PhysX", "GpuSync", pxGpuSync);
    touchCache = ReadBool(path, "Fix", "TouchCache", touchCache);
    touchCacheValidate = int(ReadNum(path, "Fix", "TouchCacheValidate", touchCacheValidate));
    touchCacheSymmetric = ReadBool(path, "Fix", "TouchCacheSymmetric", touchCacheSymmetric);
    staticBodyUpdate = ReadBool(path, "Fix", "StaticBodyUpdate", staticBodyUpdate);
    staticBodyUpdateValidate = int(ReadNum(path, "Fix", "StaticBodyUpdateValidate", staticBodyUpdateValidate));
    visibleEarlyOut = ReadBool(path, "Fix", "VisibleEarlyOut", visibleEarlyOut);
    keepDebris = ReadBool(path, "Fix", "KeepDebris", keepDebris);
    splitJobsMin = int(ReadNum(path, "Fix", "SplitJobsMin", splitJobsMin));
    seed = unsigned(ReadNum(path, "Bench", "Seed", seed));
    quitWhenDone = ReadBool(path, "Bench", "QuitWhenDone", quitWhenDone);
    loadTimeoutSeconds = ReadNum(path, "Bench", "LoadTimeoutSeconds", loadTimeoutSeconds);
}
