#include "input/recorder.h"

#include <windows.h>

#include <MinHook.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bench/bench.h"
#include "core/config.h"
#include "core/log.h"
#include "td/addresses.h"
#include "td/game.h"
#include "trace/trace.h"

using td::Addr;

namespace inputrec {

namespace {

constexpr uint32_t kVersion = 3;        // 2: frames without the player state (56 bytes), still replayable
constexpr uint32_t kFrameV2Size = 56;
constexpr uint32_t kFlagSynthetic = 1;  // made by tools/recording.py synth: no state checkpoints to compare
constexpr uint8_t kFramePlayer = 1;     // FrameRec.flags: the player fields are filled

#pragma pack(push, 1)
struct FileHeader {
    char magic[8];  // "TDPXREC1"
    uint32_t version;
    uint32_t headerSize;
    uint32_t gameStamp;
    uint32_t flags;
    char mod[64];
    char level[256];
    char layers[128];
    char mission[64];
    float acc214, acc218;  // the game's tick accumulators at frame 0
    uint32_t seed;         // CRT rand() seed: applied at level start, at frame 0 and for the game's time reseeds
    uint32_t clientW, clientH;
    uint32_t frameSize;    // sizeof(FrameRec)
    uint32_t reserved[12];
};
struct FrameRec {
    uint32_t frame;
    float rawDt;         // what the game's frame timer returned in this frame
    uint8_t state;       // game state at frame start
    uint8_t paused;      // Game+0x1e8 at frame start
    uint8_t active;      // what the game's window-active check returned
    uint8_t flags;
    uint32_t events;     // input messages that follow this record
    uint32_t randCalls;  // CRT rand() calls on the main thread during the frame
    uint32_t bodies;     // body count at frame start
    uint32_t bodyHash;   // FNV-1a over every body's transform bits at frame start
    float camPos[3];     // camera at frame start
    float camRot[4];
    // version 3: the local player at frame start
    float eyePos[3];     // player+0x640
    float feetPos[3];    // player+0x570
    float vel[3];        // player+0x5fc
    float health;        // player+0x7c8
};
struct EventRec {
    uint32_t msg;
    uint32_t rawSize;
    uint64_t wParam;
    uint64_t lParam;
};
#pragma pack(pop)
static_assert(sizeof(FileHeader) == 608, "header layout (tools/recording.py)");
static_assert(sizeof(FrameRec) == 96, "frame layout (tools/recording.py)");

struct Event {
    uint32_t msg;
    uint64_t wp, lp;
    std::vector<uint8_t> raw;
};
struct Frame {
    FrameRec rec{};
    std::vector<Event> events;
};

enum class Mode { Off, RecordArmed, Recording, ReplayArmed, Replaying, Done };
Mode g_mode = Mode::Off;
InputObserver g_observer = nullptr;
bool g_installed = false;
bool g_replaySession = false;  // replay mode: real input never reaches the game, the window counts as focused
bool g_dropInput = false;      // ignore real keyboard/mouse messages (replay: always; record: until frame 0)
HWND g_hwnd = nullptr;
DWORD g_mainTid = 0;

// The frame being collected, in both modes: written to the .tdrec (record, or replay with ReRecordFile), to
// track.csv, and in replay compared with the recording.
int g_frame = -1;
Frame g_cur;
bool g_haveCur = false;
FILE* g_out = nullptr;
std::string g_outPath;
FILE* g_track = nullptr;
uint64_t g_outFrames = 0, g_outEvents = 0;
uint64_t g_startQpc = 0;

// replay input
std::vector<Frame> g_frames;
FileHeader g_header{};
bool g_synthetic = false;
int g_stopFrame = 0;
std::vector<const Event*> g_fake;

// Replay: how far the player had to be moved back onto the recorded path ([Replay] PinPlayer).
struct PinStats {
    int frames = 0, moved = 0, maxFrame = -1;
    double sum = 0.0, max = 0.0;
};
PinStats g_pin;
float g_curPin = 0.0f;  // correction applied at the start of the current frame (track.csv)
// The camera as the frame found it, before any pin: what track.csv shows and what the fidelity check compares
// (the drift of one frame). The frame record itself (a re-recording) holds the state the frame really started from.
float g_trackCam[3] = {}, g_trackRot[4] = {};

// Holes made through Lua MakeHole (the Gluon, other tools) in the current frame: count and position sums, so two
// runs show whether the shots landed in the same places (track.csv).
uint32_t g_holes = 0;
double g_holeSum[3] = {};

struct Fidelity {
    int frames = 0;
    double sumCam = 0.0, maxCam = 0.0;
    int camFirst = -1;
    int hashEqual = 0, hashFirst = -1;
    int randFirst = -1;
};
Fidelity g_fid;

// determinism: the game's CRT (ucrtbase), not this plugin's static one
using SrandFn = void(__cdecl*)(unsigned);
using RandFn = int(__cdecl*)();
using SeedFromTimeFn = void(__fastcall*)();
SrandFn g_srand = nullptr;
RandFn oRand = nullptr;
SeedFromTimeFn oSeedFromTime = nullptr;
uint32_t g_seed = 1;
bool g_seeding = false;
bool g_loadingDt = false;  // from level start to frame 0 the frame timer reads a fixed 1/60 s (see HkTimer)

// Until frame 0 both modes count the window as focused (the game remembers being unfocused in the menu, and the
// frame that enters PLAY runs before frame 0), so a session starts the same whether or not the window had focus.
// That is exactly while real input is dropped; from frame 0 a recording stores what the game's check said.
bool ForcedFocus() { return g_dropInput; }
uint32_t g_reseeds = 0;
uint32_t g_randFrame = 0;  // main thread only
std::atomic<uint64_t> g_randMain{0}, g_randOther{0};
struct CallerSlot {
    std::atomic<uintptr_t> ra{0};
    std::atomic<uint32_t> n{0};
};
CallerSlot g_otherCallers[16];
std::atomic<uint64_t> g_randJobs{0};
std::atomic<uint32_t> g_tick{0};  // 60 Hz ticks since frame 0 (main thread writes, jobs read)

// rand() inside a job: the MSVC CRT algorithm on a state that belongs to the job, not to the thread running it.
struct JobRand {
    uint32_t depth;
    uint32_t state;
};
thread_local JobRand t_job{};
using JobRunFn = void(__fastcall*)(void* job);
JobRunFn oJobRun = nullptr;

uint32_t Mix(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = a * 0x9E3779B1u ^ b * 0x85EBCA6Bu ^ c * 0xC2B2AE35u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

// Every 64th off-main-thread call outside jobs: who called rand()'s caller (rand -> RandomInt/RandomFloat -> X -> Y).
struct StackSlot {
    std::atomic<uint64_t> key{0};  // rva(X) << 32 | rva(Y)
    std::atomic<uint32_t> n{0};
};
StackSlot g_otherStacks[32];
std::atomic<uint32_t> g_otherFrame{0};  // off-main calls in the current frame

using WndProcFn = LRESULT(CALLBACK*)(HWND, UINT, WPARAM, LPARAM);
using PumpFn = void(__fastcall*)(void* window, void* input);
using TimerFn = float(__fastcall*)(void* timer);
using IsActiveFn = uint64_t(__fastcall*)();
using GridFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using ForegroundFn = HWND(WINAPI*)();
WndProcFn oWndProc = nullptr;
PumpFn oPump = nullptr;
TimerFn oTimer = nullptr;
IsActiveFn oIsActive = nullptr;
GridFn oGrid = nullptr;
SetCursorPosFn oSetCursorPos = nullptr;
ForegroundFn oForeground = nullptr;

constexpr uintptr_t kFakeBase = 0x7FFF000000000000ull;

UINT WINAPI HkGrid(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdr);

bool IsInputMsg(UINT m) {
    return m == WM_INPUT || (m >= WM_KEYFIRST && m <= WM_KEYLAST) || (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST);
}

double SecondsSince(uint64_t qpc) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return double(trace::Now() - qpc) / double(f.QuadPart);
}

uint32_t BodyHash(uint32_t& count) {
    int n = 0;
    uint8_t** bodies = td::SceneList<uint8_t*>(0x230, n);
    uint32_t h = 2166136261u;
    for (int i = 0; bodies && i < n; ++i) {
        if (!bodies[i]) continue;
        const uint32_t* w = reinterpret_cast<const uint32_t*>(bodies[i] + 0x40);  // pos + rot
        for (int k = 0; k < 7; ++k) h = (h ^ w[k]) * 16777619u;
    }
    count = uint32_t(n);
    return h;
}

void FillState(FrameRec& r) {
    uint8_t* game = td::GameObj();
    r.state = uint8_t(td::State());
    r.paused = game ? td::At<uint8_t>(game, 0x1e8) : 0;
    r.active = 1;
    td::Transform cam{};
    td::CameraTransform(cam);
    std::memcpy(r.camPos, cam.pos, sizeof(r.camPos));
    std::memcpy(r.camRot, cam.rot, sizeof(r.camRot));
    r.bodyHash = BodyHash(r.bodies);
    if (uint8_t* p = td::LocalPlayer()) {
        std::memcpy(r.eyePos, &td::At<float>(p, 0x640), sizeof(r.eyePos));
        std::memcpy(r.feetPos, &td::At<float>(p, 0x570), sizeof(r.feetPos));
        std::memcpy(r.vel, &td::At<float>(p, 0x5fc), sizeof(r.vel));
        r.health = td::At<float>(p, 0x7c8);
        r.flags |= kFramePlayer;
    }
}

void CopyStr(char* dst, size_t n, const std::string& s) {
    std::strncpy(dst, s.c_str(), n - 1);
    dst[n - 1] = 0;
}

bool OpenOut(const std::string& path) {
    g_outPath = path;
    g_out = std::fopen(path.c_str(), "wb");
    if (!g_out) {
        LOG("record: cannot open %s", path.c_str());
        return false;
    }
    std::setvbuf(g_out, nullptr, _IOFBF, 1 << 20);
    FileHeader h{};
    std::memcpy(h.magic, "TDPXREC1", 8);
    h.version = kVersion;
    h.headerSize = sizeof(FileHeader);
    h.gameStamp = td::kBuildStamp;
    CopyStr(h.mod, sizeof(h.mod), g_config.mod);
    CopyStr(h.level, sizeof(h.level), g_config.levelFile);
    CopyStr(h.layers, sizeof(h.layers), g_config.layers);
    CopyStr(h.mission, sizeof(h.mission), g_config.mission);
    uint8_t* game = td::GameObj();
    h.acc214 = td::At<float>(game, 0x214);
    h.acc218 = td::At<float>(game, 0x218);
    h.seed = g_seed;
    RECT rc{};
    if (g_hwnd && GetClientRect(g_hwnd, &rc)) {
        h.clientW = uint32_t(rc.right - rc.left);
        h.clientH = uint32_t(rc.bottom - rc.top);
    }
    h.frameSize = sizeof(FrameRec);
    std::fwrite(&h, sizeof(h), 1, g_out);
    g_outFrames = g_outEvents = 0;
    return true;
}

void OpenTrack() {
    g_track = std::fopen((g_config.runDir + "\\track.csv").c_str(), "w");
    if (!g_track) return;
    std::setvbuf(g_track, nullptr, _IOFBF, 1 << 18);
    std::fputs("frame,dt,state,paused,active,events,rand,bodies,hash,x,y,z,qx,qy,qz,qw,rand_other,ex,ey,ez,health,pin,"
               "holes,hole_x,hole_y,hole_z\n",
               g_track);
}

void CloseFiles() {
    if (g_out) {
        std::fflush(g_out);
        std::fclose(g_out);
        g_out = nullptr;
    }
    if (g_track) {
        std::fflush(g_track);
        std::fclose(g_track);
        g_track = nullptr;
    }
}

void SeedNow(uint32_t seed) {
    if (g_srand) g_srand(seed);
}

void Capture(UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_haveCur) return;
    ++g_cur.rec.events;
    if (!g_out) return;  // replay without ReRecordFile: only the count is needed
    Event e{msg, wp, uint64_t(lp), {}};
    if (msg == WM_INPUT) {
        // through our GetRawInputData so that replayed (fake) handles work too
        UINT size = 0;
        HkGrid(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
        if (size > 0 && size <= 4096) {
            e.raw.resize(size);
            if (HkGrid(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, e.raw.data(), &size, sizeof(RAWINPUTHEADER)) !=
                size)
                e.raw.clear();
        }
    }
    g_cur.events.push_back(std::move(e));
}

// Debug: every body at the start of the frames listed in [Session] DumpBodies (diff two runs to see what diverges).
bool DumpFrame(int frame) {
    const std::string& s = g_config.dumpBodies;
    for (size_t i = 0; i < s.size();) {
        int lo = 0, hi = 0, used = 0;
        int n = std::sscanf(s.c_str() + i, "%d-%d%n", &lo, &hi, &used);
        if (n == 1 && std::sscanf(s.c_str() + i, "%d%n", &lo, &used) == 1) hi = lo;
        if (n < 1 || used <= 0) break;
        if (frame >= lo && frame <= hi) return true;
        i += size_t(used);
        while (i < s.size() && (s[i] == ',' || s[i] == ' ')) ++i;
    }
    return false;
}

void DumpBodies(int frame) {
    static FILE* f = nullptr;
    if (g_config.dumpBodies.empty() || !DumpFrame(frame)) return;
    if (!f) {
        f = std::fopen((g_config.runDir + "\\bodies.csv").c_str(), "w");
        if (!f) return;
        std::fputs("frame,index,handle,dynamic,sleep,x,y,z,qx,qy,qz,qw,vx,vy,vz,wx,wy,wz\n", f);
    }
    int n = 0;
    uint8_t** bodies = td::SceneList<uint8_t*>(0x230, n);
    for (int i = 0; bodies && i < n; ++i) {
        uint8_t* b = bodies[i];
        if (!b) continue;
        const float* p = &td::At<float>(b, 0x40);
        const float* v = &td::At<float>(b, 0xd8);
        std::fprintf(f, "%d,%d,%d,%u,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n", frame, i,
                     td::At<int>(b, 0x0c), td::At<uint8_t>(b, 0x138), td::At<uint8_t>(b, 0x140), p[0], p[1], p[2], p[3],
                     p[4], p[5], p[6], v[0], v[1], v[2], v[3], v[4], v[5]);
    }
    std::fflush(f);
}

// Replay: put the player where the recording had it at this frame's start. Debris differs from run to run once
// pieces collide, and it can push the player or hold it at another height; without this the camera (and every later
// shot) drifts. Version-3 recordings carry the player's eye, feet, velocity and health, which are restored exactly.
// Version 2 only has the camera: the player is shifted by the camera's offset, with god mode on in place of the
// recorded health. Collisions stay on, so the player still pushes debris as it did in the recording.
void PinPlayer(int frame) {
    g_curPin = 0.0f;
    if (!g_config.replayPinPlayer || g_synthetic || frame < 0 || frame >= int(g_frames.size())) return;
    uint8_t* p = td::LocalPlayer();
    if (!p) return;
    const FrameRec& rec = g_frames[size_t(frame)].rec;
    float* feet = &td::At<float>(p, 0x570);
    float* eye = &td::At<float>(p, 0x640);
    float* eyeClient = &td::At<float>(p, 0x678);
    bool exact = (rec.flags & kFramePlayer) != 0;
    float d[3];
    for (int k = 0; k < 3; ++k) d[k] = exact ? rec.eyePos[k] - eye[k] : rec.camPos[k] - g_cur.rec.camPos[k];
    double len = std::sqrt(double(d[0]) * d[0] + double(d[1]) * d[1] + double(d[2]) * d[2]);
    for (int k = 0; k < 3; ++k) {
        eyeClient[k] += d[k];
        if (exact) {
            eye[k] = rec.eyePos[k];
            feet[k] = rec.feetPos[k];
            td::At<float>(p, 0x5fc + 4 * k) = rec.vel[k];
        } else {
            eye[k] += d[k];
            feet[k] += d[k];
        }
    }
    if (exact)
        td::At<float>(p, 0x7c8) = rec.health;
    else
        td::At<uint8_t>(p, 0x6fc) = 1;
    // The render camera too: scripts aim with GetCameraTransform() (the Gluon's raycasts do), which includes
    // offsets the player state doesn't hold (head bob, landing dip, ShakeCamera while firing).
    if (uint8_t* game = td::GameObj()) {
        if (uint8_t* cam = td::At<uint8_t*>(game, 0x60)) {
            std::memcpy(&td::At<float>(cam, 0x1d0), rec.camPos, sizeof(rec.camPos));
            std::memcpy(&td::At<float>(cam, 0x1dc), rec.camRot, sizeof(rec.camRot));
        }
    }
    g_curPin = float(len);
    ++g_pin.frames;
    if (len > 1e-5) ++g_pin.moved;
    g_pin.sum += len;
    if (len > g_pin.max) {
        g_pin.max = len;
        g_pin.maxFrame = frame;
    }
}

void BeginFrame(int n) {
    g_frame = n;
    g_cur = Frame{};
    g_cur.rec.frame = uint32_t(n);
    FillState(g_cur.rec);
    std::memcpy(g_trackCam, g_cur.rec.camPos, sizeof(g_trackCam));
    std::memcpy(g_trackRot, g_cur.rec.camRot, sizeof(g_trackRot));
    DumpBodies(n);
    if (g_mode == Mode::Replaying || g_mode == Mode::ReplayArmed) {
        PinPlayer(n);
        if (g_curPin > 0.0f || (g_config.replayPinPlayer && !g_synthetic)) {
            // what this frame really starts from (a re-recording of this replay reproduces it)
            td::Transform cam{};
            if (td::CameraTransform(cam)) {
                std::memcpy(g_cur.rec.camPos, cam.pos, sizeof(g_cur.rec.camPos));
                std::memcpy(g_cur.rec.camRot, cam.rot, sizeof(g_cur.rec.camRot));
            }
            if (uint8_t* p = td::LocalPlayer()) {
                std::memcpy(g_cur.rec.eyePos, &td::At<float>(p, 0x640), sizeof(g_cur.rec.eyePos));
                std::memcpy(g_cur.rec.feetPos, &td::At<float>(p, 0x570), sizeof(g_cur.rec.feetPos));
                std::memcpy(g_cur.rec.vel, &td::At<float>(p, 0x5fc), sizeof(g_cur.rec.vel));
                g_cur.rec.health = td::At<float>(p, 0x7c8);
            }
        }
    }
    g_randFrame = 0;
    g_haveCur = true;
}

void WriteFrame(const Frame& f) {
    if (!g_out) return;
    FrameRec r = f.rec;
    r.events = uint32_t(f.events.size());
    std::fwrite(&r, sizeof(r), 1, g_out);
    for (const Event& e : f.events) {
        EventRec er{e.msg, uint32_t(e.raw.size()), e.wp, e.lp};
        std::fwrite(&er, sizeof(er), 1, g_out);
        if (!e.raw.empty()) std::fwrite(e.raw.data(), 1, e.raw.size(), g_out);
    }
    ++g_outFrames;
    g_outEvents += r.events;
    if (g_outFrames % 120 == 0) std::fflush(g_out);
}

void TrackLine(const FrameRec& r, uint32_t randOther) {
    if (!g_track) return;
    std::fprintf(g_track,
                 "%u,%.9g,%u,%u,%u,%u,%u,%u,%08x,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%.9g,%.9g,%.9g,%.9g,%.6g,%u,%.6f,"
                 "%.6f,%.6f\n",
                 r.frame, r.rawDt, r.state, r.paused, r.active, r.events, r.randCalls, r.bodies, r.bodyHash,
                 g_trackCam[0], g_trackCam[1], g_trackCam[2], g_trackRot[0], g_trackRot[1], g_trackRot[2],
                 g_trackRot[3], randOther, r.eyePos[0], r.eyePos[1], r.eyePos[2], r.health, g_curPin, g_holes,
                 g_holeSum[0], g_holeSum[1], g_holeSum[2]);
    g_holes = 0;
    g_holeSum[0] = g_holeSum[1] = g_holeSum[2] = 0.0;
}

// Replay: the frame that just finished, against the same frame of the recording.
void Compare(const FrameRec& live) {
    if (g_synthetic || live.frame >= g_frames.size()) return;
    const FrameRec& rec = g_frames[live.frame].rec;
    double dx = g_trackCam[0] - rec.camPos[0], dy = g_trackCam[1] - rec.camPos[1], dz = g_trackCam[2] - rec.camPos[2];
    double err = std::sqrt(dx * dx + dy * dy + dz * dz);
    ++g_fid.frames;
    g_fid.sumCam += err;
    if (err > g_fid.maxCam) g_fid.maxCam = err;
    if (g_fid.camFirst < 0 && err > 1e-4) {
        g_fid.camFirst = int(live.frame);
        LOG("replay: camera first differs at frame %u (%.5f m)", live.frame, err);
    }
    if (live.bodyHash == rec.bodyHash && live.bodies == rec.bodies) {
        ++g_fid.hashEqual;
    } else if (g_fid.hashFirst < 0) {
        g_fid.hashFirst = int(live.frame);
        LOG("replay: body transforms first differ at frame %u (bodies: recorded %u, now %u)", live.frame, rec.bodies,
            live.bodies);
    }
    if (g_fid.randFirst < 0 && live.randCalls != rec.randCalls) {
        g_fid.randFirst = int(live.frame);
        LOG("replay: main-thread rand() calls first differ in frame %u (recorded %u, now %u)", live.frame,
            rec.randCalls, live.randCalls);
    }
    if (live.frame % 600 == 0 && live.frame)
        LOG("replay: frame %u, camera off by %.4f m (max so far %.4f), bodies %u", live.frame, err, g_fid.maxCam,
            live.bodies);
}

void EndFrame() {
    if (!g_haveCur) return;
    g_cur.rec.randCalls = g_randFrame;
    WriteFrame(g_cur);
    TrackLine(g_cur.rec, g_otherFrame.exchange(0));
    if (g_mode == Mode::Replaying) Compare(g_cur.rec);
    g_haveCur = false;
}

void LogRandStats() {
    LOG("determinism: seed %u; rand() calls: main thread %llu, in jobs %llu (job-local), other threads outside jobs "
        "%llu; time-based reseeds replaced: %u",
        g_seed, g_randMain.load(), g_randJobs.load(), g_randOther.load(), g_reseeds);
    uintptr_t base = td::Base();
    for (const CallerSlot& s : g_otherCallers) {
        uintptr_t ra = s.ra.load();
        if (!ra) break;
        if (ra > base && ra < base + td::kBuildImageSize)
            LOG("  rand() off the main thread from teardown.exe+%#llx: %u calls", uint64_t(ra - base), s.n.load());
        else
            LOG("  rand() off the main thread from %p: %u calls", reinterpret_cast<void*>(ra), s.n.load());
    }
    for (const StackSlot& s : g_otherStacks) {
        uint64_t key = s.key.load();
        if (!key) break;
        LOG("  off-main rand() sample: called from +%#llx <- +%#llx: %u samples (x64)", key >> 32, key & 0xffffffffull,
            s.n.load());
    }
}

void StartRecording() {
    SeedNow(g_seed);
    g_reseeds = 0;
    g_tick = 0;
    g_loadingDt = false;
    OpenTrack();
    if (!OpenOut(g_config.recordFile)) {
        g_mode = Mode::Done;
        g_dropInput = false;
        return;
    }
    BeginFrame(0);
    g_dropInput = false;
    g_startQpc = trace::Now();
    g_mode = Mode::Recording;
    LOG("record: started (frame 0 = first PLAY frame, seed %u), writing %s", g_seed, g_outPath.c_str());
}

void FinishRecording() {
    if (g_mode != Mode::Recording) return;
    EndFrame();
    CloseFiles();
    g_mode = Mode::Done;
    LOG("record: saved %llu frames, %llu input events, %.1f s", g_outFrames, g_outEvents, SecondsSince(g_startQpc));
    LogRandStats();
}

void StartReplay() {
    uint8_t* game = td::GameObj();
    SeedNow(g_seed);
    g_reseeds = 0;
    g_tick = 0;
    g_loadingDt = false;
    td::At<float>(game, 0x214) = g_header.acc214;
    td::At<float>(game, 0x218) = g_header.acc218;
    OpenTrack();
    if (!g_config.replayReRecordFile.empty()) OpenOut(g_config.replayReRecordFile);
    BeginFrame(0);
    g_startQpc = trace::Now();
    g_mode = Mode::Replaying;
    LOG("replay: started, %zu frames (stopping at %d)%s%s", g_frames.size(), g_stopFrame,
        g_out ? ", re-recording to " : "", g_out ? g_outPath.c_str() : "");
}

void FinishReplay(bool ok) {
    if (g_mode != Mode::Replaying) return;
    g_haveCur = false;  // the frame in progress has not run (or was cut short): not part of the result
    CloseFiles();
    g_mode = Mode::Done;
    double secs = SecondsSince(g_startQpc);
    LOG("replay: %s at frame %d after %.1f s", ok ? "done" : "STOPPED (the game left PLAY)", g_frame, secs);
    if (g_synthetic) {
        LOG("replay: synthetic recording, no checkpoints to compare");
    } else if (g_fid.frames) {
        LOG("replay: camera vs recording: mean %.5f m, max %.5f m, identical (<0.1 mm) through frame %d of %d",
            g_fid.sumCam / g_fid.frames, g_fid.maxCam, g_fid.camFirst < 0 ? g_fid.frames - 1 : g_fid.camFirst - 1,
            g_fid.frames - 1);
        if (g_fid.hashFirst < 0)
            LOG("replay: body transforms identical in all %d frames", g_fid.frames);
        else
            LOG("replay: body transforms identical in %d of %d frames, first difference at frame %d", g_fid.hashEqual,
                g_fid.frames, g_fid.hashFirst);
        LOG("replay: main-thread rand() calls %s", g_fid.randFirst < 0 ? "identical in every frame" : "differ");
    }
    if (g_pin.frames)
        LOG("replay: player pinned to the recording: moved in %d of %d frames, mean %.4f m, max %.4f m (frame %d)",
            g_pin.moved, g_pin.frames, g_pin.sum / g_pin.frames, g_pin.max, g_pin.maxFrame);
    if (!g_config.replayReRecordFile.empty())
        LOG("replay: re-recorded %llu frames, %llu events to %s", g_outFrames, g_outEvents, g_outPath.c_str());
    LogRandStats();
    bench::Quit(ok);
}

// ---- hooks -----------------------------------------------------------------------------------------------

LRESULT CALLBACK HkWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    g_hwnd = h;
    if (IsInputMsg(msg)) {
        if (g_mode == Mode::Recording)
            Capture(msg, wp, lp);
        else if (g_dropInput)
            return 0;
        if (g_observer) g_observer(msg, wp, lp);
    }
    if (msg == WM_ACTIVATE && ForcedFocus()) wp = WA_ACTIVE;  // keep the game focused
    return oWndProc(h, msg, wp, lp);
}

// The main loop pumps messages before FrameUpdate, so what arrives in this pump belongs to the frame record that
// is still open (the previous frame), in recording and replay alike.
void __fastcall HkPump(void* window, void* input) {
    oPump(window, input);
    int fi = g_frame;
    if (g_mode != Mode::Replaying || fi < 0 || fi >= int(g_frames.size()) || !g_hwnd) return;
    const Frame& f = g_frames[size_t(fi)];
    g_fake.clear();
    for (const Event& e : f.events) {
        LPARAM lp = LPARAM(e.lp);
        if (e.msg == WM_INPUT) {
            if (e.raw.empty()) continue;
            g_fake.push_back(&e);
            lp = LPARAM(kFakeBase + g_fake.size() - 1);
        }
        Capture(e.msg, WPARAM(e.wp), lp);
        if (g_observer && IsInputMsg(e.msg)) g_observer(e.msg, e.wp, e.lp);
        oWndProc(g_hwnd, e.msg, WPARAM(e.wp), lp);
    }
}

// The frame that switches LOADING -> PLAY already runs a tick and every script's tick(dt) before frame 0, with the
// dt of a loading-screen frame: during loading the frame time is fixed so that this frame is the same in every run.
float __fastcall HkTimer(void* timer) {
    float v = oTimer(timer);
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != td::VA(Addr::FrameDtReadReturn)) return v;
    if (g_loadingDt && (g_mode == Mode::Off || g_mode == Mode::RecordArmed || g_mode == Mode::ReplayArmed))
        return 1.0f / 60.0f;
    if (g_mode != Mode::Recording && g_mode != Mode::Replaying) return v;
    int fi = g_frame;
    if (g_mode == Mode::Replaying && fi >= 0 && fi < int(g_frames.size())) v = g_frames[size_t(fi)].rec.rawDt;
    if (g_haveCur) g_cur.rec.rawDt = v;
    return v;
}

uint64_t __fastcall HkIsActive() {
    uint64_t v;
    if (ForcedFocus()) {
        int fi = g_frame;
        v = (g_mode == Mode::Replaying && fi >= 0 && fi < int(g_frames.size())) ? g_frames[size_t(fi)].rec.active : 1;
    } else {
        v = (oIsActive() & 0xff) ? 1 : 0;
    }
    if (g_haveCur) g_cur.rec.active = uint8_t(v);
    return v;
}

UINT WINAPI HkGrid(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdr) {
    uintptr_t v = reinterpret_cast<uintptr_t>(h);
    if (v >= kFakeBase && v < kFakeBase + 0x100000) {
        size_t i = size_t(v - kFakeBase);
        if (i >= g_fake.size() || !size) return UINT(-1);
        const Event& e = *g_fake[i];
        UINT n = UINT(e.raw.size());
        if (cmd == RID_HEADER && n >= sizeof(RAWINPUTHEADER)) n = sizeof(RAWINPUTHEADER);
        if (!data) {
            *size = n;
            return 0;
        }
        if (*size < n) {
            *size = n;
            return UINT(-1);
        }
        std::memcpy(data, e.raw.data(), n);
        return n;
    }
    return oGrid(h, cmd, data, size, hdr);
}

BOOL WINAPI HkSetCursorPos(int x, int y) {
    if (ForcedFocus()) return TRUE;  // leave the real cursor alone
    return oSetCursorPos(x, y);
}

HWND WINAPI HkForeground() {
    if (ForcedFocus() && g_hwnd) return g_hwnd;
    return oForeground();
}

int __cdecl HkRand() {
    if (g_seeding) {
        if (t_job.depth) {
            g_randJobs.fetch_add(1, std::memory_order_relaxed);
            t_job.state = t_job.state * 214013u + 2531011u;
            return int((t_job.state >> 16) & 0x7fff);
        }
        if (GetCurrentThreadId() == g_mainTid) {
            ++g_randFrame;
            g_randMain.fetch_add(1, std::memory_order_relaxed);
        } else {
            uint64_t k = g_randOther.fetch_add(1, std::memory_order_relaxed);
            g_otherFrame.fetch_add(1, std::memory_order_relaxed);
            uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
            for (CallerSlot& s : g_otherCallers) {
                uintptr_t cur = s.ra.load(std::memory_order_relaxed);
                if (cur == 0 && s.ra.compare_exchange_strong(cur, ra)) cur = ra;
                if (cur == ra) {
                    s.n.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
            if ((k & 63) == 0) {
                void* fr[3] = {};
                USHORT got = RtlCaptureStackBackTrace(1, 3, fr, nullptr);  // [0] in rand's caller, [1] X, [2] Y
                uintptr_t base = td::Base();
                auto rva = [&](void* p) -> uint64_t {
                    uintptr_t a = reinterpret_cast<uintptr_t>(p);
                    return a > base && a < base + td::kBuildImageSize ? uint64_t(a - base) : 0xffffffffull;
                };
                uint64_t key = (got > 1 ? rva(fr[1]) << 32 : 0) | (got > 2 ? rva(fr[2]) : 0);
                for (StackSlot& s : g_otherStacks) {
                    uint64_t cur = s.key.load(std::memory_order_relaxed);
                    if (cur == 0 && s.key.compare_exchange_strong(cur, key)) cur = key;
                    if (cur == key) {
                        s.n.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                }
            }
        }
    }
    return oRand();
}

using MakeHoleFn = uint64_t(__fastcall*)(void* world, void* shape, const float* pos, float r0, float r1, float r2,
                                         bool sound, const void* impulse);
MakeHoleFn oMakeHole = nullptr;
uint64_t __fastcall HkMakeHole(void* world, void* shape, const float* pos, float r0, float r1, float r2, bool sound,
                               const void* impulse) {
    if (g_haveCur && pos && GetCurrentThreadId() == g_mainTid) {
        ++g_holes;
        for (int k = 0; k < 3; ++k) g_holeSum[k] += pos[k];
    }
    return oMakeHole(world, shape, pos, r0, r1, r2, sound, impulse);
}

// Every job the job manager runs, on a worker or on a thread that helps while it waits (the main thread too).
void __fastcall HkJobRun(void* job) {
    if (!g_seeding) {
        oJobRun(job);
        return;
    }
    JobRand saved = t_job;
    uint32_t type = uint32_t(*reinterpret_cast<uintptr_t*>(job) - td::Base());  // vftable RVA
    t_job.depth = saved.depth + 1;
    t_job.state = Mix(g_seed, g_tick.load(std::memory_order_relaxed), type);
    oJobRun(job);
    t_job = saved;
}

// srand((int)time(NULL)), called by the Game constructor and after a generator at 0x1406a61c0 restores its fixed
// seed. During a session the reseed is deterministic instead.
void __fastcall HkSeedFromTime() {
    if (g_seeding && g_srand) {
        SeedNow(g_seed + 0x9E3779B9u * ++g_reseeds);
        return;
    }
    oSeedFromTime();
}

bool Hook(Addr a, void* detour, void** orig) {
    if (!td::CodeMatches(a)) {
        LOG("inputrec: %s bytes differ", td::Info(a).name);
        return false;
    }
    MH_STATUS st = MH_CreateHook(reinterpret_cast<void*>(td::VA(a)), detour, orig);
    if (st != MH_OK) LOG("inputrec: hook %s: %s", td::Info(a).name, MH_StatusToString(st));
    return st == MH_OK;
}

bool HookApi(const wchar_t* module, const char* name, void* detour, void** orig) {
    MH_STATUS st = MH_CreateHookApi(module, name, detour, orig);
    if (st != MH_OK) LOG("inputrec: hook %s: %s", name, MH_StatusToString(st));
    return st == MH_OK;
}

}  // namespace

bool Install() {
    g_installed = true;
    g_replaySession = g_config.mode == "replay";
    g_dropInput = true;
    g_seed = g_config.recordSeed;
    HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
    g_srand = ucrt ? reinterpret_cast<SrandFn>(GetProcAddress(ucrt, "srand")) : nullptr;
    bool ok = Hook(Addr::WndProc, reinterpret_cast<void*>(&HkWndProc), reinterpret_cast<void**>(&oWndProc));
    ok &= Hook(Addr::MessagePump, reinterpret_cast<void*>(&HkPump), reinterpret_cast<void**>(&oPump));
    ok &= Hook(Addr::TimerElapsed, reinterpret_cast<void*>(&HkTimer), reinterpret_cast<void**>(&oTimer));
    ok &= Hook(Addr::WindowIsActive, reinterpret_cast<void*>(&HkIsActive), reinterpret_cast<void**>(&oIsActive));
    ok &= Hook(Addr::SeedFromTime, reinterpret_cast<void*>(&HkSeedFromTime), reinterpret_cast<void**>(&oSeedFromTime));
    ok &= Hook(Addr::JobRun, reinterpret_cast<void*>(&HkJobRun), reinterpret_cast<void**>(&oJobRun));
    ok &= Hook(Addr::MakeHole, reinterpret_cast<void*>(&HkMakeHole), reinterpret_cast<void**>(&oMakeHole));
    ok &= HookApi(L"user32", "GetRawInputData", reinterpret_cast<void*>(&HkGrid), reinterpret_cast<void**>(&oGrid));
    ok &= HookApi(L"user32", "SetCursorPos", reinterpret_cast<void*>(&HkSetCursorPos),
                  reinterpret_cast<void**>(&oSetCursorPos));
    ok &= HookApi(L"user32", "GetForegroundWindow", reinterpret_cast<void*>(&HkForeground),
                  reinterpret_cast<void**>(&oForeground));
    ok &= HookApi(L"ucrtbase", "rand", reinterpret_cast<void*>(&HkRand), reinterpret_cast<void**>(&oRand));
    ok &= g_srand != nullptr;
    LOG("inputrec: hooks %s", ok ? "installed" : "INCOMPLETE");
    return ok;
}

bool LoadReplay(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        LOG("replay: cannot open %s", path.c_str());
        return false;
    }
    bool ok = std::fread(&g_header, sizeof(g_header), 1, f) == 1 && std::memcmp(g_header.magic, "TDPXREC1", 8) == 0 &&
              g_header.headerSize == sizeof(FileHeader);
    bool v2 = ok && g_header.version == 2 && g_header.frameSize == kFrameV2Size;
    bool v3 = ok && g_header.version == kVersion && g_header.frameSize == sizeof(FrameRec);
    if (!v2 && !v3) {
        LOG("replay: %s is not a version-2/3 TeardownPhysX recording", path.c_str());
        std::fclose(f);
        return false;
    }
    if (g_header.gameStamp != td::kBuildStamp) LOG("replay: WARNING recorded on another game build (%08x)", g_header.gameStamp);
    g_seed = g_header.seed;
    g_synthetic = (g_header.flags & kFlagSynthetic) != 0;
    g_frames.clear();
    uint64_t events = 0;
    for (;;) {
        Frame fr;  // a version-2 frame is the first 56 bytes; its player fields stay zero (no kFramePlayer flag)
        if (std::fread(&fr.rec, g_header.frameSize, 1, f) != 1) break;
        bool bad = false;
        for (uint32_t i = 0; i < fr.rec.events; ++i) {
            EventRec er;
            if (std::fread(&er, sizeof(er), 1, f) != 1 || er.rawSize > 4096) {
                bad = true;
                break;
            }
            Event e{er.msg, er.wParam, er.lParam, std::vector<uint8_t>(er.rawSize)};
            if (er.rawSize && std::fread(e.raw.data(), 1, er.rawSize, f) != er.rawSize) {
                bad = true;
                break;
            }
            fr.events.push_back(std::move(e));
        }
        if (bad) break;
        events += fr.events.size();
        g_frames.push_back(std::move(fr));
    }
    std::fclose(f);
    // Stop where the recording paused for good (the pause menu used to quit); earlier pauses are replayed.
    int lastUnpaused = -1;
    for (int i = 0; i < int(g_frames.size()); ++i)
        if (!g_frames[size_t(i)].rec.paused && g_frames[size_t(i)].rec.state == td::kPlay) lastUnpaused = i;
    g_stopFrame = g_config.replayStopAtPause ? lastUnpaused + 1 : int(g_frames.size());
    if (g_config.replayMaxFrames > 0) g_stopFrame = std::min(g_stopFrame, g_config.replayMaxFrames);
    LOG("replay: loaded %s: %zu frames, %llu input events, seed %u, recorded at %ux%u, level %s / %s / %s%s",
        path.c_str(), g_frames.size(), events, g_seed, g_header.clientW, g_header.clientH, g_header.mod,
        g_header.level, g_header.layers, g_synthetic ? " (synthetic)" : "");
    return !g_frames.empty();
}

void OnLevelStart() {
    if (!g_srand || (g_config.mode != "record" && g_config.mode != "replay")) return;
    g_seeding = true;
    g_loadingDt = true;
    SeedNow(g_seed ^ 0x51ED5EEDu);
}

void OnStateSwitching(int, int newState) {
    // The switch into PLAY builds the scene (script init() runs inside it), after a loading screen whose frame count
    // and main-thread rand() use vary from run to run: give the scene build a fixed sequence.
    if (g_seeding && newState == td::kPlay && g_mode == Mode::Off) SeedNow(Mix(g_seed, 0x5CE4E, 0));
}

void ArmRecord() {
    if (g_mode == Mode::Off) g_mode = Mode::RecordArmed;
}

void ArmReplay() {
    if (g_mode == Mode::Off) g_mode = Mode::ReplayArmed;
}

void OnFrameStart() {
    if (!g_mainTid) g_mainTid = GetCurrentThreadId();
    switch (g_mode) {
        case Mode::RecordArmed:
            if (td::State() == td::kPlay) StartRecording();
            break;
        case Mode::Recording:
            EndFrame();
            if (g_config.recordStopAfterFrames > 0 && g_frame + 1 >= g_config.recordStopAfterFrames) {
                g_haveCur = false;
                FinishRecording();
                bench::Quit(true);
                break;
            }
            BeginFrame(g_frame + 1);
            break;
        case Mode::ReplayArmed:
            if (td::State() == td::kPlay) StartReplay();
            break;
        case Mode::Replaying: {
            EndFrame();
            int next = g_frame + 1;
            if (next >= g_stopFrame || next >= int(g_frames.size())) {
                FinishReplay(true);
                break;
            }
            BeginFrame(next);
            if (next % 60 == 0 && next <= 1800) {
                const float* q = g_cur.rec.camRot;  // heading of the camera's -Z axis, degrees
                float fx = -(2 * (q[0] * q[2] + q[3] * q[1])), fz = -(1 - 2 * (q[0] * q[0] + q[1] * q[1]));
                LOG("replay: frame %d camera (%.2f, %.2f, %.2f) heading %.1f, bodies %u", next, g_cur.rec.camPos[0],
                    g_cur.rec.camPos[1], g_cur.rec.camPos[2], std::atan2(fx, fz) * 57.29578, g_cur.rec.bodies);
            }
            break;
        }
        default:
            break;
    }
}

void OnTick() {
    if (g_mode == Mode::Recording || g_mode == Mode::Replaying) g_tick.fetch_add(1, std::memory_order_relaxed);
}

void OnLeavePlay() {
    if (g_mode == Mode::Recording) {
        FinishRecording();
        if (g_config.recordQuitAfter) bench::Quit(true);
    } else if (g_mode == Mode::Replaying) {
        FinishReplay(false);
    }
}

void SetInputObserver(InputObserver fn) { g_observer = fn; }

bool Driving() { return g_installed; }

void FlushAtExit() {
    if (g_mode != Mode::Recording) return;
    if (g_haveCur && g_out) {
        g_cur.rec.randCalls = g_randFrame;
        WriteFrame(g_cur);
    }
    if (g_out) {
        _fflush_nolock(g_out);
        _fclose_nolock(g_out);
        g_out = nullptr;
    }
    if (g_track) {
        _fflush_nolock(g_track);
        _fclose_nolock(g_track);
        g_track = nullptr;
    }
}

}  // namespace inputrec
