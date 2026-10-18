// COD VR - camera control probe.
//
// Established by earlier runs (all measured, not assumed):
//   * a proxy opengl32.dll loads inside CoDSP.exe despite the Steam DRM stub
//   * cgamex86.dll keeps Quake 3's module ABI: it exports vmMain and dllEntry,
//     and dllEntry stashes the engine syscall pointer in a module global whose
//     address can be recovered from the instruction stream at runtime
//   * syscalls 71 and 243 each receive, exactly once per frame, a pointer to
//     one shared struct whose layout is byte-identical to Quake 3's refdef_t:
//     x,y,w,h at +0, fov_x/fov_y at +16, vieworg at +24, viewaxis at +36
//
// What remains unproven, and what this build settles:
//   1. do writes to that struct actually reach the renderer?
//   2. does the engine accept two scene renders in one frame, each with its
//      own viewport and camera?  (this is exactly what stereo requires)
//   3. which of 71 / 243 is R_RenderScene?
//
// The test is a timed sequence of phases, each isolating one variable, scored
// by the DLL itself from framebuffer readback -- nobody has to watch a screen.
// Comparing the screen's halves to each other would prove nothing, since a
// real scene is asymmetric; halving the viewport is content-independent,
// because either the right half is blank or it is not.
//
// Run with CODVR_SELFTEST=1 in the environment to execute the offline unit
// tests at load time instead (see SelfTest), which exercise the hand-written
// assembly hook without needing the game.

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <share.h>
#include <stdarg.h>
#include <math.h>
#include <float.h>

static HMODULE  g_realGL;
static FILE*    g_log;
static CRITICAL_SECTION g_logLock;
static bool     g_logReady;

#include "gl_thunks.inc"
#include "vr.h"
#include "codvr.h"
#include "vr_math.h"
#include "vr_input.h"
#include "vr_weapon.h"
#include "vr_binds.h"
#include "vr_glsurf.h"
#include "vr_vbo.h"
#include "vr_handpose.h"
#include "vr_objects.h"
#include "vr_native_hands.h"
#include "vr_menu.h"
#include "vr_snapshot.h"
#include "cfg_line.h"
#include "vr_cfg_contract.h"

// vr.cpp resolves modern GL entry points through here, since this translation
// unit owns the handle to the real driver. wglGetProcAddress is required for
// anything past GL 1.1 on Windows; GetProcAddress covers the 1.1 core.
extern "C" void* CODVR_GetGLProc(const char* name);
void Log(const char* fmt, ...);            // defined below, in the logging section

// Vertical sync caps the game's frame rate to the monitor, and because frames
// alternate between eyes each eye then updates at half of that -- 42 Hz off a
// 85 Hz cap, which judders badly. The headset has its own compositor doing the
// pacing, so syncing to the desktop monitor buys nothing. Force the interval to
// zero and let the game run as fast as it can; each eye then refreshes far
// above the display rate and the alternation stops being visible.
extern "C" BOOL WINAPI i_wglSwapIntervalEXT(int interval)
{
    typedef BOOL (WINAPI *Fn)(int);
    typedef PROC (WINAPI *GetProcFn)(LPCSTR);
    static Fn real = nullptr;
    static bool looked = false;
    if (!looked) {
        looked = true;
        // Resolve the REAL entry point directly. Going through CODVR_GetGLProc
        // would hand back this very wrapper and recurse forever.
        if (p_wglGetProcAddress)
            real = (Fn)((GetProcFn)p_wglGetProcAddress)("wglSwapIntervalEXT");
    }
    if (!real)
        return FALSE;
    static bool logged = false;
    if (!logged && interval != 0) {
        logged = true;
        Log("VR: game asked for swap interval %d; forcing 0 (vsync off) so the "
            "per-eye rate is not halved by the monitor", interval);
    }
    return real(0);
}

// The game obtains extension entry points through wglGetProcAddress, so this is
// the only place its vsync call can actually be caught.
// Defined with the render-surface block far below; declared here because
// wglGetProcAddress is the ONLY place an extension entry point can be caught,
// and it sits at the top of the file.
static PROC GLSurfHookExtension(LPCSTR name, PROC real);
static bool PathBesideModule(const char* leaf, char* out, size_t outSize);
static bool Sha256File(const char* path, char out[65]);
static void LogVbogameBanner();   // armed runs only; defined after g_probeCfg

extern "C" PROC WINAPI i_wglGetProcAddress(LPCSTR name)
{
    typedef PROC (WINAPI *Fn)(LPCSTR);
    if (name && !strcmp(name, "wglSwapIntervalEXT"))
        return (PROC)&i_wglSwapIntervalEXT;
    PROC real = ((Fn)p_wglGetProcAddress)(name);
    PROC hooked = nullptr;
    if (name && real)
        hooked = GLSurfHookExtension(name, real);

    // Census of what the engine actually asks for. An engine requests each
    // entry point once at renderer init, so this is naturally bounded; the
    // cap is a backstop against some layer polling in a loop. This one log is
    // how the buffer-object question ("which upload path does CoD use?") gets
    // answered with evidence instead of a guess.
    static int s_getprocLogged;
    if (name && s_getprocLogged < 160) {
        ++s_getprocLogged;
        Log("getproc[%d]: %s -> %s", s_getprocLogged, name,
            hooked ? "WRAPPED" : (real ? "driver" : "null"));
    }
    return hooked ? hooked : real;
}

extern "C" void* CODVR_GetGLProc(const char* name)
{
    typedef PROC (WINAPI *GetProcFn)(LPCSTR);
    if (p_wglGetProcAddress) {
        PROC p = ((GetProcFn)p_wglGetProcAddress)(name);
        if (p) return (void*)p;
    }
    return g_realGL ? (void*)GetProcAddress(g_realGL, name) : nullptr;
}

// ---------------------------------------------------------------- logging

static void LogInit()
{
    char path[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                       | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&LogInit, &self);
    if (!GetModuleFileNameA(self, path, MAX_PATH))
        return;
    char* slash = strrchr(path, '\\');
    if (!slash)
        return;
    strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "codvr_result.log");

    // _fsopen with _SH_DENYWR, NOT fopen_s: fopen_s opens the file with
    // sharing denied entirely, which made codvr_result.log unreadable from
    // outside for as long as the game lived. That single fact blinded TWO
    // attended-run watchers on 2026-08-27 (13:42 and 15:53) whatever share
    // mode the reader asked for, and got two healthy runs killed. _SH_DENYWR
    // keeps the write exclusive but lets the harness read the log live.
    g_log = _fsopen(path, "w", _SH_DENYWR);
    if (!g_log) {
        char tmp[MAX_PATH];
        if (GetTempPathA(MAX_PATH, tmp)) {
            strcat_s(tmp, "codvr_result.log");
            g_log = _fsopen(tmp, "w", _SH_DENYWR);
            if (!g_log)
                return;
        }
    }
    InitializeCriticalSection(&g_logLock);
    g_logReady = true;
}

// Not static: vr.cpp logs through the same file and lock.
void Log(const char* fmt, ...)
{
    if (!g_logReady)
        return;
    EnterCriticalSection(&g_logLock);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);   // every line, so a crash still leaves the evidence
    LeaveCriticalSection(&g_logLock);
}

// ------------------------------------------------------------ safe memory

// VirtualQuery rather than SEH: no exception machinery on the syscall path,
// and it correctly rejects guard pages that IsBadReadPtr would wave through.
bool Readable(const void* p, size_t n)
{
    if (!p || (uintptr_t)p < 0x10000)
        return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)))
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return false;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
                   | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
                   | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & ok))
        return false;
    size_t avail = (size_t)mbi.BaseAddress + mbi.RegionSize - (size_t)p;
    return avail >= n;
}


// ---------------------------------------------------------- refdef layout
//
// The layout constants themselves live in codvr.h, since the probe needs them
// too. Only the routines that act on them are here.

// Scan for three consecutive float3s forming an orthonormal basis. Kept
// layout-agnostic so a different build of the module would still be found.
int FindOrthonormal(const BYTE* buf, size_t bytes)
{
    const float eps = 1e-3f;
    size_t limit = bytes >= 36 ? bytes - 36 : 0;
    for (size_t off = 0; off <= limit; off += 4) {
        const float* m = (const float*)(buf + off);
        bool finite = true;
        for (int i = 0; i < 9; ++i)
            if (!_finite(m[i]) || fabsf(m[i]) > 1.5f) { finite = false; break; }
        if (!finite)
            continue;

        float l0 = sqrtf(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
        float l1 = sqrtf(m[3]*m[3] + m[4]*m[4] + m[5]*m[5]);
        float l2 = sqrtf(m[6]*m[6] + m[7]*m[7] + m[8]*m[8]);
        if (fabsf(l0-1) > eps || fabsf(l1-1) > eps || fabsf(l2-1) > eps)
            continue;
        float d01 = m[0]*m[3] + m[1]*m[4] + m[2]*m[5];
        float d02 = m[0]*m[6] + m[1]*m[7] + m[2]*m[8];
        float d12 = m[3]*m[6] + m[4]*m[7] + m[5]*m[8];
        if (fabsf(d01) > eps || fabsf(d02) > eps || fabsf(d12) > eps)
            continue;
        if (m[0]==1 && m[4]==1 && m[8]==1 && !m[1] && !m[2]
            && !m[3] && !m[5] && !m[6] && !m[7])
            continue;                              // zeroed memory, not a camera
        return (int)off;
    }
    return -1;
}

// Gate for touching a struct: it must look like a viewport AND carry unit
// basis vectors. Stops us mangling a call that merely shares a syscall id.
bool IsRefdef(const void* p)
{
    if (!Readable(p, RD_MIN_BYTES))
        return false;
    const int* iv = (const int*)p;
    if (iv[RD_W] < 64 || iv[RD_W] > 16384 || iv[RD_H] < 64 || iv[RD_H] > 16384)
        return false;
    const float* m = (const float*)((const BYTE*)p + RD_AXIS_BYTES);
    float l0 = sqrtf(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
    float l1 = sqrtf(m[3]*m[3] + m[4]*m[4] + m[5]*m[5]);
    return fabsf(l0 - 1.0f) < 1e-3f && fabsf(l1 - 1.0f) < 1e-3f;
}

// --------------------------------------------------- cgame syscall hooking

typedef int (__cdecl *SyscallFn)(int, ...);

SyscallFn         g_realSyscall;   // shared with probe.cpp via codvr.h
static SyscallFn* g_syscallSlot;
static bool       g_cgameHooked;

static volatile LONG g_callsThisFrame[MAX_SYSCALL];
static LONG   g_framesSeen[MAX_SYSCALL];
static LONG   g_framesOnce[MAX_SYSCALL];
static int    g_lastArg[MAX_SYSCALL];
static BYTE   g_snap[MAX_SYSCALL][SNAP_BYTES];
static bool   g_snapValid[MAX_SYSCALL];
static LONG   g_frame;

enum { TRACE_MAX = 16384 };
static int           g_traceBuf[TRACE_MAX];
static volatile LONG g_traceCount;
static volatile LONG g_tracing;
static bool          g_traceDone;

// ------------------------------------------------------------ scene replay
//
// id Tech 3's RE_RenderScene renders entities [firstSceneEntity, numentities)
// then sets firstSceneEntity = numentities, so a *second* render in the same
// frame draws zero entities -- the "characters/weapon in one eye" bug. Fix:
// as the cgame issues each entity add, snapshot the refEntity it points at
// (the caller's pointer is transient, so we must copy the bytes now), then
// re-issue the captured adds between the two eye renders. Re-adding appends a
// fresh [numentities .. 2*numentities) batch that the second render consumes.
//
// Which syscall is AddRefEntityToScene is identified by the *shape* of its
// argument -- a refEntity_t carries an orthonormal 3x3 axis and a small reType
// int at offset 0 -- not by a hardcoded number. That also keeps replay
// crash-safe: RE_AddRefEntityToScene calls ri.Error(ERR_DROP) on an
// out-of-range reType, so we only replay structs whose reType is sane.
// Entity adds are NOT all one-argument calls. Static arity analysis of
// cgamex86.dll (tools/find_render_syscall.py) says syscall 37 takes 2 and
// syscall 38 takes 4. Replaying them with zeros in the remaining slots fed the
// engine null pointers it then dereferenced -- an instant crash to desktop on
// mission load. So capture the whole argument list and replay it verbatim,
// substituting only arg0 with our snapshot of the struct it pointed at.
enum { REPLAY_MAX = 2048, REPLAY_SNAP = 256, ENT_AXIS_OFF = 12, REPLAY_ARGS = 8,
       // CoD's scene-entity struct carries the SAME orientation twice: the
       // 3x3 axis at +12 and Quake-order euler angles at +56 (verified from a
       // live byte dump: +56 pitch 27.169 / +60 yaw -146.096 exactly matching
       // the axis rows). A rewrite must update both, so the touched span runs
       // through the angles.
       ENT_ANGLES_OFF = 56, ENT_REWRITE_SPAN = 72 };

// Scene building is a SEQUENCE, not a bag of independent entity adds. The
// captured frame shows a strict `34, 38, 34, 38, ...` alternation: 34 takes a
// single non-pointer argument and 38 takes four, the first of which points at
// the origin+axis struct. Replaying only the calls whose argument happened to
// be a readable pointer meant re-issuing every 38 while dropping every 34 that
// set up the state it consumes -- so the second eye still drew nothing.
//
// So capture EVERY call in the replay set, in order, with all its arguments,
// and snapshot the pointed-at struct only when arg0 actually is a pointer.
// Replay re-issues the whole sequence unchanged apart from that substitution.
struct CapturedCall {
    int  num;
    int  args[REPLAY_ARGS];
    bool hasSnap;
    BYTE snap[REPLAY_SNAP];
};
static CapturedCall   g_replay[REPLAY_MAX];
static volatile LONG  g_replayCount;
static bool           g_replayCapture;
static bool           g_replayEnabled = true;         // kill switch: codvr.cfg "replay 0"
static bool           g_isReplaySyscall[MAX_SYSCALL]; // candidate entity-add ids

// Is this a valid rotation basis? Unlike FindOrthonormal, the identity is
// ACCEPTED: an unrotated entity legitimately carries an identity axis, and
// rejecting it (as the refdef scan does, to skip zeroed memory) would throw
// away real entities.
static bool IsOrthonormal3x3(const float* m)
{
    const float eps = 1e-3f;
    for (int i = 0; i < 9; ++i)
        if (!_finite(m[i]) || fabsf(m[i]) > 1.5f)
            return false;
    const float l0 = sqrtf(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
    const float l1 = sqrtf(m[3]*m[3] + m[4]*m[4] + m[5]*m[5]);
    const float l2 = sqrtf(m[6]*m[6] + m[7]*m[7] + m[8]*m[8]);
    if (fabsf(l0-1) > eps || fabsf(l1-1) > eps || fabsf(l2-1) > eps)
        return false;
    const float d01 = m[0]*m[3] + m[1]*m[4] + m[2]*m[5];
    const float d02 = m[0]*m[6] + m[1]*m[7] + m[2]*m[8];
    const float d12 = m[3]*m[6] + m[4]*m[7] + m[5]*m[8];
    return fabsf(d01) <= eps && fabsf(d02) <= eps && fabsf(d12) <= eps;
}

// Does this snapshot look like a CoD scene entity we can safely re-add?
//
// Layout recovered from a captured byte dump (2026-08-17): float origin[3] at
// +0, orthonormal 3x3 axis at +12. This is NOT Quake 3's refEntity_t, which
// starts with an int reType -- the first classifier tested for that and so
// matched literally nothing. The origin in the dump was byte-identical to the
// player's eye position, which is what confirmed the field.
static bool ReplayIsEntity(const BYTE* snap)
{
    const float* org = (const float*)snap;
    for (int i = 0; i < 3; ++i)
        if (!_finite(org[i]) || fabsf(org[i]) > 1.0e6f)
            return false;
    return IsOrthonormal3x3((const float*)(snap + ENT_AXIS_OFF));
}

extern "C" void __cdecl NoteSyscall(int number, const int* args)
{
    if (number < 0 || number >= MAX_SYSCALL)
        return;
    const int arg0 = args[0];
    LONG n = InterlockedIncrement(&g_callsThisFrame[number]);
    g_lastArg[number] = arg0;

    if (g_tracing) {
        LONG i = InterlockedIncrement(&g_traceCount) - 1;
        if (i < TRACE_MAX)
            g_traceBuf[i] = number;
    }
    // Snapshot only the first call each frame: one syscall fires >10k times.
    if (n == 1) {
        const void* p = (const void*)(uintptr_t)arg0;
        if (Readable(p, SNAP_BYTES)) {
            memcpy(g_snap[number], p, SNAP_BYTES);
            g_snapValid[number] = true;
        } else {
            g_snapValid[number] = false;
        }
    }

    // Capture every entity-add candidate for replay to the second eye. The
    // arg is a pointer to a struct the caller owns transiently, so copy it
    // now; by the time we replay (at RenderScene) that memory may be reused.
    if (g_replayCapture && g_isReplaySyscall[number]) {
        LONG i = InterlockedIncrement(&g_replayCount) - 1;
        if (i >= 0 && i < REPLAY_MAX) {
            CapturedCall& c = g_replay[i];
            c.num = number;
            // Keep every argument. Slots past the call's real arity just pick
            // up adjacent stack, which the callee ignores -- but a slot the
            // callee DOES read must carry its original value.
            for (int k = 0; k < REPLAY_ARGS; ++k)
                c.args[k] = args[k];

            // Snapshot only when arg0 really is a pointer. State-setting calls
            // pass plain integers and are replayed as-is.
            const void* p = (const void*)(uintptr_t)arg0;
            c.hasSnap = Readable(p, REPLAY_SNAP);
            if (c.hasSnap)
                memcpy(c.snap, p, REPLAY_SNAP);
        }
    }
}



// The two syscalls that each receive the camera struct once per frame. Not
// static: the probe reads them, and the naked assembly interposer branches on
// them directly.
int g_candA = 71;
int g_candB = 243;

// 71 was established by disassembling cgamex86.dll: of the two candidates it
// is the only one whose call sites consistently clean up 8 bytes, i.e. take
// exactly one argument (the refdef). Overridable via codvr.cfg.
int  g_renderSyscall = 71;
static bool g_vrDrewStereo;     // did the render hook draw a 3D scene this frame?
static int  g_vrEye;            // which eye this frame renders; the pair
                                // machine in vr.cpp decides (VR_ExpectedEye)

static int  g_hudShiftPx = 12;  // codvr.cfg "hudshift"; 0 leaves it at infinity
LONG g_viewW, g_viewH;          // the engine's own render size

// ------------------------------------------------- GL pass classification
//
// Which pass the GL stream is in RIGHT NOW: the 3D scene, or the flat 2D
// overlay (HUD, menus). glViewport and glScissor need the answer to pick a
// mapping -- the scene must fill the render target untouched, while the 2D
// layer is reshaped into the 16:9 band and nudged per eye for convergence.
//
// The old scheme flipped a flag around the render SYSCALL. That was wrong in
// a way that survived three hardware runs: the syscall only QUEUES the
// scene, and the engine executes the whole frame's GL after the syscall
// returns -- so every scene glViewport executed in "HUD phase" and the WORLD
// was squeezed into the per-eye-shifted 16:9 band. The 2026-08-19 headset
// log proved it: at the first replaced projection the live viewport was
// 187,454 1440x810 -- exactly HudRemap(0,0,1440,1440) plus eye 0's
// principal shift -- which is the measured viewportCoverage/scissorCoverage
// FAIL, the black border around a squashed world, and ~16 deg of OPPOSITE
// lateral image shift per eye: a ~32 deg binocular divergence that reads as
// eyes half a metre apart.
//
// The truth lives in the GL stream itself:
//   * the render hook announces a queued scene (g_glScenePending);
//   * the scene's own GL begins with a PERSPECTIVE load on the projection
//     stack and 2D work begins with an ORTHO (or identity) load at stack
//     depth 0 -- a flare-style pushed ortho does NOT end the scene, because
//     the pop restores the scene projection;
//   * once the overlay has begun (perspective -> ortho edge at depth 0),
//     the scene never resumes within the frame, so a later perspective load
//     is some 2D element's own projection -- classified HUD and never
//     replaced with the eye frustum.
// On every phase change the last viewport/scissor the ENGINE asked for are
// re-issued through the new mapping, so whichever order the engine sets
// viewport and projection in, the state its draws actually see is right.
enum { PROJK_UNKNOWN = 0, PROJK_PERSP = 1, PROJK_ORTHO = 2 };
enum { PROJK_STACK_MAX = 8 };
static int  g_projKind0 = PROJK_UNKNOWN;   // kind at projection stack depth 0
static int  g_projStackDepth;              // engine pushes on GL_PROJECTION
static bool g_glScenePending;              // refdef queued; scene GL not begun
static bool g_glScene2DDone;               // overlay begun; scene over for good
static bool g_glScenePhaseApplied;         // classification last applied to GL
static int  g_lastRawViewport[4];          // engine coordinates, pre-mapping
static bool g_lastRawViewportValid;
static int  g_lastRawScissor[4];
static bool g_lastRawScissorValid;
static bool g_projNoteDone;                // optics-state note, once per window

static bool GLScenePhase()
{
    return g_glScenePending || (g_projKind0 == PROJK_PERSP && !g_glScene2DDone);
}

// Defined with the GL wrappers below (they own the mapping helpers).
// CODVR_SceneQueued flips the classification to "scene" the moment the
// render hook queues a 3D refdef -- before ANY of that frame's GL executes --
// so even an engine clear issued ahead of the first projection load runs
// against full-target scissor state.
static void CODVR_SceneQueued();
static void CODVR_GLFrameReset();

// Default set of syscalls to capture as entity-add candidates, taken from the
// pre-RenderScene bulk in the captured frame trace. Being generous here is
// safe: ReplayIsEntity gates what actually gets re-issued, so a non-entity id
// in this set is captured, logged, and skipped rather than replayed.
static void SetupReplaySet()
{
    // The observed scene-building sequence: 37 opens, then 34/38 alternate.
    // Deliberately narrow -- syscall 7 (166 pointer args/frame) proved NOT to
    // be an entity, and 61 takes no arguments at all, so replaying either is
    // pure risk. Extend with "replayAdd <n>" in codvr.cfg if a frame trace
    // shows something missing.
    static const int kDefault[] = { 34, 37, 38 };
    for (size_t i = 0; i < sizeof(kDefault)/sizeof(kDefault[0]); ++i)
        g_isReplaySyscall[kDefault[i]] = true;
    // Capture in BOTH modes. It is passive -- only the VR path ever re-issues
    // anything -- and capturing in probe mode means the entity-struct question
    // can be answered by a cheap flat run instead of a headset session.
    g_replayCapture = true;
}

static void DumpCandidateStruct(int num, const BYTE* snap);   // defined below
static void LogSceneGeometry(const int* iv, const float* fv, const char* when);

// One-shot report on what the capture actually collected. Deliberately does
// NOT require VR to be active: it is called from the render hook on any path,
// so the whole question of which syscall carries entities can be answered by
// running the game FLAT on the monitor -- no headset session needed.
static void ReportCaptureOnce()
{
    static bool logged = false;
    LONG n = g_replayCount;
    if (logged || n <= 0)
        return;
    logged = true;
    if (n > REPLAY_MAX) n = REPLAY_MAX;

    LONG seen[MAX_SYSCALL] = {0};
    LONG kept[MAX_SYSCALL] = {0};
    LONG ptrs[MAX_SYSCALL] = {0};
    int  entityShaped = 0;
    for (LONG i = 0; i < n; ++i) {
        const CapturedCall& c = g_replay[i];
        if (c.num < 0 || c.num >= MAX_SYSCALL) continue;
        ++seen[c.num];
        if (c.hasSnap) ++ptrs[c.num];
        if (c.hasSnap && ReplayIsEntity(c.snap)) { ++kept[c.num]; ++entityShaped; }
    }

    Log("");
    Log("======= SCENE CAPTURE (one scene) =======");
    Log("  %ld calls captured, %d carrying an entity struct", n, entityShaped);
    Log("  (ALL of these are replayed in order, entity-shaped or not, so that");
    Log("   state-setting calls still precede the calls that consume them)");
    for (int s = 0; s < MAX_SYSCALL; ++s) {
        if (seen[s] == 0) continue;
        Log("  syscall %3d: %ld calls, %ld with a pointer arg, %ld entity-shaped",
            s, seen[s], ptrs[s], kept[s]);
    }
    // Nothing matched => the layout assumption is wrong. Dump raw bytes so the
    // real layout can be read off the numbers instead of guessed at.
    if (entityShaped == 0) {
        Log("");
        Log("  NOTHING looked like a Quake 3 entity. Raw bytes of the first");
        Log("  capture of each candidate follow; the real layout is in here.");
        bool dumped[MAX_SYSCALL] = {false};
        for (LONG i = 0; i < n; ++i) {
            int s = g_replay[i].num;
            if (s < 0 || s >= MAX_SYSCALL || dumped[s] || !g_replay[i].hasSnap)
                continue;
            dumped[s] = true;
            DumpCandidateStruct(s, g_replay[i].snap);
        }
    }
    Log("========================================");
}

// The raw re-issue, kept free of C++ objects so it can sit inside __try.
// Replays the captured sequence IN ORDER so state-setting calls still precede
// the calls that consume them.
static int ReplayInner(LONG n)
{
    int issued = 0;
    for (LONG i = 0; i < n; ++i) {
        const CapturedCall& c = g_replay[i];
        if (c.num < 0 || c.num >= MAX_SYSCALL)
            continue;
        // arg0 becomes our snapshot when we took one; otherwise the original
        // integer. Every other argument is passed exactly as the game did.
        void* a0 = c.hasSnap ? (void*)c.snap : (void*)(uintptr_t)c.args[0];
        g_realSyscall(c.num, a0,
                      c.args[1], c.args[2], c.args[3],
                      c.args[4], c.args[5], c.args[6]);
        ++issued;
    }
    return issued;
}

// Re-issue this scene's captured entity adds so the second eye's RenderScene
// has a scene to draw. Called between the two eye renders.
//
// Guarded, because this calls into engine internals with arguments we
// reconstructed: getting them wrong once already crashed the game to desktop
// on mission load. If it faults, switch replay off and keep playing -- a
// one-eyed weapon is a far better outcome than losing the session, and the log
// then says exactly what happened.
static void ReplayCapturedEntities()
{
    if (!g_replayEnabled)
        return;

    LONG n = g_replayCount;
    if (n > REPLAY_MAX) n = REPLAY_MAX;

    __try {
        int issued = ReplayInner(n);
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("VR replay: re-issued %d of %ld captured calls to the second eye",
                issued, n);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_replayEnabled = false;
        Log("VR replay: FAULTED (code 0x%08lX) re-issuing entities -- replay is",
            (unsigned long)GetExceptionCode());
        Log("  now DISABLED for this session; the game keeps running with the");
        Log("  weapon/soldiers in one eye. Captured %ld, syscalls involved:", n);
        bool noted[MAX_SYSCALL] = {false};
        for (LONG i = 0; i < n && i < REPLAY_MAX; ++i) {
            int s = g_replay[i].num;
            if (s >= 0 && s < MAX_SYSCALL && !noted[s]) {
                noted[s] = true;
                Log("    syscall %d", s);
            }
        }
    }
}

// One-shot dump of the geometry the engine actually uses. The per-eye viewport
// writes assume the refdef rect and the GL back buffer share a coordinate
// space and a size; if they do not, the blit reads the wrong region and the
// headset shows part of the frame or none of it. Logged from the render hook,
// so these are the real values for a real scene, not assumptions.
static void LogSceneGeometry(const int* iv, const float* fv, const char* when)
{
    typedef void (WINAPI *GetIntFn)(unsigned, int*);
    enum { GL_VIEWPORT_ = 0x0BA2, GL_SCISSOR_BOX_ = 0x0C10 };

    Log("  [%s] refdef x=%d y=%d w=%d h=%d  fov %.2f x %.2f",
        when, iv[RD_X], iv[RD_Y], iv[RD_W], iv[RD_H], fv[RD_FOVX], fv[RD_FOVY]);

    GetIntFn getInt = (GetIntFn)p_glGetIntegerv;
    if (getInt) {
        int vp[4] = {0,0,0,0}, sc[4] = {0,0,0,0};
        getInt(GL_VIEWPORT_, vp);
        getInt(GL_SCISSOR_BOX_, sc);
        Log("  [%s] GL viewport %d,%d %dx%d   scissor %d,%d %dx%d",
            when, vp[0], vp[1], vp[2], vp[3], sc[0], sc[1], sc[2], sc[3]);
    }
}

// The replay classifier matched nothing, which means CoD's entity struct is
// NOT laid out like Quake 3's. Guessing another offset would cost another play
// session, so dump the raw bytes of the first capture of each candidate and
// read the layout off the numbers instead.
static void DumpCandidateStruct(int num, const BYTE* snap)
{
    Log("  --- syscall %d, first argument, 64 bytes ---", num);
    for (int off = 0; off < 64; off += 16) {
        const int*   iv = (const int*)(snap + off);
        const float* fv = (const float*)(snap + off);
        Log("    +%02d  int %11d %11d %11d %11d", off, iv[0], iv[1], iv[2], iv[3]);
        Log("         flt %11.3f %11.3f %11.3f %11.3f", fv[0], fv[1], fv[2], fv[3]);
    }
    int orth = FindOrthonormal(snap, REPLAY_SNAP);
    Log("    orthonormal 3x3 found at byte offset: %d", orth);
}

// Clears the scene capture once a render call finishes, whichever path it
// took. Captures are per-SCENE, not per-frame: the engine can render more than
// one scene in a frame (portal/skybox, or a separate viewmodel pass), and the
// second scene's replay must not re-add the first scene's entities.
struct SceneCaptureScope {
    bool active;
    explicit SceneCaptureScope(bool a) : active(a) {}
    ~SceneCaptureScope() { if (active) InterlockedExchange(&g_replayCount, 0); }
};

// ============================================================================
// RETIRED_INVALID_37_38_WEAPON_PATH
// ============================================================================
//
// Syscalls 37 and 38 are NOT entity adds. The completed contradiction audit
// identified them as:
//
//     37 = CM_PointContents
//     38 = CM_TransformedPointContents
//
// and the pointer argument they carry as `&cg.refdef.vieworg`. The static
// analysis that once read them as AddRefEntityWithAttachments was wrong: the
// struct being handed around is the CAMERA, not the view weapon.
//
// The old forwarder therefore treated the refdef as an entity --
//
//     org  = (float*)p            ==  cg.refdef.vieworg
//     axis = (float*)(p + 12)     ==  cg.refdef.viewaxis
//
// -- and both gates that were meant to reject a non-entity were structurally
// incapable of firing:
//
//   * IsOrthonormal3x3(p+12) reads the real viewaxis, orthonormal by
//     construction, so the shape gate ALWAYS passed. The 2026-08-23 hardware
//     log records 100% of 37/38 calls as "entity-shaped" (2 of 2, 12 of 12).
//   * ClassifyViewWeaponEntity measures `entOrigin - g_gameVieworg`, but
//     entOrigin IS the camera, so the distance was always 0 -> class "near"
//     -> always "the view weapon". The same log records the tell verbatim:
//     "entity census: ... nearest 0.0 u ... rewritten 7696".
//
// The result was 7696 WRITE-THROUGH rewrites of cg.refdef.vieworg/viewaxis
// with the weapon pose -- a camera corruption path wearing a weapon feature's
// name, and a direct contributor to the physically intolerable stereo
// behaviour of that run.
//
// This is an architecture error, not a tuning error. It is NOT fixed by
// lowering a threshold, improving the classifier, or hiding it behind the
// weapon-transform toggle. Syscalls 37/38 are now semantically transparent
// collision/content queries: nothing diverts them, and no weapon origin,
// weapon axis, controller pose, mouse aim or body yaw can write through them.
//
// The implementation below is retained ONLY as the documented record of what
// was removed. It has ZERO production callers -- SetupWeaponDivertSet no
// longer flags 37 or 38, and the naked hook's divert branch is gone -- and it
// is compiled out entirely. Physical weapon control returns via the DObj bone
// controller (a real, writable per-bone API reachable by syscall), not here.
#define RETIRED_INVALID_37_38_WEAPON_PATH 0

// The invariant counter itself lives in vr.cpp, so that BOTH test binaries
// (the DLL suite and the mocked-OpenXR safe suite, which does not link this
// file) see the same counter the stereo fail-safe reads.
static unsigned char g_entDivert[MAX_SYSCALL];

static void SetupWeaponDivertSet()
{
    // DELIBERATELY EMPTY for 37/38. See RETIRED_INVALID_37_38_WEAPON_PATH
    // above: those are CM_PointContents / CM_TransformedPointContents on
    // &cg.refdef.vieworg, and diverting them rewrites the CAMERA.
    //
    // The array itself stays (the self-test drives a synthetic id through it
    // to prove the divert machinery is still wired), but no production
    // syscall is flagged here.
}

#if RETIRED_INVALID_37_38_WEAPON_PATH
static bool          g_entRewriteFaulted;

// RETIRED -- NOT COMPILED, ZERO CALLERS. Preserved verbatim as the record of
// the removed camera-corruption path. Do not re-enable.
extern "C" int __cdecl EntityForward(int num, void* p, int a2, int a3, int a4,
                                     int a5, int a6, int a7)
{
    BYTE saved[ENT_REWRITE_SPAN];
    int  wrote = 0;
    __try {
        if (!g_entRewriteFaulted && Readable(p, ENT_REWRITE_SPAN)) {
            float* org    = (float*)p;
            float* axis   = (float*)((BYTE*)p + ENT_AXIS_OFF);
            float* angles = (float*)((BYTE*)p + ENT_ANGLES_OFF);
            if (IsOrthonormal3x3(axis)) {
                memcpy(saved, p, ENT_REWRITE_SPAN);
                wrote = VRWeapon_MaybeRewriteEntity(org, axis, angles,
                                                    (unsigned)(uintptr_t)p);
            }
        }
        return g_realSyscall(num, p, a2, a3, a4, a5, a6, a7);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_entRewriteFaulted = true;
        VRWeapon_SetTransformEnabled(0);
        Log("VR weapon: entity rewrite FAULTED (code 0x%08lX) -- weapon "
            "transform disabled for this session",
            (unsigned long)GetExceptionCode());
        if (wrote)
            memcpy(p, saved, ENT_REWRITE_SPAN);   // leave cgame's struct sane
        return 0;
    }
}
#endif  // RETIRED_INVALID_37_38_WEAPON_PATH

// Forwarding takes eight slots even though the call is believed to be
// (number, refdef). Under cdecl the caller cleans the stack, so passing more
// arguments than the callee reads is harmless, while passing fewer than it
// reads would corrupt it. Reading past the caller's arguments only picks up
// adjacent committed stack, which the callee ignores.
extern "C" int __cdecl SplitRenderAndForward(int num, void* p,
                                             int a2, int a3, int a4,
                                             int a5, int a6, int a7)
{
    if (!IsRefdef(p))
        return g_realSyscall(num, p, a2, a3, a4, a5, a6, a7);

    int*   iv = (int*)p;
    float* fv = (float*)p;

    SceneCaptureScope captureScope(num == g_renderSyscall);

    InterlockedExchange(&g_viewW, iv[RD_W]);
    InterlockedExchange(&g_viewH, iv[RD_H]);

    // Report the capture and the true scene geometry on the first render call,
    // whatever mode we are in. Running flat on the monitor therefore answers
    // both open questions without spending a headset session.
    if (num == g_renderSyscall) {
        // Tell the surface layer a 3D scene is starting, in EVERY mode. The
        // whole point of the flat runs is that draw classification is proved
        // on a monitor, so this must not be inside the VR-only branch.
        GLSurf_SceneQueued(VR_Active() ? g_vrEye : 0);
        // SEAL the pair-latched object set the moment an eye consumes it: a
        // hand that starts tracking between the eyes must not change what
        // eye 1 renders against. (VRObj_NoteEyeConsumed marks every slot
        // sampled and records the serial for the stereo consistency check.)
        if (VR_Active())
            VRObj_NoteEyeConsumed(g_vrEye);
        static bool flatGeomLogged = false;
        if (!flatGeomLogged) {
            flatGeomLogged = true;
            // A marker the flat harness can see. `spmap` lands on the mission
            // briefing menu and waits for a key, so the harness taps keys until
            // gameplay starts -- and it must STOP the moment it does, or it
            // would keep making the player jump and ruin the A/B determinism
            // the screenshot pair depends on.
            {
                char mk[MAX_PATH];
                if (PathBesideModule("codvr_ingame.marker", mk, sizeof(mk))) {
                    FILE* mf = nullptr;
                    if (fopen_s(&mf, mk, "w") == 0 && mf) {
                        fputs("in game", mf);
                        fclose(mf);
                    }
                }
            }
            Log("");
            Log("=== FIRST 3D SCENE ===");
            Log("======= SCENE GEOMETRY (first scene) =======");
            LogSceneGeometry(iv, fv, "game's own");
            Log("===========================================");
            LogVbogameBanner();
        }
        ReportCaptureOnce();
    }

    // ---- VR path: render the scene once per eye, side by side ----
    // The engine consumes the entity list on the first RenderScene, so before
    // the second eye we replay the captured entity adds to refill it. The
    // world (BSP) needs no replay -- it is re-walked from the refdef each call;
    // only entities (soldiers, the weapon) are consumed, which is exactly the
    // "characters in one eye" symptom.
    if (!Probe_Enabled() && VR_Active() && num == g_renderSyscall) {
        const int   sx = iv[RD_X], sy = iv[RD_Y], sw = iv[RD_W], sh = iv[RD_H];
        const float ofx = fv[RD_FOVX], ofy = fv[RD_FOVY];
        float gameOrigin[3] = { fv[RD_ORG], fv[RD_ORG+1], fv[RD_ORG+2] };
        float gameAxis[9];
        memcpy(gameAxis, (BYTE*)p + RD_AXIS_BYTES, sizeof(gameAxis));

        // One-shot geometry report. "before" is what the game asked for;
        // "after" is what the engine left set once it had rendered our
        // half-width viewport. If "after" does not match what we wrote, our
        // camera/viewport writes are not reaching the renderer -- which would
        // explain a head-locked image far better than any camera-math error.
        static bool geomLogged = false;
        const bool logGeom = !geomLogged;
        if (logGeom) {
            geomLogged = true;
            Log("");
            Log("=== SCENE GEOMETRY (first stereo scene) ===");
            Log("  back buffer believed to be %ldx%ld", g_viewW, g_viewH);
            LogSceneGeometry(iv, fv, "before");
        }

        // The REAL aim (before any substitution) feeds the steering loop and
        // the view-weapon classifier: the game's view angles ARE the weapon.
        VRWeapon_NoteGameAim(gameAxis, gameOrigin, CODVR_NowSeconds());

        // ONE eye per frame, across the whole back buffer.
        //
        // Rendering both eyes in a single frame required replaying the scene
        // for the second one, because the engine hands its entity list to the
        // first render and empties it -- and reconstructing that replay from
        // outside the engine kept failing (and once crashed the game). So
        // every game frame renders a complete, ordinary scene for one eye;
        // the pair machine in vr.cpp spans ONE OpenXR frame across TWO such
        // game frames (eye 0 then eye 1 against the same located views, with
        // no xrWaitFrame between them), so both eyes of every display frame
        // are freshly rendered instead of one being a frame stale. Each eye
        // also keeps the full back-buffer width and a whole 2D/HUD pass.
        //
        // HEAD/WEAPON DECOUPLING: while the controller aim model is live, the
        // camera is composed on the virtual BODY basis instead of the game's
        // aim -- the head owns the view completely, and the game's own view
        // angles (steered to the controller) stay the weapon. With no
        // controllers the base falls back to the game's aim: byte-identical
        // to the baseline build.
        const int eye = g_vrEye;
        float org[3], axis[9], fovX, fovY;
        float bodyBase[9];
        const float* camBase = gameAxis;
        if (VRWeapon_GetViewBasisRows(bodyBase))
            camBase = bodyBase;
        VR_GetEyeCamera(eye, gameOrigin, camBase, sw, sh, org, axis, &fovX, &fovY);

        fv[RD_FOVX] = fovX;
        fv[RD_FOVY] = fovY;
        fv[RD_ORG] = org[0]; fv[RD_ORG+1] = org[1]; fv[RD_ORG+2] = org[2];
        memcpy((BYTE*)p + RD_AXIS_BYTES, axis, sizeof(axis));

        // Announce the queued scene to the GL classifier. The engine executes
        // this frame's GL AFTER this syscall returns; from here until the 2D
        // overlay's ortho load, viewport/scissor calls belong to the scene
        // and must NOT be reshaped -- the eye's aspect is entirely in the
        // projection matrix the interception installs.
        CODVR_SceneQueued();
        int rv = g_realSyscall(num, p, a2, a3, a4, a5, a6, a7);

        if (logGeom) {
            Log("  this frame renders eye %d full-size: x=%d w=%d  fov %.2f x %.2f",
                eye, sx, sw, fovX, fovY);
            LogSceneGeometry(iv, fv, "after render");
            Log("==========================================");
        }

        iv[RD_X] = sx; iv[RD_Y] = sy; iv[RD_W] = sw; iv[RD_H] = sh;
        fv[RD_FOVX] = ofx; fv[RD_FOVY] = ofy;
        fv[RD_ORG] = gameOrigin[0]; fv[RD_ORG+1] = gameOrigin[1]; fv[RD_ORG+2] = gameOrigin[2];
        memcpy((BYTE*)p + RD_AXIS_BYTES, gameAxis, sizeof(gameAxis));
        g_vrDrewStereo = true;
        return rv;                          // captureScope clears the capture
    }

    // The diagnostic owns the call entirely when it is running (probe.cpp).
    int probeRet = 0;
    if (Probe_RenderHook(num, p, a2, a3, a4, a5, a6, a7, &probeRet))
        return probeRet;

    return g_realSyscall(num, p, a2, a3, a4, a5, a6, a7);
}

static __declspec(naked) int __cdecl SyscallHook()
{
    __asm {
        pushad
        lea  eax, [esp + 32 + 8]      // &varargs[0], not just the first one
        push eax
        mov  eax, [esp + 32 + 4 + 4]  // syscall number
        push eax
        call NoteSyscall
        add  esp, 8
        popad

        mov  eax, [esp + 4]
        cmp  eax, dword ptr [g_candA]
        je   divert
        cmp  eax, dword ptr [g_candB]
        je   divert
        // The entity-add divert branch is GONE. See
        // RETIRED_INVALID_37_38_WEAPON_PATH: 37/38 are CM_PointContents /
        // CM_TransformedPointContents on &cg.refdef.vieworg, so diverting
        // them rewrote the camera. Every syscall that is not a render
        // candidate now reaches the engine untouched -- 37 and 38 included.
        cmp  eax, MAX_SYSCALL
        jae  forward
        cmp  byte ptr [g_entDivert + eax], 0
        jne  divertEnt
    forward:
        jmp  dword ptr [g_realSyscall]
    divert:
        // Tail-jump: SplitRenderAndForward is cdecl and inherits this exact
        // frame, so [esp+4]=number and [esp+8]=refdef land where expected.
        jmp  SplitRenderAndForward
    divertEnt:
        // Reachable only by the self-test's synthetic id (36); the production
        // divert set is empty. Transparent pass-through: no struct is read,
        // no camera byte is written.
        jmp  dword ptr [g_realSyscall]
    }
}

// dllEntry is always: mov eax,[esp+4] / mov [global],eax / ret. Recovering
// `global` from the bytes means never hardcoding an address -- which already
// paid off, as the module loaded at two different bases across runs.
static SyscallFn* FindSyscallSlot(HMODULE cgame)
{
    FARPROC entry = GetProcAddress(cgame, "dllEntry");
    if (!entry) {
        Log("cgame: no dllEntry export");
        return nullptr;
    }
    const BYTE* p = (const BYTE*)entry;
    if (!(p[0]==0x8B && p[1]==0x44 && p[2]==0x24 && p[3]==0x04 && p[4]==0xA3)) {
        Log("cgame: dllEntry prologue mismatch: %02X %02X %02X %02X %02X",
            p[0], p[1], p[2], p[3], p[4]);
        return nullptr;
    }
    return *(SyscallFn**)(p + 5);
}

// ---- cgame ADS/zoom hint --------------------------------------------------
// CoD1's weapalt (ADS) is a toggle whose true state the injection layer could
// only ASSUME -- and an assumed toggle drifts across loads/deaths/switches.
// Static analysis found a readable ground-truth-ish signal: the weapon-FOV
// function at cgame RVA 0x25200 interpolates by a float at RVA 0x11E15C that
// is exactly 0.0 at hip-fire and nonzero while the ADS zoom is engaged or
// transitioning. Reading it is fully guarded; a wrong game build or an
// unmapped page just reports "no hint" and the toggle bookkeeping stands.
static DWORD g_cgameAdsHintRva = 0x11E15C;   // codvr.cfg "cgameAdsHintRva", 0 off

extern "C" int CODVR_GetCgameAdsHint(float* value)
{
    if (!g_cgameAdsHintRva || !value)
        return 0;
    HMODULE cg = GetModuleHandleA("cgamex86.dll");
    if (!cg)
        return 0;
    __try {
        const float v = *(const float*)((BYTE*)cg + g_cgameAdsHintRva);
        if (!_finite(v) || v < -1000.0f || v > 1000.0f)
            return 0;
        *value = v;
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// ---- two-hand ADS random-dispersion reduction ---------------------------
// CoD keeps deterministic view kick/recoil separate from weaponDef.adsSpread.
// For the retail 1.5 binaries below, cgame exposes the player's current
// weapon index and gamex86 owns the corresponding weaponDef table.  Touch only
// that one weapon's adsSpread, only while the native support hand is attached
// and cgame says ADS is actually engaged.  Every pointer and binary identity
// is checked; an unknown build simply retains stock behaviour.
static const DWORD  CODVR_GAME_STAMP            = 0x40443FB0;
static const DWORD  CODVR_GAME_IMAGE_SIZE       = 0x002EC000;
static const DWORD  CODVR_CGAME_STAMP           = 0x40443FB6;
static const DWORD  CODVR_CGAME_IMAGE_SIZE      = 0x00274000;
static const DWORD  CODVR_GAME_WEAPON_COUNT_RVA = 0x0012564C;
static const DWORD  CODVR_GAME_WEAPON_TABLE_RVA = 0x00125650;
static const DWORD  CODVR_CGAME_WEAPON_INDEX_RVA= 0x0011E154;
static const size_t CODVR_WEAPON_ADS_SPREAD_OFF = 0x38C;
static const float  CODVR_TWO_HAND_ADS_SCALE    = 0.25f;

static float* g_twoHandAdsSpreadAddress;
static float  g_twoHandAdsSpreadOriginal;
static bool   g_twoHandAdsSpreadActive;
static bool   g_twoHandAdsLogged;
static bool   g_twoHandAdsBuildWarningLogged;

static float TwoHandAdsScaledSpread(float original)
{
    return original * CODVR_TWO_HAND_ADS_SCALE;
}

static bool TwoHandAdsCondition(int twoHandState, int hintValid, float adsHint)
{
    return twoHandState == VRNH_2H_ATTACHED && hintValid
        && _finite(adsHint) && fabsf(adsHint) > 0.0001f;
}

static bool WritableFloat(float* p)
{
    if (!Readable(p, sizeof(*p)))
        return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)))
        return false;
    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY
                         | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        && (mbi.Protect & writable) != 0;
}

static bool RetailModuleIdentity(HMODULE module, DWORD stamp, DWORD imageSize)
{
    if (!module || !Readable(module, sizeof(IMAGE_DOS_HEADER)))
        return false;
    __try {
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)module;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
            return false;
        const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const BYTE*)module + dos->e_lfanew);
        return Readable(nt, sizeof(*nt)) && nt->Signature == IMAGE_NT_SIGNATURE
            && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386
            && nt->FileHeader.TimeDateStamp == stamp
            && nt->OptionalHeader.SizeOfImage == imageSize;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void RestoreTwoHandAdsSpread()
{
    if (!g_twoHandAdsSpreadActive)
        return;
    __try {
        if (WritableFloat(g_twoHandAdsSpreadAddress))
            *g_twoHandAdsSpreadAddress = g_twoHandAdsSpreadOriginal;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // A module/map teardown can invalidate the prior weaponDef between
        // frames.  There is nothing safe to restore in that case.
    }
    g_twoHandAdsSpreadAddress = nullptr;
    g_twoHandAdsSpreadOriginal = 0.0f;
    g_twoHandAdsSpreadActive = false;
}

static void UpdateTwoHandAdsSpread()
{
    VRNHDebug hands = {};
    VRNH_GetDebug(&hands);
    float adsHint = 0.0f;
    const int hintValid = CODVR_GetCgameAdsHint(&adsHint);
    if (!hands.enabled || !TwoHandAdsCondition(hands.gun.twoHandState, hintValid, adsHint)) {
        RestoreTwoHandAdsSpread();
        return;
    }

    HMODULE game = GetModuleHandleA("gamex86.dll");
    HMODULE cgame = GetModuleHandleA("cgamex86.dll");
    if (!RetailModuleIdentity(game, CODVR_GAME_STAMP, CODVR_GAME_IMAGE_SIZE)
        || !RetailModuleIdentity(cgame, CODVR_CGAME_STAMP, CODVR_CGAME_IMAGE_SIZE)) {
        RestoreTwoHandAdsSpread();
        if (!g_twoHandAdsBuildWarningLogged) {
            g_twoHandAdsBuildWarningLogged = true;
            Log("accuracy: retail module identity mismatch; two-hand ADS spread left unchanged");
        }
        return;
    }

    __try {
        const int weaponIndex = *(const int*)((const BYTE*)cgame + CODVR_CGAME_WEAPON_INDEX_RVA);
        const int weaponCount = *(const int*)((const BYTE*)game + CODVR_GAME_WEAPON_COUNT_RVA);
        void** weaponTable = *(void***)((const BYTE*)game + CODVR_GAME_WEAPON_TABLE_RVA);
        if (weaponIndex <= 0 || weaponCount <= 0 || weaponCount > 256
            || weaponIndex > weaponCount || !Readable(weaponTable, (weaponCount + 1) * sizeof(void*))) {
            RestoreTwoHandAdsSpread();
            return;
        }
        BYTE* weaponDef = (BYTE*)weaponTable[weaponIndex];
        float* spread = weaponDef ? (float*)(weaponDef + CODVR_WEAPON_ADS_SPREAD_OFF) : nullptr;
        if (!WritableFloat(spread)) {
            RestoreTwoHandAdsSpread();
            return;
        }

        if (g_twoHandAdsSpreadActive && g_twoHandAdsSpreadAddress != spread)
            RestoreTwoHandAdsSpread();
        if (!g_twoHandAdsSpreadActive) {
            const float original = *spread;
            if (!_finite(original) || original < 0.0f || original > 45.0f)
                return;
            g_twoHandAdsSpreadAddress = spread;
            g_twoHandAdsSpreadOriginal = original;
            g_twoHandAdsSpreadActive = true;
            if (!g_twoHandAdsLogged) {
                g_twoHandAdsLogged = true;
                Log("accuracy: two-hand ADS weapon %d random spread %.3f -> %.3f (recoil unchanged)",
                    weaponIndex, original, TwoHandAdsScaledSpread(original));
            }
        }
        *spread = TwoHandAdsScaledSpread(g_twoHandAdsSpreadOriginal);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        RestoreTwoHandAdsSpread();
    }
}

static void TryHookCgame()
{
    if (g_cgameHooked)
        return;
    HMODULE cgame = GetModuleHandleA("cgamex86.dll");
    if (!cgame)
        return;
    SyscallFn* slot = FindSyscallSlot(cgame);
    if (!slot || !*slot)
        return;

    DWORD old;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) {
        Log("cgame: VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    g_realSyscall = *slot;
    g_syscallSlot = slot;
    *slot = (SyscallFn)SyscallHook;
    VirtualProtect(slot, sizeof(*slot), old, &old);

    g_cgameHooked = true;
    Log("cgame @ %p hooked (slot %p, engine syscall %p)", cgame, slot, g_realSyscall);
    Log("experiment starts on the first rendered scene; ~30s of play needed");
}


// ---------------------------------------------------------------- tracing

static void DumpTrace()
{
    LONG n = g_traceCount;
    if (n > TRACE_MAX) n = TRACE_MAX;
    Log("");
    Log("======= SINGLE-FRAME CALL ORDER (%ld calls) =======", n);
    int step = 0;
    for (LONG i = 0; i < n; ) {
        int v = g_traceBuf[i];
        LONG j = i;
        while (j < n && g_traceBuf[j] == v) ++j;
        LONG run = j - i;
        const char* tag = "";
        if (v == g_candA)      tag = "   <=== candidate A (71)";
        else if (v == g_candB) tag = "   <=== candidate B (243)";
        else if (run > 20)     tag = "   (bulk: scene building)";
        Log("  [%3d] syscall %3d x%-6ld%s", step++, v, run, tag);
        i = j;
    }
    Log("  (in id Tech 3 the render call follows the bulk entity adds)");
    Log("======= end call order =======");
}

// ------------------------------------------------------- intercepted wgl

// The engine issues screen coordinates for its own resolution. When rendering
// is redirected into the supersampled target, every rectangle it sets has to be
// scaled by the same factor or it would draw into a corner of a much larger
// buffer. Scaling both of these uniformly keeps geometry and the 2D pass
// correct while every pixel simply gets larger.
// The render buffer is square (it matches the headset's nearly-square eye and
// spends no pixels on detail the eye cannot use), but the engine's 2D layer is
// laid out for a wide screen. Drawn straight onto a square buffer the HUD comes
// out stretched vertically -- ammo counts and the compass visibly taller than
// they should be.
//
// So during the 2D pass the engine's screen rectangle is remapped into a
// centred band of the intended aspect. The engine still believes it is drawing
// to the full screen; its output simply lands in correctly-proportioned space.
// Setting hudAspect to 0 turns this off and draws the HUD stretched to fill.
static float g_hudAspect = 1.7777f;     // codvr.cfg "hudAspect"

// The weapon's own trim (codvr.cfg "vr_gun_off_*" / "vr_gun_pitch|yaw|roll",
// and the VR Options overlay), metres and degrees.
static float g_cfgGunCalPos[3];
static float g_cfgGunCalRot[3];

// GAMEPLAY HUD safe area (codvr.cfg "vr_hud_scale"). The full-width band puts
// HUD corners (compass, ammo) at the extreme temporal angles where the lens
// runs out, so during 3D frames the band shrinks toward the centre by this
// factor. 0.88 still demanded eye gymnastics on hardware (2026-08-20: "cannot
// comfortably look far enough down-left/down-right"); 0.70 was judged "much
// better but could be slightly easier to see", so the default grows a touch
// to 0.73 -- bigger text, corners still well inside comfortable eye travel.
// Menus are NOT scaled here; they have their own angular fit at capture time.
static float g_hudScale = 0.73f;

// Whole-band nudge as a fraction of the screen (codvr.cfg
// "vr_hud_horizontal_offset" / "vr_hud_vertical_offset"): positive moves the
// HUD right / down in screen terms. 3D frames only, like the scale.
static float g_hudOffsetX = 0.0f;
static float g_hudOffsetY = 0.0f;

static void HudRemap(int* x, int* y, int* w, int* h)
{
    const int sw = (int)g_viewW, sh = (int)g_viewH;
    if (g_hudAspect <= 0.0f || sw < 2 || sh < 2)
        return;
    const float screenAspect = (float)sw / (float)sh;
    if (fabsf(screenAspect - g_hudAspect) < 0.01f)
        return;                          // already the right shape

    float bw = (float)sw, bh = (float)sh;
    if (screenAspect > g_hudAspect) bw = bh * g_hudAspect;   // pillarbox
    else                            bh = bw / g_hudAspect;   // letterbox
    float nudgeX = 0.0f, nudgeY = 0.0f;
    if (g_vrDrewStereo) {
        // vr_hud_scale times the VR UI safe area (P3, 2026-09-04): the band
        // fits inside the per-axis safe bounds with its aspect kept, so the
        // uniform factor is the smaller of the two.
        const float ux = VR_UiInsetX(), uy = VR_UiInsetY();
        float f = (g_hudScale > 0.0f && g_hudScale < 1.0f) ? g_hudScale : 1.0f;
        f *= (ux < uy) ? ux : uy;
        if (f > 0.0f && f < 1.0f) {
            bw *= f;
            bh *= f;
        }
        nudgeX = g_hudOffsetX * (float)sw;
        nudgeY = g_hudOffsetY * (float)sh;
    }
    const float offX = (sw - bw) * 0.5f + nudgeX;
    const float offY = (sh - bh) * 0.5f + nudgeY;
    const float scaleX = bw / (float)sw, scaleY = bh / (float)sh;

    *x = (int)(offX + *x * scaleX + 0.5f);
    *y = (int)(offY + *y * scaleY + 0.5f);
    *w = (int)(*w * scaleX + 0.5f);
    *h = (int)(*h * scaleY + 0.5f);
}

// Per-eye offset for the flat 2D overlay, in the game's own pixels (the
// supersample scale is applied after it). Two contributions:
//
//  * PRINCIPAL-POINT ALIGNMENT. The scene renders through the eye's
//    asymmetric frustum, so the image centre is NOT the view axis -- it sits
//    ~15 deg toward the nose, mirrored between the eyes. The HUD is drawn at
//    identical pixel positions in both eyes; unshifted, the same element
//    would land ~30 deg apart binocularly and be unfusable. Shifting each
//    eye's 2D pass so the HUD centre sits ON the view axis restores fusion
//    (and puts the crosshair where the gun actually aims).
//  * The existing convergence nudge (hudshift), which brings the fused
//    overlay in from infinity to roughly arm's length.
//
// Only over a 3D scene: a menu frame is copied to both eyes identically and
// presented as a flat panel, where any shift would just move it off-centre.
static void HudOffsetForEye(int* dx, int* dy)
{
    *dx = 0; *dy = 0;
    if (GLScenePhase() || !VR_Active() || !g_vrDrewStereo)
        return;
    VR_GetHudPrincipalShift(g_vrEye, (int)g_viewW, (int)g_viewH, dx, dy);
    if (g_hudShiftPx != 0)
        *dx += (g_vrEye == 0) ? g_hudShiftPx : -g_hudShiftPx;
}

// Both of these take the engine's own screen coordinates and put them where
// they belong in the supersampled target: reshaped to the intended aspect and
// nudged for eye depth during the 2D pass, then scaled up.
static void MapRect(int* x, int* y, int* w, int* h, bool hudPass)
{
    if (hudPass) {
        HudRemap(x, y, w, h);
        int dx = 0, dy = 0;
        HudOffsetForEye(&dx, &dy);
        *x += dx;
        *y += dy;
    }
    // Per-axis: the scene target is shaped to the runtime eye aspect, so the
    // horizontal and vertical factors are not the same number any more.
    float sx = 1.0f, sy = 1.0f;
    VR_SupersampleXY(&sx, &sy);
    if (sx != 1.0f || sy != 1.0f) {
        *x = (int)(*x * sx + 0.5f); *y = (int)(*y * sy + 0.5f);
        *w = (int)(*w * sx + 0.5f); *h = (int)(*h * sy + 0.5f);
    }
}

extern "C" void WINAPI i_glViewport(int x, int y, int w, int h)
{
    typedef void (WINAPI *Fn)(int,int,int,int);
    // Remember what the ENGINE asked for: a pass-boundary phase change
    // re-issues it under the new mapping, whichever of viewport/projection
    // the engine happens to set first.
    g_lastRawViewport[0] = x; g_lastRawViewport[1] = y;
    g_lastRawViewport[2] = w; g_lastRawViewport[3] = h;
    g_lastRawViewportValid = true;
    GLSurf_NoteViewport(x, y, w, h);
    MapRect(&x, &y, &w, &h, !GLScenePhase());
    ((Fn)p_glViewport)(x, y, w, h);
}

extern "C" void WINAPI i_glScissor(int x, int y, int w, int h)
{
    typedef void (WINAPI *Fn)(int,int,int,int);
    // The scissor has to follow the viewport through the same remap, or the 2D
    // pass would be clipped against a rectangle that no longer matches where it
    // is being drawn.
    g_lastRawScissor[0] = x; g_lastRawScissor[1] = y;
    g_lastRawScissor[2] = w; g_lastRawScissor[3] = h;
    g_lastRawScissorValid = true;
    GLSurf_NoteScissor(x, y, w, h);
    MapRect(&x, &y, &w, &h, !GLScenePhase());
    ((Fn)p_glScissor)(x, y, w, h);
}

// Re-issue the engine's last viewport/scissor under the current phase's
// mapping, straight through the driver (never back through the wrappers,
// which would re-store them as new engine requests).
static void ReapplyStoredRects()
{
    typedef void (WINAPI *Fn)(int,int,int,int);
    const bool hudPass = !GLScenePhase();
    if (g_lastRawViewportValid && p_glViewport) {
        int x = g_lastRawViewport[0], y = g_lastRawViewport[1];
        int w = g_lastRawViewport[2], h = g_lastRawViewport[3];
        MapRect(&x, &y, &w, &h, hudPass);
        ((Fn)p_glViewport)(x, y, w, h);
    }
    if (g_lastRawScissorValid && p_glScissor) {
        int x = g_lastRawScissor[0], y = g_lastRawScissor[1];
        int w = g_lastRawScissor[2], h = g_lastRawScissor[3];
        MapRect(&x, &y, &w, &h, hudPass);
        ((Fn)p_glScissor)(x, y, w, h);
    }
}

static void UpdateScenePhase()
{
    const bool now = GLScenePhase();
    // The surface layer must never claim a HUD or menu draw, so it is told
    // the phase from the SAME classifier the viewport mapping uses -- there is
    // only one answer to "are we in the 3D pass" in this build.
    GLSurf_ScenePhase(now ? 1 : 0);
    if (now != g_glScenePhaseApplied) {
        g_glScenePhaseApplied = now;
        ReapplyStoredRects();
    }
}

// A load/ortho/identity/frustum landed on the projection stack at depth 0.
// Depth 0 is the only level that decides the pass -- a pushed ortho is a
// mid-scene excursion (flare style), not the overlay.
static void SetProjKind0(int kind)
{
    if (g_projStackDepth > 0)
        return;
    if (g_projKind0 == PROJK_PERSP && kind == PROJK_ORTHO)
        g_glScene2DDone = true;         // the overlay has begun for this frame
    if (kind == PROJK_PERSP)
        g_glScenePending = false;       // the queued scene's GL is now running
    g_projKind0 = kind;
    UpdateScenePhase();
}

static void CODVR_SceneQueued()
{
    g_glScenePending = true;
    g_glScene2DDone  = false;
    g_projNoteDone   = false;    // the optics note re-records for this window
    UpdateScenePhase();
}

// Per-frame reset, from the swap hook: the next frame is 2D until its render
// hook says otherwise, exactly the assumption menus and loading screens need.
// Syncs the applied flag SILENTLY -- re-issuing rects between frames would
// shrink the live scissor for nothing; the engine re-establishes its own
// state at the top of every frame, and the transition re-issue already
// covers every pass edge inside one.
static void CODVR_GLFrameReset()
{
    g_glScenePending = false;
    g_glScene2DDone  = false;
    g_projKind0      = PROJK_ORTHO;
    g_projStackDepth = 0;
    g_glScenePhaseApplied = GLScenePhase();
}

// GL_BACK / GL_FRONT are meaningless for a framebuffer object -- naming one
// while ours is bound is an error and the draw is dropped. The engine sets the
// draw buffer every frame, so translate the window-buffer names to our colour
// attachment whenever the offscreen target is in use.
enum { GL_FRONT_ = 0x0404, GL_BACK_B = 0x0405, GL_COLOR_ATTACHMENT0_B = 0x8CE0 };

static unsigned TranslateBuffer(unsigned buf)
{
    if (VR_Supersample() > 1.0f && (buf == GL_BACK_B || buf == GL_FRONT_))
        return GL_COLOR_ATTACHMENT0_B;
    return buf;
}

extern "C" void WINAPI i_glDrawBuffer(unsigned buf)
{
    typedef void (WINAPI *Fn)(unsigned);
    ((Fn)p_glDrawBuffer)(TranslateBuffer(buf));
}

extern "C" void WINAPI i_glReadBuffer(unsigned buf)
{
    typedef void (WINAPI *Fn)(unsigned);
    ((Fn)p_glReadBuffer)(TranslateBuffer(buf));
}

// ------------------------------------------ render-surface interception
//
// Everything below hands the draw stream to src/vr_glsurf.cpp. Two rules
// govern this whole block:
//
//   * NOTHING here queries the driver. A glGet on the ~10k draws of an
//     ordinary frame would stall the pipeline for information we can simply
//     remember, so every wrapper does a store (or a switch) and forwards.
//   * The surface layer decides whether it OWNS a draw. When it returns 1 it
//     has already issued the draw itself; the wrapper must not issue it
//     again. Returning 0 -- the answer for every draw that is not positively
//     identified as the first-person weapon or hands -- means the engine's
//     own call goes through byte-for-byte, which is what "fails closed" means
//     at this layer.

static bool g_glSurfDriverReady;

static void InstallGLSurfDriver()
{
    if (g_glSurfDriverReady)
        return;
    // These are the REAL driver entry points resolved at load: calling back
    // through our own i_* wrappers would recurse forever the moment the
    // surface layer re-issued a draw.
    GLSurfDriver d;
    memset(&d, 0, sizeof(d));
    d.drawElements  = (void (WINAPI*)(unsigned,int,unsigned,const void*))p_glDrawElements;
    d.drawArrays    = (void (WINAPI*)(unsigned,int,int))p_glDrawArrays;
    d.loadMatrixf   = (void (WINAPI*)(const float*))p_glLoadMatrixf;
    d.matrixMode    = (void (WINAPI*)(unsigned))p_glMatrixMode;
    d.getFloatv     = (void (WINAPI*)(unsigned,float*))p_glGetFloatv;
    d.getIntegerv   = (void (WINAPI*)(unsigned,int*))p_glGetIntegerv;
    d.getDoublev    = (void (WINAPI*)(unsigned,double*))p_glGetDoublev;
    d.isEnabled     = (unsigned char (WINAPI*)(unsigned))p_glIsEnabled;
    d.getTexLevelParameteriv =
        (void (WINAPI*)(unsigned,int,unsigned,int*))p_glGetTexLevelParameteriv;
    d.vertexPointer = (void (WINAPI*)(int,unsigned,int,const void*))p_glVertexPointer;
    d.normalPointer = (void (WINAPI*)(unsigned,int,const void*))p_glNormalPointer;
    if (!d.drawElements || !d.loadMatrixf || !d.getFloatv)
        return;                            // try again on the next make-current
    GLSurf_SetDriver(&d);
    g_glSurfDriverReady = true;
    Log("glsurf: driver installed (draw/matrix/query entry points resolved)");
}

extern "C" void WINAPI i_glDrawElements(unsigned mode, int count, unsigned type,
                                        const void* idx)
{
    typedef void (WINAPI *Fn)(unsigned,int,unsigned,const void*);
    if (GLSurf_OnDrawElements(mode, count, type, idx))
        return;                            // the surface layer issued it
    ((Fn)p_glDrawElements)(mode, count, type, idx);
}

extern "C" void WINAPI i_glDrawArrays(unsigned mode, int first, int count)
{
    typedef void (WINAPI *Fn)(unsigned,int,int);
    if (GLSurf_OnDrawArrays(mode, first, count))
        return;
    ((Fn)p_glDrawArrays)(mode, first, count);
}

extern "C" void WINAPI i_glVertexPointer(int size, unsigned type, int stride, const void* p)
{
    typedef void (WINAPI *Fn)(int,unsigned,int,const void*);
    GLSurf_NoteVertexPointer(size, type, stride, p);
    ((Fn)p_glVertexPointer)(size, type, stride, p);
}

extern "C" void WINAPI i_glNormalPointer(unsigned type, int stride, const void* p)
{
    typedef void (WINAPI *Fn)(unsigned,int,const void*);
    GLSurf_NoteNormalPointer(type, stride, p);
    ((Fn)p_glNormalPointer)(type, stride, p);
}

extern "C" void WINAPI i_glTexCoordPointer(int size, unsigned type, int stride, const void* p)
{
    typedef void (WINAPI *Fn)(int,unsigned,int,const void*);
    GLSurf_NoteTexCoordPointer(size, type, stride, p);
    ((Fn)p_glTexCoordPointer)(size, type, stride, p);
}

extern "C" void WINAPI i_glColorPointer(int size, unsigned type, int stride, const void* p)
{
    typedef void (WINAPI *Fn)(int,unsigned,int,const void*);
    GLSurf_NoteColorPointer(size, type, stride, p);
    ((Fn)p_glColorPointer)(size, type, stride, p);
}

extern "C" void WINAPI i_glEnableClientState(unsigned cap)
{
    typedef void (WINAPI *Fn)(unsigned);
    GLSurf_NoteClientState(cap, 1);
    ((Fn)p_glEnableClientState)(cap);
}

extern "C" void WINAPI i_glDisableClientState(unsigned cap)
{
    typedef void (WINAPI *Fn)(unsigned);
    GLSurf_NoteClientState(cap, 0);
    ((Fn)p_glDisableClientState)(cap);
}

extern "C" void WINAPI i_glBindTexture(unsigned target, unsigned tex)
{
    typedef void (WINAPI *Fn)(unsigned,unsigned);
    GLSurf_NoteBindTexture(target, tex);
    ((Fn)p_glBindTexture)(target, tex);
}

// CoD1's renderer explicitly ignores this advertised extension at startup.
// Apply it at the game's existing mip-filter assignment instead, without
// changing the filter, uploads, mip levels or any VR-owned texture. Internal
// VR/menu allocations resolve the real driver directly, not this export.
extern "C" void WINAPI i_glTexParameteri(unsigned target, unsigned pname, int value)
{
    typedef void (WINAPI *ParamFn)(unsigned,unsigned,int);
    ((ParamFn)p_glTexParameteri)(target,pname,value);
    const bool mip = value >= 0x2700 && value <= 0x2703;
    if (target != 0x0DE1 || pname != 0x2801
        || (!mip && value != 0x2600 && value != 0x2601)) return;

    typedef HGLRC (WINAPI *ContextFn)(void);
    typedef const unsigned char* (WINAPI *StringFn)(unsigned);
    typedef void (WINAPI *GetIntFn)(unsigned,int*);
    typedef void (WINAPI *GetFloatFn)(unsigned,float*);
    typedef void (WINAPI *FloatParamFn)(unsigned,unsigned,float);
    ContextFn current=(ContextFn)p_wglGetCurrentContext;
    if (!current || !current() || !p_glGetString || !p_glGetIntegerv
        || !p_glGetFloatv || !p_glTexParameterf) return;
    int texture=0;
    ((GetIntFn)p_glGetIntegerv)(0x8069,&texture);
    if (!texture) return;
    const char* extensions=(const char*)((StringFn)p_glGetString)(0x1F03);
    const char* token="GL_EXT_texture_filter_anisotropic";
    const size_t length=strlen(token);
    bool supported=false;
    for (const char* match=extensions ? strstr(extensions,token) : nullptr;
         match; match=strstr(match+length,token)) {
        if ((match==extensions || match[-1]==' ')
            && (match[length]==' ' || match[length]==0)) { supported=true; break; }
    }
    if (!supported) return;
    float maximum=1;
    if (mip) {
        ((GetFloatFn)p_glGetFloatv)(0x84FF,&maximum);
        if (!_finite(maximum) || maximum<2) return;
    }
    ((FloatParamFn)p_glTexParameterf)(target,0x84FE,maximum);
    static bool logged;
    if (mip && !logged) {
        logged=true;
        float actual=0;
        typedef void (WINAPI *GetTexFn)(unsigned,unsigned,float*);
        if (p_glGetTexParameterfv)
            ((GetTexFn)p_glGetTexParameterfv)(target,0x84FE,&actual);
        Log("quality: mipmapped 2D texture %d anisotropy %.1fx (driver maximum %.1fx); non-mip/UI unchanged",
            texture,actual,maximum);
    }
}

extern "C" void WINAPI i_glDepthRange(double zn, double zf)
{
    typedef void (WINAPI *Fn)(double,double);
    GLSurf_NoteDepthRange(zn, zf);
    ((Fn)p_glDepthRange)(zn, zf);
}

extern "C" void WINAPI i_glDepthMask(unsigned char flag)
{
    typedef void (WINAPI *Fn)(unsigned char);
    GLSurf_NoteDepthMask(flag);
    ((Fn)p_glDepthMask)(flag);
}

extern "C" void WINAPI i_glDepthFunc(unsigned func)
{
    typedef void (WINAPI *Fn)(unsigned);
    GLSurf_NoteDepthFunc(func);
    ((Fn)p_glDepthFunc)(func);
}

extern "C" void WINAPI i_glCullFace(unsigned mode)
{
    typedef void (WINAPI *Fn)(unsigned);
    GLSurf_NoteCullFace(mode);
    ((Fn)p_glCullFace)(mode);
}

extern "C" void WINAPI i_glFrontFace(unsigned mode)
{
    typedef void (WINAPI *Fn)(unsigned);
    GLSurf_NoteFrontFace(mode);
    ((Fn)p_glFrontFace)(mode);
}

extern "C" void WINAPI i_glBlendFunc(unsigned s, unsigned d)
{
    typedef void (WINAPI *Fn)(unsigned,unsigned);
    GLSurf_NoteBlendFunc(s, d);
    ((Fn)p_glBlendFunc)(s, d);
}

extern "C" void WINAPI i_glAlphaFunc(unsigned func, float ref)
{
    typedef void (WINAPI *Fn)(unsigned,float);
    GLSurf_NoteAlphaFunc(func, ref);
    ((Fn)p_glAlphaFunc)(func, ref);
}

extern "C" void WINAPI i_glEnable(unsigned cap)
{
    typedef void (WINAPI *Fn)(unsigned);
    GLSurf_NoteEnable(cap, 1);
    ((Fn)p_glEnable)(cap);
}

extern "C" void WINAPI i_glDisable(unsigned cap)
{
    typedef void (WINAPI *Fn)(unsigned);
    GLSurf_NoteEnable(cap, 0);
    ((Fn)p_glDisable)(cap);
}

// Extension entry points reach the engine through wglGetProcAddress, so their
// wrappers are installed there rather than in the export table. Only the ones
// that change surface identity are wrapped: the multitexture unit selector
// (which texture binding belongs to which unit) and the compiled-vertex-array
// lock, which tells us the engine has frozen a vertex range.
static void (WINAPI *p_glActiveTextureARB)(unsigned);
static void (WINAPI *p_glClientActiveTextureARB)(unsigned);
static void (WINAPI *p_glLockArraysEXT)(int, int);
static void (WINAPI *p_glUnlockArraysEXT)(void);

extern "C" void WINAPI i_glActiveTextureARB(unsigned unit)
{
    GLSurf_NoteActiveTexture(unit);
    if (p_glActiveTextureARB) p_glActiveTextureARB(unit);
}

extern "C" void WINAPI i_glClientActiveTextureARB(unsigned unit)
{
    if (p_glClientActiveTextureARB) p_glClientActiveTextureARB(unit);
}

extern "C" void WINAPI i_glLockArraysEXT(int first, int count)
{
    GLSurf_NoteLockArrays(first, count);
    if (p_glLockArraysEXT) p_glLockArraysEXT(first, count);
}

extern "C" void WINAPI i_glUnlockArraysEXT(void)
{
    GLSurf_NoteUnlockArrays();
    if (p_glUnlockArraysEXT) p_glUnlockArraysEXT();
}

// ---- buffer objects -------------------------------------------------------
//
// The 2026-08-26 crash: the engine draws the viewmodel out of vertex buffer
// objects, so glVertexPointer's last argument is a byte OFFSET, and the layer
// had no way to know because none of these entry points were intercepted.
// Now they are, and vr_vbo mirrors every engine upload into a CPU shadow the
// feature code reads instead of ever touching an offset. Wrappers forward
// verbatim -- the engine's GL behaviour is unchanged; we only OBSERVE.
//
// Both the ARB names (what a 2003 engine requests) and the GL 1.5 core names
// are hooked, sharing slots: whichever spelling the engine asks for, the
// mirror sees the calls.
static void          (WINAPI *p_glGenBuffersARB)(int, unsigned*);
static void          (WINAPI *p_glDeleteBuffersARB)(int, const unsigned*);
static void          (WINAPI *p_glBindBufferARB)(unsigned, unsigned);
static void          (WINAPI *p_glBufferDataARB)(unsigned, ptrdiff_t, const void*, unsigned);
static void          (WINAPI *p_glBufferSubDataARB)(unsigned, ptrdiff_t, ptrdiff_t, const void*);
static void*         (WINAPI *p_glMapBufferARB)(unsigned, unsigned);
static unsigned char (WINAPI *p_glUnmapBufferARB)(unsigned);

// First few calls of each kind are logged with their arguments -- that is the
// Phase-1 flat trace ("which upload path does the engine use, at what sizes")
// -- and after that only the vr_vbo counters tick.
enum { VBO_TRACE_LINES = 10 };

extern "C" void WINAPI i_glGenBuffersARB(int n, unsigned* ids)
{
    if (p_glGenBuffersARB) p_glGenBuffersARB(n, ids);   // driver fills the ids
    VBO_NoteGenBuffers(n, ids);
    static int t; if (t < VBO_TRACE_LINES) { ++t;
        Log("vbo: GenBuffers n=%d first=%u", n, (n > 0 && ids) ? ids[0] : 0u); }
}

extern "C" void WINAPI i_glDeleteBuffersARB(int n, const unsigned* ids)
{
    VBO_NoteDeleteBuffers(n, ids);
    static int t; if (t < VBO_TRACE_LINES) { ++t;
        Log("vbo: DeleteBuffers n=%d first=%u", n, (n > 0 && ids) ? ids[0] : 0u); }
    if (p_glDeleteBuffersARB) p_glDeleteBuffersARB(n, ids);
}

extern "C" void WINAPI i_glBindBufferARB(unsigned target, unsigned id)
{
    VBO_NoteBindBuffer(target, id);
    static int t; if (t < VBO_TRACE_LINES) { ++t;
        Log("vbo: BindBuffer target=0x%04X id=%u", target, id); }
    if (p_glBindBufferARB) p_glBindBufferARB(target, id);
}

extern "C" void WINAPI i_glBufferDataARB(unsigned target, ptrdiff_t size,
                                         const void* data, unsigned usage)
{
    VBO_NoteBufferData(target, size, data, usage);
    static int t; if (t < VBO_TRACE_LINES) { ++t;
        Log("vbo: BufferData target=0x%04X size=%d data=%s usage=0x%04X",
            target, (int)size, data ? "yes" : "NULL(orphan)", usage); }
    if (p_glBufferDataARB) p_glBufferDataARB(target, size, data, usage);
}

extern "C" void WINAPI i_glBufferSubDataARB(unsigned target, ptrdiff_t offset,
                                            ptrdiff_t size, const void* data)
{
    VBO_NoteBufferSubData(target, offset, size, data);
    static int t; if (t < VBO_TRACE_LINES) { ++t;
        Log("vbo: BufferSubData target=0x%04X offset=%d size=%d",
            target, (int)offset, (int)size); }
    if (p_glBufferSubDataARB) p_glBufferSubDataARB(target, offset, size, data);
}

extern "C" void* WINAPI i_glMapBufferARB(unsigned target, unsigned access)
{
    void* p = p_glMapBufferARB ? p_glMapBufferARB(target, access) : nullptr;
    VBO_NoteMapBuffer(target, access, p);               // needs the driver's ptr
    static int t; if (t < VBO_TRACE_LINES) { ++t;
        Log("vbo: MapBuffer target=0x%04X access=0x%04X -> %s",
            target, access, p ? "ptr" : "NULL"); }
    return p;
}

extern "C" unsigned char WINAPI i_glUnmapBufferARB(unsigned target)
{
    // Mirror BEFORE forwarding: the mapped pointer dies when the driver's
    // Unmap returns, and the engine's writes are complete by the time it
    // calls this.
    VBO_NoteUnmapBuffer(target);
    static int t; if (t < VBO_TRACE_LINES) { ++t;
        Log("vbo: UnmapBuffer target=0x%04X", target); }
    return p_glUnmapBufferARB ? p_glUnmapBufferARB(target) : 0;
}

// Called from i_wglGetProcAddress. Returns our wrapper for the handful of
// extension entry points that carry surface identity, and null (meaning "hand
// back the driver's own function") for everything else -- the engine's shader
// compiler, VBO paths and anything else stay completely untouched.
static PROC GLSurfHookExtension(LPCSTR name, PROC real)
{
    struct Entry { const char* name; void** slot; void* wrapper; };
    static const Entry kHooks[] = {
        { "glActiveTextureARB",       (void**)&p_glActiveTextureARB,       (void*)&i_glActiveTextureARB },
        { "glClientActiveTextureARB", (void**)&p_glClientActiveTextureARB, (void*)&i_glClientActiveTextureARB },
        { "glLockArraysEXT",          (void**)&p_glLockArraysEXT,          (void*)&i_glLockArraysEXT },
        { "glUnlockArraysEXT",        (void**)&p_glUnlockArraysEXT,        (void*)&i_glUnlockArraysEXT },
        // Buffer objects: the vr_vbo mirror. ARB and core spellings share
        // slots -- the engine uses one, and either lands in the same wrapper.
        { "glGenBuffersARB",          (void**)&p_glGenBuffersARB,          (void*)&i_glGenBuffersARB },
        { "glGenBuffers",             (void**)&p_glGenBuffersARB,          (void*)&i_glGenBuffersARB },
        { "glDeleteBuffersARB",       (void**)&p_glDeleteBuffersARB,       (void*)&i_glDeleteBuffersARB },
        { "glDeleteBuffers",          (void**)&p_glDeleteBuffersARB,       (void*)&i_glDeleteBuffersARB },
        { "glBindBufferARB",          (void**)&p_glBindBufferARB,          (void*)&i_glBindBufferARB },
        { "glBindBuffer",             (void**)&p_glBindBufferARB,          (void*)&i_glBindBufferARB },
        { "glBufferDataARB",          (void**)&p_glBufferDataARB,          (void*)&i_glBufferDataARB },
        { "glBufferData",             (void**)&p_glBufferDataARB,          (void*)&i_glBufferDataARB },
        { "glBufferSubDataARB",       (void**)&p_glBufferSubDataARB,       (void*)&i_glBufferSubDataARB },
        { "glBufferSubData",          (void**)&p_glBufferSubDataARB,       (void*)&i_glBufferSubDataARB },
        { "glMapBufferARB",           (void**)&p_glMapBufferARB,           (void*)&i_glMapBufferARB },
        { "glMapBuffer",              (void**)&p_glMapBufferARB,           (void*)&i_glMapBufferARB },
        { "glUnmapBufferARB",         (void**)&p_glUnmapBufferARB,         (void*)&i_glUnmapBufferARB },
        { "glUnmapBuffer",            (void**)&p_glUnmapBufferARB,         (void*)&i_glUnmapBufferARB },
    };
    for (size_t i = 0; i < sizeof(kHooks)/sizeof(kHooks[0]); ++i) {
        if (strcmp(name, kHooks[i].name) == 0) {
            *kHooks[i].slot = (void*)real;
            return (PROC)kHooks[i].wrapper;
        }
    }
    return nullptr;
}

// ---- flat draw-stream capture ---------------------------------------------
//
// The whole Phase 1 census runs on a monitor. codvr.cfg arms it, the game is
// launched with `+spmap`, the capture writes itself out and the process is
// asked to close normally. No headset, no human watching a screen, and no
// judgement call about what "looks like" the weapon.

// Every flat-run / probe setting lives in ONE struct that knows the
// production contract (see vr_cfg_contract.h). The names below are views onto
// it, so the hundred-odd existing use sites read as before while there is
// exactly one place that decides what a probe setting is allowed to be.
//
// The kill switch this replaced ("vr_disable 1" taken at face value) is how a
// capture rig reached a player on 2026-08-25. A probe setting now has to be
// ARMED, and an unarmed config ends at the production values whatever it says.
static CodvrProbeCfg g_probeCfg = {};
static_assert(CODVR_PROBE_CLASS_NONE == GLSC_NONE,
              "contract's 'no probe' value drifted from GLSurfClass");

static int&   g_vrDisabled     = g_probeCfg.vrDisabled;
static int&   g_glcapFrames    = g_probeCfg.capFrames;    // 0 disables the capture entirely
static int&   g_glcapSkip      = g_probeCfg.capSkip;      // scene frames to let the map settle
static int&   g_glcapAutoQuit  = g_probeCfg.autoQuit;
static int&   g_glcapQuitAfter = g_probeCfg.quitAfter;    // scene frames before a no-capture quit
static char (&g_glcapTag)[48]  = g_probeCfg.tag;
static int    g_glcapDumped    = 0;                       // runtime state, not configuration

// The flat A/B probe (Phase 2). One class, one known rigid delta, everything
// else untouched -- so a screenshot pair can be scored by a script instead of
// by someone looking at a monitor and deciding it "moved a bit".
static int&    g_glprobeClass    = g_probeCfg.probeClass;
static float (&g_glprobeDelta)[4] = g_probeCfg.delta;     // dx, dy, dz, yaw

static void ApplyGLProbe()
{
    GLSurf_SetProbe(g_glprobeClass, g_glprobeDelta[0], g_glprobeDelta[1],
                    g_glprobeDelta[2], g_glprobeDelta[3]);
    float piv[3] = { 0, 0, 0 };
    const int known = GLSurf_ClassPivot(g_glprobeClass, piv);
    Log("config: glsurf probe class=%s d=(%.3f %.3f %.3f) yaw=%.2f  pivot=%s "
        "(%.2f %.2f %.2f)",
        GLSurf_ClassName(g_glprobeClass), g_glprobeDelta[0], g_glprobeDelta[1],
        g_glprobeDelta[2], g_glprobeDelta[3],
        known ? "surface" : "CAMERA (nothing of this class drawn yet)",
        piv[0], piv[1], piv[2]);
    if (!known && g_glprobeDelta[3] != 0.0f)
        Log("glsurf: WARNING -- a yaw probe with no surface pivot rotates about "
            "the eye, which swings the object round the head instead of turning it");
}

// Build a path beside this DLL. Used for the capture dump, which must land in
// the game folder next to codvr_result.log rather than in a temp directory.
static bool PathBesideModule(const char* leaf, char* out, size_t outSize)
{
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                       | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&PathBesideModule, &self);
    char path[MAX_PATH];
    if (!GetModuleFileNameA(self, path, MAX_PATH))
        return false;
    char* slash = strrchr(path, '\\');
    if (!slash)
        return false;
    slash[1] = 0;
    if (strlen(path) + strlen(leaf) + 1 > outSize)
        return false;
    strcpy_s(out, outSize, path);
    strcat_s(out, outSize, leaf);
    return true;
}

// SHA-256 of a file, uppercase hex to match what PowerShell's Get-FileHash
// prints. The build-ID block states the hash of the DLL that is ACTUALLY
// executing, so the harness can prove identity from the log alone. The
// implementation is self-contained (FIPS 180-4, pinned to the 'abc' vector
// by the self test) rather than CryptoAPI: this runs at DLL ATTACH, and
// CryptAcquireContext can pull in provider DLLs under the loader lock.
// Fails closed: any I/O error returns false.
static void Sha256Block(const unsigned char* p, UINT32 st[8])
{
    static const UINT32 K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
        0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
        0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
        0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
        0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
        0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
        0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
        0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
        0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    #define CODVR_ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))
    UINT32 w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = ((UINT32)p[i*4] << 24) | ((UINT32)p[i*4+1] << 16)
             | ((UINT32)p[i*4+2] << 8) | (UINT32)p[i*4+3];
    for (int i = 16; i < 64; ++i) {
        const UINT32 s0 = CODVR_ROR(w[i-15], 7) ^ CODVR_ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        const UINT32 s1 = CODVR_ROR(w[i-2], 17) ^ CODVR_ROR(w[i-2],  19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    UINT32 a = st[0], b = st[1], c = st[2], d = st[3];
    UINT32 e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 64; ++i) {
        const UINT32 S1 = CODVR_ROR(e, 6) ^ CODVR_ROR(e, 11) ^ CODVR_ROR(e, 25);
        const UINT32 ch = (e & f) ^ (~e & g);
        const UINT32 t1 = h + S1 + ch + K[i] + w[i];
        const UINT32 S0 = CODVR_ROR(a, 2) ^ CODVR_ROR(a, 13) ^ CODVR_ROR(a, 22);
        const UINT32 mj = (a & b) ^ (a & c) ^ (b & c);
        const UINT32 t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
    st[4] += e; st[5] += f; st[6] += g; st[7] += h;
    #undef CODVR_ROR
}

static bool Sha256File(const char* path, char out[65])
{
    out[0] = 0;
    HANDLE f = CreateFileA(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    UINT32 st[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    static unsigned char buf[65536 + 64];  // load/self-test time only
    unsigned rem = 0;
    UINT64 total = 0;
    bool ok = true;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(f, buf + rem, 65536, &got, nullptr)) { ok = false; break; }
        total += got;
        unsigned have = rem + (unsigned)got;
        unsigned off = 0;
        for (; off + 64 <= have && got > 0; off += 64)
            Sha256Block(buf + off, st);
        if (got == 0) {
            // Final padding over the tail bytes.
            unsigned n = have - (got ? off : 0);
            unsigned char tail[128];
            memcpy(tail, buf + (got ? off : 0), n);
            tail[n++] = 0x80;
            while ((n % 64) != 56) { if (n >= sizeof(tail)) { ok = false; break; } tail[n++] = 0; }
            if (!ok) break;
            const UINT64 bits = total * 8;
            for (int i = 7; i >= 0; --i) tail[n++] = (unsigned char)(bits >> (i * 8));
            for (unsigned o = 0; o < n; o += 64) Sha256Block(tail + o, st);
            break;
        }
        rem = have - off;
        memmove(buf, buf + off, rem);
    }
    CloseHandle(f);
    if (!ok)
        return false;
    for (int i = 0; i < 8; ++i)
        sprintf_s(out + i * 8, 9, "%08X", st[i]);
    out[64] = 0;
    return true;
}

// Defined below with the capture tick; the sweep needs them first.
static void LogGLSurfStats(const char* when);
static void GLSurfQuitCleanly(HDC hdc, const char* why);

// ---- framebuffer capture for the flat A/B probe ---------------------------
//
// A screenshot pair is the acceptance test the milestone asks for: weapon
// pixels move, world pixels do not. It is taken at a FIXED SCENE FRAME INDEX
// so the A run and the B run sample the same moment of the same deterministic
// map load, and written as an uncompressed 24-bit TGA -- no library, no
// compression, nothing that could reorder a byte between the two runs.
static int g_shotFrame = -1;      // scene frame to grab; -1 = never
static int g_shotDone;

static void WriteTGA(const char* path, const unsigned char* rgb, int w, int h)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f)
        return;
    unsigned char hdr[18];
    memset(hdr, 0, sizeof(hdr));
    hdr[2]  = 2;                        // uncompressed true-colour
    hdr[12] = (unsigned char)(w & 0xFF);
    hdr[13] = (unsigned char)((w >> 8) & 0xFF);
    hdr[14] = (unsigned char)(h & 0xFF);
    hdr[15] = (unsigned char)((h >> 8) & 0xFF);
    hdr[16] = 24;
    fwrite(hdr, 1, sizeof(hdr), f);
    // glReadPixels gives bottom-up RGB; TGA wants bottom-up BGR.
    const size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char px[3] = { rgb[i*3+2], rgb[i*3+1], rgb[i*3+0] };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

static void GrabScreenshot(const char* stage)
{
    const int w = (int)g_viewW, h = (int)g_viewH;
    if (w < 2 || h < 2)
        return;
    typedef void (WINAPI *ReadFn)(int,int,int,int,unsigned,unsigned,void*);
    typedef void (WINAPI *PixStoreFn)(unsigned,int);
    if (!p_glReadPixels)
        return;
    unsigned char* buf = (unsigned char*)malloc((size_t)w * h * 3);
    if (!buf)
        return;
    if (p_glPixelStorei)
        ((PixStoreFn)p_glPixelStorei)(0x0D05 /*GL_PACK_ALIGNMENT*/, 1);
    ((ReadFn)p_glReadPixels)(0, 0, w, h, 0x1907 /*GL_RGB*/,
                             0x1401 /*GL_UNSIGNED_BYTE*/, buf);
    char leaf[96], path[MAX_PATH];
    sprintf_s(leaf, "codvr_shot_%s_%s.tga", g_glcapTag, stage);
    if (PathBesideModule(leaf, path, sizeof(path))) {
        WriteTGA(path, buf, w, h);
        Log("glsurf: screenshot %dx%d at scene frame %u -> %s",
            w, h, GLSurf_SceneFrameCount(), leaf);
    }
    free(buf);
}

// ---- flat automation: make sure the window is actually in front -----------
//
// MEASURED CAUSE, 2026-08-24. A flat run would load the map, finish
// CL_InitCGame, and then render 800k draws and ZERO 3D scenes for fifteen
// minutes. The engine log says why in one line:
//
//     CL_InitCGame:  3.54 seconds
//     cvar set cl_paused 1        <-- and never back to 0
//
// CoD1 SP pauses itself when its window is not the foreground window, and a
// paused client never renders the 3D scene -- so every capture came back
// empty for a reason that had nothing to do with the capture.
//
// The fix has to run INSIDE the game process. An external script calling
// SetForegroundWindow is refused by Windows foreground-lock often enough to be
// useless, and the obvious workaround -- synthesising keystrokes at the window
// -- is worse than useless: when the focus call quietly fails, those keys land
// in whatever the user is actually doing. From in here the window is our own,
// and attaching to the current foreground thread's input queue makes the
// request one Windows honours.
//
// Config-gated ("vr_flat_focus 1"), and it stops the moment a scene renders,
// so it can never fight the user for focus during ordinary play.
static int& g_flatFocus = g_probeCfg.flatFocus;

static void FlatForceForeground(HDC hdc)
{
    if (!g_flatFocus)
        return;
    if (GLSurf_SceneFrameCount() > 0)
        return;                       // gameplay is running; never touch focus again
    HWND hwnd = hdc ? WindowFromDC(hdc) : nullptr;
    if (!hwnd)
        return;
    static unsigned tick = 0;
    if ((tick++ % 60) != 0)           // about once a second, not every frame
        return;
    if (GetForegroundWindow() == hwnd)
        return;

    const DWORD myTid = GetCurrentThreadId();
    const DWORD fgTid = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    if (fgTid && fgTid != myTid)
        AttachThreadInput(myTid, fgTid, TRUE);
    ShowWindow(hwnd, SW_RESTORE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    if (fgTid && fgTid != myTid)
        AttachThreadInput(myTid, fgTid, FALSE);

    static int announced = 0;
    if (!announced) {
        announced = 1;
        Log("glsurf: flat focus assist active -- the engine pauses (cl_paused 1) "
            "and stops rendering the 3D scene whenever its window is not in front");
    }
}

// ---- the flat A/B probe sweep ---------------------------------------------
//
// PHASE 2, in ONE level start. The milestone wants four separate proofs --
// baseline, gun, right hand, left hand -- and running them as four launches
// means four map loads, four briefing screens, and four chances for the scene
// to differ between the A and the B image. Instead the sweep steps through
// every stage inside a single running level, so every stage sees the same
// camera, the same map and (if nobody touches the controls) very nearly the
// same animation phase.
//
// Each stage: arm one probe, let it settle, record one frame's draw stream,
// grab the framebuffer, log the measured pixel shift. Then the next.
struct ProbeStage {
    int         cls;
    float       dx, dy, dz, yaw;
    const char* name;
};

// 3.937 CoD units == 10 cm at the measured 1 unit = 1 inch.
//
// BASELINES ARE INTERLEAVED, one before every probe. A single baseline at the
// start would be compared against a probe stage half a second later, and the
// weapon idle-sways in that time -- so a pixel difference would mean "the
// probe moved it, OR the animation did" and the screenshot could not settle
// anything. With a control immediately adjacent to each probe, the difference
// between consecutive baselines IS the animation noise floor, and a probe has
// to beat its own local floor to count.
static const ProbeStage kSweep[] = {
    { GLSC_NONE,     0.000f, 0, 0,  0.0f, "base0"    },
    { GLSC_GUN,      3.937f, 0, 0,  0.0f, "gun_x"    },
    { GLSC_NONE,     0.000f, 0, 0,  0.0f, "base1"    },
    { GLSC_GUN,      0.000f, 0, 0, 15.0f, "gun_yaw"  },
    { GLSC_NONE,     0.000f, 0, 0,  0.0f, "base2"    },
    { GLSC_HAND_R,   3.937f, 0, 0,  0.0f, "handR_x"  },
    { GLSC_NONE,     0.000f, 0, 0,  0.0f, "base3"    },
    { GLSC_HAND_L,  -3.937f, 0, 0,  0.0f, "handL_x"  },
    { GLSC_NONE,     0.000f, 0, 0,  0.0f, "base4"    },
    { GLSC_SLEEVE_R, 3.937f, 0, 0,  0.0f, "sleeveR_x"},
    { GLSC_NONE,     0.000f, 0, 0,  0.0f, "base5"    },
    { GLSC_SLEEVE_L,-3.937f, 0, 0,  0.0f, "sleeveL_x"},
    { GLSC_NONE,     0.000f, 0, 0,  0.0f, "base6"    },
};
enum { SWEEP_STAGES = (int)(sizeof(kSweep)/sizeof(kSweep[0])) };

static int& g_sweepEnabled     = g_probeCfg.sweep;
static int& g_sweepStageFrames = g_probeCfg.sweepFrames;  // scene frames per stage
static int g_sweepStage = -1;
static int g_sweepStageStart;
static int g_sweepShotDone;
static int g_sweepCapArmed;
static int g_sweepFinished;

// Running totals as they stood when the current stage began, so each stage
// can be reported as a delta rather than as a cumulative figure.
static struct {
    unsigned transformed, suppressed, splitOk, splitBad;
    unsigned classified[GLSC_COUNT];
} g_sweepBase;

static void SweepBeginStage(int stage)
{
    const ProbeStage& st = kSweep[stage];
    g_glprobeClass    = st.cls;
    g_glprobeDelta[0] = st.dx; g_glprobeDelta[1] = st.dy;
    g_glprobeDelta[2] = st.dz; g_glprobeDelta[3] = st.yaw;
    ApplyGLProbe();
    Log("glsurf sweep: stage %d/%d '%s' begins at scene frame %u",
        stage + 1, SWEEP_STAGES, st.name, GLSurf_SceneFrameCount());
    g_sweepStage      = stage;
    g_sweepStageStart = (int)GLSurf_SceneFrameCount();
    g_sweepShotDone   = 0;
    g_sweepCapArmed   = 0;

    // Snapshot the running totals so the stage can report ITS OWN work. The
    // first sweep reported cumulative figures, which made a baseline stage
    // that transforms nothing claim "150 transformed draws" inherited from
    // the probe before it -- readable only if you already knew to subtract.
    GLSurfStats s0; GLSurf_GetStats(&s0);
    g_sweepBase.transformed = s0.transformed;
    g_sweepBase.suppressed  = s0.suppressed;
    g_sweepBase.splitOk     = GLSurf_SplitVerifiedCount();
    g_sweepBase.splitBad    = GLSurf_SplitRejectedCount();
    for (int i = 0; i < GLSC_COUNT; ++i) g_sweepBase.classified[i] = s0.classified[i];
}

static void SweepEndStage()
{
    if (g_sweepStage < 0) return;
    const ProbeStage& st = kSweep[g_sweepStage];
    GLSurfShift sh; GLSurf_GetShiftMetrics(&sh);
    GLSurfStats gs; GLSurf_GetStats(&gs);
    Log("glsurf sweep: stage '%s' RESULT geometry shift median %.1f px, p95 %.1f px, "
        "max %.1f px, last mean (%+.1f, %+.1f) px; samples %u valid / %u rejected-nearW",
        st.name, sh.medianPx, sh.p95Px, sh.maxPx, sh.lastX, sh.lastY,
        sh.validSamples, sh.rejectedNearW);
    Log("glsurf sweep: stage '%s' THIS STAGE %u transformed draws, %u suppressed, "
        "%u split-verified, %u split-rejected",
        st.name,
        gs.transformed - g_sweepBase.transformed,
        gs.suppressed  - g_sweepBase.suppressed,
        GLSurf_SplitVerifiedCount() - g_sweepBase.splitOk,
        GLSurf_SplitRejectedCount() - g_sweepBase.splitBad);
    Log("glsurf sweep: stage '%s' classes THIS STAGE gun %u handL %u handR %u "
        "sleeveL %u sleeveR %u vmOther %u",
        st.name,
        gs.classified[GLSC_GUN]      - g_sweepBase.classified[GLSC_GUN],
        gs.classified[GLSC_HAND_L]   - g_sweepBase.classified[GLSC_HAND_L],
        gs.classified[GLSC_HAND_R]   - g_sweepBase.classified[GLSC_HAND_R],
        gs.classified[GLSC_SLEEVE_L] - g_sweepBase.classified[GLSC_SLEEVE_L],
        gs.classified[GLSC_SLEEVE_R] - g_sweepBase.classified[GLSC_SLEEVE_R],
        gs.classified[GLSC_VM_OTHER] - g_sweepBase.classified[GLSC_VM_OTHER]);
}

// Driven once per game frame from the swap hook, but stepped on SCENE frames:
// a frame that drew no 3D scene must not advance the sweep, or a loading
// screen would burn through every stage before the level appeared.
static void SweepTick(HDC hdc)
{
    if (!g_sweepEnabled || g_sweepFinished)
        return;
    const int now = (int)GLSurf_SceneFrameCount();
    if (now < g_glcapSkip)
        return;                                  // let the level settle first

    if (g_sweepStage < 0) {
        SweepBeginStage(0);
        return;
    }
    const int age = now - g_sweepStageStart;

    // Record one frame of the draw stream a few frames in, once the probe has
    // certainly been applied to a whole frame.
    if (age == 4 && !g_sweepCapArmed) {
        g_sweepCapArmed = 1;
        char tag[64];
        sprintf_s(tag, "%s_%s", g_glcapTag, kSweep[g_sweepStage].name);
        GLSurf_ArmCapture(0, 1, tag);
    }
    if (g_sweepCapArmed == 1 && GLSurf_CaptureDone()) {
        g_sweepCapArmed = 2;
        char leaf[128], path[MAX_PATH];
        sprintf_s(leaf, "codvr_draws_%s_%s.csv", g_glcapTag, kSweep[g_sweepStage].name);
        if (PathBesideModule(leaf, path, sizeof(path))) {
            const int n = GLSurf_DumpCapture(path);
            Log("glsurf sweep: stage '%s' wrote %d draw records to %s",
                kSweep[g_sweepStage].name, n, leaf);
        }
    }
    if (age == 10 && !g_sweepShotDone) {
        g_sweepShotDone = 1;
        GrabScreenshot(kSweep[g_sweepStage].name);
    }
    if (age >= g_sweepStageFrames) {
        SweepEndStage();
        if (g_sweepStage + 1 < SWEEP_STAGES) {
            SweepBeginStage(g_sweepStage + 1);
        } else {
            g_sweepFinished = 1;
            g_glprobeClass = GLSC_NONE;
            ApplyGLProbe();                       // leave nothing armed
            Log("glsurf sweep: COMPLETE (%d stages)", SWEEP_STAGES);
            LogGLSurfStats("sweep");
            if (g_glcapAutoQuit)
                GLSurfQuitCleanly(hdc, "probe sweep complete");
        }
    }
}

static void LogGLSurfStats(const char* when)
{
    GLSurfStats s;
    GLSurf_GetStats(&s);
    Log("glsurf [%s]: draws %u (scene %u, depth-hacked %u)", when,
        s.drawsSeen, s.drawsScene, s.drawsDepthHacked);
    Log("glsurf [%s]: gun %u  handL %u  handR %u  sleeveL %u  sleeveR %u  vmOther %u",
        when, s.classified[GLSC_GUN], s.classified[GLSC_HAND_L],
        s.classified[GLSC_HAND_R], s.classified[GLSC_SLEEVE_L],
        s.classified[GLSC_SLEEVE_R], s.classified[GLSC_VM_OTHER]);
    Log("glsurf [%s]: transformed %u  suppressed %u  restores %u  "
        "rejected(noDriver %u, ambiguous %u)  pairMismatch %u",
        when, s.transformed, s.suppressed, s.matrixRestores,
        s.rejectedNoDriver, s.rejectedAmbiguous, GLSurf_PairMismatchCount());
    // The client-array probe's census, by reason. "nullPage" firing on scene
    // draws means the engine handed us buffer offsets and the shadow was NOT
    // consulted -- the exact silent-inertness the vbo lines below rule out.
    Log("glsurf [%s]: probe refused(nullPage %u, unmapped %u, wildIdx %u)  "
        "probe(%u queries, %u cache hits)  reentries %u",
        when, s.rejectedNullPage, s.rejectedUnmapped, s.rejectedWildIndex,
        s.probeQueries, s.probeCacheHits, GLSurf_ReentryCount());
    Log("glsurf [%s]: vbo-sourced reads: vert %u ok / %u miss, idx %u ok / %u miss, "
        "scratch refused %u",
        when, s.vboVertResolves, s.vboVertMisses, s.vboIdxResolves,
        s.vboIdxMisses, s.rejectedVboScratch);
    {
        VBOStats v; VBO_GetStats(&v);
        Log("vbo [%s]: gen %u del %u bind %u(arr %u elem %u other %u)",
            when, v.genCalls, v.deleteCalls, v.bindCalls,
            v.bindArray, v.bindElement, v.bindOtherTarget);
        Log("vbo [%s]: Data %u(arr %u elem %u)  SubData %u(arr %u elem %u)  "
            "Map %u Unmap %u(copies %u)",
            when, v.dataCalls, v.dataArray, v.dataElement,
            v.subDataCalls, v.subDataArray, v.subDataElement,
            v.mapCalls, v.unmapCalls, v.unmapCopies);
        Log("vbo [%s]: shadows %u live (peak %u), %u KB (peak %u KB), stale %u; "
            "resolve %u hit / miss(unknown %u, stale %u, range %u); "
            "refused(tableFull %u, alloc %u, subRange %u, mapNoSize %u, unbound %u); "
            "contexts %u (overflow %u)",
            when, v.buffersLive, v.buffersPeak,
            (unsigned)(v.bytesShadowed / 1024), (unsigned)(v.bytesPeak / 1024),
            v.staleBuffers, v.resolveHits, v.resolveMissUnknown,
            v.resolveMissStale, v.resolveMissRange,
            v.tableFull, v.allocFailed, v.subOutOfRange, v.mapUnknownSize,
            v.dataUnbound, v.contextsSeen, v.ctxTableFull);
    }
    Log("glsurf [%s]: merged-batch split verified %u, rejected %u", when,
        GLSurf_SplitVerifiedCount(), GLSurf_SplitRejectedCount());
    {
        GLSurfShift sh; GLSurf_GetShiftMetrics(&sh);
        Log("glsurf [%s]: geometry shift median %.1f px, p95 %.1f px, max %.1f px "
            "over %u valid samples from %u draws (%u rejected at the near plane)",
            when, sh.medianPx, sh.p95Px, sh.maxPx, sh.validSamples, sh.draws,
            sh.rejectedNearW);
    }
    // The palm solve's verdicts on the two rendered hands. This is the Phase-3
    // evidence line: a flat hand-solve run must show solves > 0, refusals not
    // climbing, finite positions, and two hands at DIFFERENT positions.
    for (int cls = GLSC_HAND_L; cls <= GLSC_HAND_R; ++cls) {
        GLSurfHandSolve hs;
        const int valid = GLSurf_GetHandSolve(cls, &hs);
        Log("glsurf [%s]: palm %s: %u solves, %u refusals (idx %u, base %u, "
            "solve %u), valid=%d pos(%.2f %.2f %.2f) xAxis(%.3f %.3f %.3f) "
            "rms %.4f",
            when, GLSurf_ClassName(cls), hs.solves, hs.refusals,
            hs.refusedIdx, hs.refusedBase, hs.refusedSolve, valid,
            hs.pos[0], hs.pos[1], hs.pos[2],
            hs.axis[0], hs.axis[1], hs.axis[2], hs.rmsM);
        // The refusal SHAPES: which (indexCount, vertexSpan) draws refused,
        // and how near the nearest landmark table came on the last one. This
        // is what turns "refusals climbing" into a named batch.
        if (hs.refusals) {
            GLSurfRefuseBucket rb[GLS_REFUSE_BUCKETS];
            unsigned ovf = 0;
            const int nrb = GLSurf_GetRefuseHisto(cls, rb, &ovf);
            char buf[240] = { 0 };
            int off = 0;
            for (int i = 0; i < nrb && off < 200; ++i)
                off += sprintf_s(buf + off, sizeof(buf) - off,
                                 " (idx=%d span=%d x%u)",
                                 rb[i].count, rb[i].span, rb[i].hits);
            Log("glsurf [%s]: palm %s refusal shapes:%s%s  lastRef{site %d "
                "idx=%d span=%d bestRms %.4fm}  lastGoodScene %u (now %u)",
                when, GLSurf_ClassName(cls), buf, ovf ? " +ovf" : "",
                hs.lastRefSite, hs.lastRefCount, hs.lastRefSpan,
                hs.lastRefBestRmsM, hs.lastGoodScene, GLSurf_SceneFrameCount());
        }
    }
}

static void GLSurfQuitCleanly(HDC hdc, const char* why)
{
    // Close the way the user would: the engine runs its own shutdown, so the
    // next launch does not meet the "Run In Safe Mode?" prompt that a forced
    // termination leaves behind.
    HWND hwnd = WindowFromDC(hdc);
    if (!hwnd) hwnd = GetActiveWindow();
    if (hwnd) {
        Log("glsurf: %s -- posting WM_CLOSE for a clean exit", why);
        PostMessageA(hwnd, WM_CLOSE, 0, 0);
    } else {
        Log("glsurf: %s but no window found to close", why);
    }
}

// Which cgame syscalls are actually firing. When a flat run produces no scene
// at all, this is what separates "cgame is not running" from "cgame is running
// but 71 is not the render call in this build".
static void LogSyscallCensus()
{
    int top[8] = {0}; LONG topN[8] = {0};
    for (int n = 0; n < MAX_SYSCALL; ++n) {
        const LONG v = g_framesSeen[n];
        if (v <= 0) continue;
        for (int k = 0; k < 8; ++k) {
            if (v > topN[k]) {
                for (int j = 7; j > k; --j) { topN[j] = topN[j-1]; top[j] = top[j-1]; }
                topN[k] = v; top[k] = n;
                break;
            }
        }
    }
    Log("syscall census: %d:%ld  %d:%ld  %d:%ld  %d:%ld  %d:%ld  %d:%ld  %d:%ld  %d:%ld"
        "   (render=%d seen %ld frames)",
        top[0], topN[0], top[1], topN[1], top[2], topN[2], top[3], topN[3],
        top[4], topN[4], top[5], topN[5], top[6], topN[6], top[7], topN[7],
        g_renderSyscall, g_framesSeen[g_renderSyscall]);
}

// Armed runs answer their identity question in the log itself. The
// 2026-08-27 attended run was played on the safe baseline and nobody could
// tell until the artefacts came back empty; the vbogame harness now watches
// for this banner the moment gameplay starts and aborts the run if it is
// missing or wrong. Production (unarmed) configs never emit it. Called from
// the first-scene block, which already fires exactly once.
static void LogVbogameBanner()
{
    if (!g_probeCfg.armed)
        return;
    char self[MAX_PATH] = { 0 };
    char sha[65] = { 0 };
    if (!PathBesideModule("opengl32.dll", self, sizeof(self))
        || !Sha256File(self, sha))
        strcpy_s(sha, "UNAVAILABLE");
    VBOStats v;
    VBO_GetStats(&v);
    const bool shadowLive = (v.buffersLive > 0 || v.contextsSeen > 0);
    Log("");
    Log("=== VBOGAME REAL PALM PROBE ACTIVE ===");
    Log("instrumented DLL SHA: %s", sha);
    Log("probe armed: YES");
    Log("handsolve: %s", g_probeCfg.handSolveProbe ? "YES" : "NO");
    Log("VBO shadow: %s (%u buffers, %u contexts mirrored)",
        shadowLive ? "YES" : "INERT", v.buffersLive, v.contextsSeen);
    Log("===");
}

static void GLSurfCaptureTick(HDC hdc)
{
    static unsigned sceneFrames = 0;

    // Flat hand-solve diagnostic (armed configs only): keep zero-delta hand
    // targets installed so BuildHandDelta -- and through it the palm solve on
    // the REAL, VBO-backed vertex stream -- runs on every hand draw. A zero
    // offset with an identity rotation asks the hand to stay exactly where
    // the engine drew it, so the run LOOKS untouched while every solve
    // verdict lands in the stats dump.
    if (g_probeCfg.handSolveProbe) {
        GLSurf_SetHandTargetHold(GLSC_HAND_R, 1);
        GLSurf_SetHandTargetHold(GLSC_HAND_L, 1);
    }
    // vbogame milestones, once each, armed runs only: the harness names the
    // deepest stage a run actually reached instead of collapsing every
    // missing later line into "wrong binary".
    if (g_probeCfg.armed) {
        static int msViewmodel, msVbo, msPalmR, msPalmL, msBoth;
        if (!msViewmodel || !msVbo) {
            GLSurfStats st; GLSurf_GetStats(&st);
            if (!msViewmodel && (st.classified[GLSC_GUN] + st.classified[GLSC_HAND_L]
                                 + st.classified[GLSC_HAND_R]) > 0) {
                msViewmodel = 1;
                Log("=== FIRST VIEWMODEL DRAW ===");
            }
            if (!msVbo && (st.vboVertResolves + st.vboIdxResolves) > 0) {
                msVbo = 1;
                Log("=== VBO SHADOW RESOLVED ===");
            }
        }
        if (!msPalmR || !msPalmL) {
            GLSurfHandSolve hs;
            if (!msPalmR) {
                GLSurf_GetHandSolve(GLSC_HAND_R, &hs);
                if (hs.solves > 0) { msPalmR = 1; Log("=== RIGHT PALM SOLVED ==="); }
            }
            if (!msPalmL) {
                GLSurf_GetHandSolve(GLSC_HAND_L, &hs);
                if (hs.solves > 0) { msPalmL = 1; Log("=== LEFT PALM SOLVED ==="); }
            }
            if (msPalmR && msPalmL && !msBoth) {
                msBoth = 1;
                Log("=== REAL PALM PATH ACTIVE ===");
            }
        }
        // Raw evidence: the first refused attempt's and the first successful
        // attempt's OBSERVED landmark positions, per hand, exactly as read
        // from the live vertex stream. With these in the log, any future
        // "why did the solve say that" question is answerable offline.
        for (int hand = 0; hand < 2; ++hand) {
            // Up to THREE refused samples (the first can be a load-time
            // artifact; the 2026-08-28 refusal question needs a mid-gameplay
            // one) and one successful sample per hand.
            static int      dumpedRefused[2], dumpedOk[2];
            static unsigned lastAttemptSeen[2];
            if (dumpedRefused[hand] >= 3 && dumpedOk[hand])
                continue;
            VRHandPoseSample smp;
            VRHandPose_GetLastSample(hand, &smp);
            if (smp.attempts == 0 || smp.n <= 0)
                continue;
            if (smp.attempts == lastAttemptSeen[hand])
                continue;                    // same attempt as last tick
            lastAttemptSeen[hand] = smp.attempts;
            int* flag = smp.ok ? &dumpedOk[hand] : &dumpedRefused[hand];
            if (smp.ok ? (*flag != 0) : (*flag >= 3))
                continue;
            ++*flag;
            const char* nm = (hand == VRHP_TRIGGER) ? "TRIG" : "SUPP";
            Log("palmdump %s attempt=%u ok=%d rms=%.4f upm=%.2f n=%d",
                nm, smp.attempts, smp.ok, smp.rms, smp.unitsPerMetre, smp.n);
            char line[256];
            int  ll = 0;
            for (int i = 0; i < smp.n; ++i) {
                ll += sprintf_s(line + ll, sizeof(line) - ll,
                                " %d=(%.3f %.3f %.3f)", smp.idx[i],
                                smp.obs[i*3], smp.obs[i*3+1], smp.obs[i*3+2]);
                if (ll > 180 || i == smp.n - 1) {
                    Log("palmdump %s%s", nm, line);
                    ll = 0;
                    line[0] = 0;
                }
            }
        }
    }
    // ---- PRODUCTION palm evidence, bounded ------------------------------
    //
    // The 2026-08-28 hardware run refused ~50% of REAL hand draws at the
    // solver with a constant 0.0455 m residual. Rig coverage is complete (all
    // 16 shipped hand xmodels resolve to the 3 rigs in the table) and no other
    // shipped asset has that batch shape (1014-model scan), so the remaining
    // question is WHICH BYTES those draws read. Attended flat runs are no
    // longer permitted, so the raw evidence has to come out of an ordinary VR
    // session: two refused clouds and one solved cloud per hand, then silence.
    // Identical refused clouds mean stale/constant data; differing ones mean
    // live-but-wrong data. Six short lines, once per session.
    {
        static int dumpedRef[2], dumpedOk[2];
        static unsigned lastSeen[2];
        for (int hand = 0; hand < 2; ++hand) {
            if (dumpedRef[hand] >= 2 && dumpedOk[hand])
                continue;
            VRHandPoseSample smp;
            VRHandPose_GetLastSample(hand, &smp);
            if (smp.attempts == 0 || smp.n <= 0 || smp.attempts == lastSeen[hand])
                continue;
            lastSeen[hand] = smp.attempts;
            int* flag = smp.ok ? &dumpedOk[hand] : &dumpedRef[hand];
            if (smp.ok ? (*flag != 0) : (*flag >= 2))
                continue;
            ++*flag;
            const int cls = (hand == VRHP_TRIGGER) ? GLSC_HAND_R : GLSC_HAND_L;
            GLSurfHandSolve hsv;
            GLSurf_GetHandSolve(cls, &hsv);
            const char* nm = (hand == VRHP_TRIGGER) ? "TRIG" : "SUPP";
            Log("palmdump %s %s attempt=%u rms=%.4f upm=%.2f n=%d "
                "src{vbo=%u off=%u} rig=%d",
                nm, smp.ok ? "OK" : "REFUSED", smp.attempts, smp.rms,
                smp.unitsPerMetre, smp.n,
                smp.ok ? hsv.lastOkBuffer : hsv.lastRefBuffer,
                smp.ok ? hsv.lastOkOffset : hsv.lastRefOffset,
                smp.ok ? hsv.lastRig : -99);
            char line[256];
            int  ll = 0;
            for (int i = 0; i < smp.n; ++i) {
                ll += sprintf_s(line + ll, sizeof(line) - ll,
                                " %d=(%.3f %.3f %.3f)", smp.idx[i],
                                smp.obs[i*3], smp.obs[i*3+1], smp.obs[i*3+2]);
                if (ll > 180 || i == smp.n - 1) {
                    Log("palmdump %s%s", nm, line);
                    ll = 0;
                    line[0] = 0;
                }
            }
        }
    }

    {
        static unsigned wallBeat = 0;
        if (++wallBeat % 600 == 0) {
            GLSurfStats st; GLSurf_GetStats(&st);
            Log("glsurf wall %u: scenes %u, draws %u (dh %u), armed=%d done=%d",
                wallBeat, GLSurf_SceneFrameCount(), st.drawsSeen,
                st.drawsDepthHacked, GLSurf_CaptureArmed(), GLSurf_CaptureDone());
            // One line of geometry-sourcing truth per beat: on a VBO-drawing
            // engine, "vboOk 0 / nullPage climbing" is the silent-inertness
            // signature and must be visible in a hardware run's log.
            VBOStats v; VBO_GetStats(&v);
            Log("glsurf wall %u: vboOk %u vboMiss %u nullPage %u shadows %u/%uKB "
                "(peak %u) ctx %u tableFull %u",
                wallBeat, st.vboVertResolves + st.vboIdxResolves,
                st.vboVertMisses + st.vboIdxMisses, st.rejectedNullPage,
                v.buffersLive, (unsigned)(v.bytesShadowed / 1024),
                v.buffersPeak, v.contextsSeen, v.tableFull);
            // Sourcing HONESTY, split by reason. "vboMiss 0" used to be read
            // as "sourcing is healthy" while an orphaned store resolved
            // successfully to its zero fill and the palm solve fitted a bind
            // cloud onto 48 identical points. missUndef is the counter that
            // distinguishes "the engine has not written yet" from "the id is
            // unknown" and from "the shadow went stale".
            Log("glsurf wall %u: vboMiss unknown %u stale %u range %u "
                "UNDEF %u | orphans %u spanDrops %u",
                wallBeat, v.resolveMissUnknown, v.resolveMissStale,
                v.resolveMissRange, v.resolveMissUndefined,
                v.orphans, v.validSpanDrops);
            LogSyscallCensus();
        }
    }
    if (g_glScenePending || g_glScene2DDone)
        ++sceneFrames;

    // Heartbeat. An empty capture has two very different causes -- the game
    // never reached gameplay, or it did and the classifier saw nothing -- and
    // without this line they are indistinguishable in the log.
    {
        static unsigned lastBeat = 0;
        const unsigned nBeat = (unsigned)GLSurf_SceneFrameCount();
        if (nBeat >= lastBeat + 300) {
            lastBeat = nBeat;
            GLSurfStats st; GLSurf_GetStats(&st);
            Log("glsurf: %u scene frames, %u draws (%u depth-hacked), "
                "armed=%d recording-done=%d",
                nBeat, st.drawsSeen, st.drawsDepthHacked,
                GLSurf_CaptureArmed(), GLSurf_CaptureDone());
        }
    }

    if (g_glcapFrames <= 0) {
        // No capture, but a bounded flat run was still asked for (used by the
        // A/B probe, which needs screenshots rather than a draw census).
        if (g_glcapQuitAfter > 0 && (int)sceneFrames >= g_glcapQuitAfter && !g_glcapDumped) {
            g_glcapDumped = 1;
            LogGLSurfStats("run");
            if (g_glcapAutoQuit) GLSurfQuitCleanly(hdc, "flat run complete");
        }
        return;
    }
    if (!GLSurf_CaptureArmed()) {
        GLSurf_ArmCapture(g_glcapSkip, g_glcapFrames, g_glcapTag);
        Log("glsurf: capture armed -- skip %d scene frames, record %d, tag '%s'",
            g_glcapSkip, g_glcapFrames, g_glcapTag);
        return;
    }
    if (!GLSurf_CaptureDone())
        return;
    if (!g_glcapDumped) {
        g_glcapDumped = 1;
        char leaf[96], path[MAX_PATH];
        sprintf_s(leaf, "codvr_draws_%s.csv", g_glcapTag);
        if (PathBesideModule(leaf, path, sizeof(path))) {
            const int n = GLSurf_DumpCapture(path);
            Log("glsurf: wrote %d draw records to %s", n, path);
        } else {
            Log("glsurf: could not build a dump path");
        }
        LogGLSurfStats("capture");
    }
    if (!g_glcapAutoQuit)
        return;
    // A palm run needs the player to idle, FIRE and RELOAD after the two
    // recorded frames. Quitting the moment the capture completed (the
    // behaviour until 2026-08-27) closed the game seconds into gameplay, so
    // no fire/reload palm evidence could ever exist. With
    // vr_glcap_quit_after set, the run lingers until that many scene frames
    // and keeps dumping palm/vbo stats, so the animated states land in the
    // log before the clean self-quit.
    if (g_glcapQuitAfter > 0 && (int)sceneFrames < g_glcapQuitAfter) {
        static unsigned lastLinger = 0;
        const unsigned now = (unsigned)GLSurf_SceneFrameCount();
        if (now >= lastLinger + 600) {
            lastLinger = now;
            LogGLSurfStats("linger");
        }
        return;
    }
    static int quitPosted = 0;
    if (!quitPosted) {
        quitPosted = 1;
        if (g_glcapQuitAfter > 0)
            LogGLSurfStats("run-end");
        GLSurfQuitCleanly(hdc, "capture complete");
    }
}

// ----------------------------------------- projection-stack interception
//
// CoD1's renderer (id Tech 3 family) establishes its 3D camera by loading a
// perspective matrix onto the GL_PROJECTION stack, computed CPU-side from
// the refdef's SYMMETRIC fov. A headset eye is an ASYMMETRIC frustum, so
// the refdef fov is culling-only and the real optics are installed here:
// every matrix the engine puts on the projection stack is offered to
// vr.cpp (CODVR_MaybeReplaceProjection), which substitutes the armed eye's
// true frustum exactly when a 3D scene render is in flight and the matrix
// is structurally perspective. Menu/HUD orthographic loads and every
// modelview load pass through untouched.
//
// TIMING: the render syscall only QUEUES the scene -- the engine executes
// its GL command buffer after the syscall returns and before
// wglSwapBuffers, all on this thread. The armed window in vr.cpp therefore
// spans hook -> submit, which brackets exactly that execution.

static volatile LONG g_glMatrixMode = 0x1700;    // GL_MODELVIEW, the GL default
enum { GL_PROJECTION_MODE_ = 0x1701 };

extern "C" void WINAPI i_glMatrixMode(unsigned mode)
{
    typedef void (WINAPI *Fn)(unsigned);
    InterlockedExchange(&g_glMatrixMode, (LONG)mode);
    GLSurf_NoteMatrixMode(mode);
    ((Fn)p_glMatrixMode)(mode);
}

// ---- projection-event ring: the offline answer to "what were the seven
// perspective loads?" The 2026-08-19 run replaced 7 matrices per eye and
// nothing in the log said what they belonged to. Every projection-stack
// event up to the first completed stereo pair is recorded with the GL state
// current at that moment and dumped once, so the next run enumerates every
// load with its viewport, scissor, depth and classification -- no guessing.
enum { PROJRING_MAX = 48 };
struct ProjRingEntry {
    long     frame;                 // wglSwapBuffers count at the event
    char     op;                    // L=loadf D=loadd F=frustum O=ortho I=identity ^=push v=pop
    signed char eye;                // armed eye at the event, -1 none
    unsigned char replaced;         // 1 if the eye frustum was installed
    unsigned char persp;            // structurally perspective?
    unsigned char depth;            // projection stack depth at the event
    unsigned char scenePhase;       // classification at the event
    unsigned char scOn;
    float    m0, m5, m8, m9;        // the ENGINE's matrix (pre-replacement)
    int      vp[4], sc[4];
};
static ProjRingEntry g_projRing[PROJRING_MAX];
static unsigned g_projRingCount;    // total events; ring keeps the last N
static bool     g_projRingDumped;

static void ProjRingNote(char op, const float* engineM, int replaced, int persp)
{
    if (g_projRingDumped)
        return;                     // one-shot diagnostic; free afterwards
    typedef void (WINAPI *GetIntFn)(unsigned, int*);
    typedef unsigned char (WINAPI *IsEnFn)(unsigned);
    GetIntFn getInt = (GetIntFn)p_glGetIntegerv;
    IsEnFn   isEn   = (IsEnFn)p_glIsEnabled;
    ProjRingEntry& e = g_projRing[g_projRingCount++ % PROJRING_MAX];
    e.frame      = g_frame;
    e.op         = op;
    e.eye        = (signed char)VR_ProjectionArmedEye();
    e.replaced   = (unsigned char)replaced;
    e.persp      = (unsigned char)persp;
    e.depth      = (unsigned char)g_projStackDepth;
    e.scenePhase = (unsigned char)(GLScenePhase() ? 1 : 0);
    e.m0 = engineM ? engineM[0] : 0.0f;
    e.m5 = engineM ? engineM[5] : 0.0f;
    e.m8 = engineM ? engineM[8] : 0.0f;
    e.m9 = engineM ? engineM[9] : 0.0f;
    memset(e.vp, 0, sizeof(e.vp));
    memset(e.sc, 0, sizeof(e.sc));
    e.scOn = 0;
    if (getInt) {
        getInt(0x0BA2 /*GL_VIEWPORT*/, e.vp);
        getInt(0x0C10 /*GL_SCISSOR_BOX*/, e.sc);
        e.scOn = isEn ? (isEn(0x0C11 /*GL_SCISSOR_TEST*/) != 0) : 0;
    }
}

static void ProjRingDumpOnce()
{
    if (g_projRingDumped || g_projRingCount == 0)
        return;
    if (VR_TestProjectionReplacedCount(0) == 0 || VR_TestProjectionReplacedCount(1) == 0)
        return;                     // wait until both eyes have rendered once
    g_projRingDumped = true;
    unsigned n = g_projRingCount < PROJRING_MAX ? g_projRingCount : PROJRING_MAX;
    unsigned first = g_projRingCount < PROJRING_MAX ? 0 : g_projRingCount - PROJRING_MAX;
    Log("");
    Log("======= PROJECTION EVENT RING (last %u of %u) =======", n, g_projRingCount);
    Log("  op: L/D=load F=frustum O=ortho I=identity ^=push v=pop; state read AT the event");
    for (unsigned i = 0; i < n; ++i) {
        const ProjRingEntry& e = g_projRing[(first + i) % PROJRING_MAX];
        Log("  [%2u] f%ld %c eye%d repl%u persp%u d%u %s m0/m5/m8/m9 %.4f %.4f %+.4f %+.4f "
            "vp %d,%d %dx%d sc %s %d,%d %dx%d",
            i, e.frame, e.op, (int)e.eye, e.replaced, e.persp, e.depth,
            e.scenePhase ? "SCENE" : "hud  ",
            e.m0, e.m5, e.m8, e.m9,
            e.vp[0], e.vp[1], e.vp[2], e.vp[3],
            e.scOn ? "ON" : "off", e.sc[0], e.sc[1], e.sc[2], e.sc[3]);
    }
    Log("======= end projection ring =======");
}

// Record the viewport/scissor the scene is really rendered with, at the
// FIRST replacement of each render window -- that is the world pass's state.
// (The old version recorded only the first two replacements EVER, which
// pinned the optics verdict to whatever the engine loaded at that moment.)
static void NoteReplacedProjectionState()
{
    if (g_projNoteDone)
        return;
    const int eye = VR_ProjectionArmedEye();
    if (eye < 0)
        return;
    g_projNoteDone = true;
    typedef void (WINAPI *GetIntFn)(unsigned, int*);
    typedef unsigned char (WINAPI *IsEnFn)(unsigned);
    GetIntFn getInt = (GetIntFn)p_glGetIntegerv;
    IsEnFn   isEn   = (IsEnFn)p_glIsEnabled;
    if (!getInt)
        return;
    int vp[4] = { 0, 0, 0, 0 }, sc[4] = { 0, 0, 0, 0 };
    getInt(0x0BA2 /*GL_VIEWPORT*/, vp);
    getInt(0x0C10 /*GL_SCISSOR_BOX*/, sc);
    const int on = isEn ? (isEn(0x0C11 /*GL_SCISSOR_TEST*/) != 0) : 0;
    CODVR_NoteProjectionGLState(eye, vp, sc, on);
}

extern "C" void WINAPI i_glLoadMatrixf(const float* m)
{
    typedef void (WINAPI *Fn)(const float*);
    if (g_glMatrixMode == GL_PROJECTION_MODE_ && m) {
        const int persp = VRM_IsPerspective(m);
        // Replacement is offered ONLY to the scene's own loads. Hardware
        // showed 7 perspective loads per frame; one arriving after the 2D
        // overlay has begun is some HUD element's projection, and giving it
        // the eye frustum would paint that element across the whole view.
        if (persp && GLScenePhase()) {
            float repl[16];
            memcpy(repl, m, sizeof(repl));
            if (CODVR_MaybeReplaceProjection(repl)) {
                ((Fn)p_glLoadMatrixf)(repl);
                SetProjKind0(PROJK_PERSP);
                NoteReplacedProjectionState();
                ProjRingNote('L', m, 1, 1);
                return;
            }
        }
        ((Fn)p_glLoadMatrixf)(m);
        SetProjKind0(persp ? PROJK_PERSP : PROJK_ORTHO);
        ProjRingNote('L', m, 0, persp);
        return;
    }
    ((Fn)p_glLoadMatrixf)(m);
}

extern "C" void WINAPI i_glLoadMatrixd(const double* m)
{
    typedef void (WINAPI *Fd)(const double*);
    typedef void (WINAPI *Ff)(const float*);
    if (g_glMatrixMode == GL_PROJECTION_MODE_ && m) {
        float mf[16];
        for (int i = 0; i < 16; ++i)
            mf[i] = (float)m[i];
        const int persp = VRM_IsPerspective(mf);
        if (persp && GLScenePhase()) {
            float repl[16];
            memcpy(repl, mf, sizeof(repl));
            if (CODVR_MaybeReplaceProjection(repl)) {
                ((Ff)p_glLoadMatrixf)(repl);
                SetProjKind0(PROJK_PERSP);
                NoteReplacedProjectionState();
                ProjRingNote('D', mf, 1, 1);
                return;
            }
        }
        ((Fd)p_glLoadMatrixd)(m);
        SetProjKind0(persp ? PROJK_PERSP : PROJK_ORTHO);
        ProjRingNote('D', mf, 0, persp);
        return;
    }
    ((Fd)p_glLoadMatrixd)(m);
}

extern "C" void WINAPI i_glFrustum(double l, double r, double b, double t,
                                   double zn, double zf)
{
    typedef void (WINAPI *Fn)(double, double, double, double, double, double);
    typedef void (WINAPI *Ff)(const float*);
    if (g_glMatrixMode == GL_PROJECTION_MODE_
        && zn > 0.0 && zf > zn && r > l && t > b) {
        // Build the exact matrix glFrustum would produce and let the same
        // policy decide. glFrustum MULTIPLIES onto the current top, but its
        // only idiomatic use is straight after glLoadIdentity, where a load
        // of the replacement is equivalent.
        float m[16] = { 0 };
        m[0]  = (float)(2.0 * zn / (r - l));
        m[5]  = (float)(2.0 * zn / (t - b));
        m[8]  = (float)((r + l) / (r - l));
        m[9]  = (float)((t + b) / (t - b));
        m[10] = (float)(-(zf + zn) / (zf - zn));
        m[11] = -1.0f;
        m[14] = (float)(-2.0 * zf * zn / (zf - zn));
        if (GLScenePhase() && CODVR_MaybeReplaceProjection(m)) {
            ((Ff)p_glLoadMatrixf)(m);
            SetProjKind0(PROJK_PERSP);
            NoteReplacedProjectionState();
            ProjRingNote('F', m, 1, 1);
            return;
        }
        ((Fn)p_glFrustum)(l, r, b, t, zn, zf);
        SetProjKind0(PROJK_PERSP);
        ProjRingNote('F', m, 0, 1);
        return;
    }
    ((Fn)p_glFrustum)(l, r, b, t, zn, zf);
}

// The 2D overlay announces itself with an ortho (usually after an identity
// load) on the projection stack; both flip the classification to HUD when
// they land at depth 0. All four of these forward untouched -- they exist
// only so the classifier sees the stream.
extern "C" void WINAPI i_glOrtho(double l, double r, double b, double t,
                                 double zn, double zf)
{
    typedef void (WINAPI *Fn)(double, double, double, double, double, double);
    ((Fn)p_glOrtho)(l, r, b, t, zn, zf);
    if (g_glMatrixMode == GL_PROJECTION_MODE_) {
        SetProjKind0(PROJK_ORTHO);
        ProjRingNote('O', nullptr, 0, 0);
    }
}

extern "C" void WINAPI i_glLoadIdentity(void)
{
    typedef void (WINAPI *Fn)(void);
    ((Fn)p_glLoadIdentity)();
    if (g_glMatrixMode == GL_PROJECTION_MODE_) {
        SetProjKind0(PROJK_ORTHO);
        ProjRingNote('I', nullptr, 0, 0);
    }
}

extern "C" void WINAPI i_glPushMatrix(void)
{
    typedef void (WINAPI *Fn)(void);
    ((Fn)p_glPushMatrix)();
    if (g_glMatrixMode == GL_PROJECTION_MODE_) {
        if (g_projStackDepth < PROJK_STACK_MAX - 1)
            ++g_projStackDepth;
        ProjRingNote('^', nullptr, 0, 0);
    }
}

extern "C" void WINAPI i_glPopMatrix(void)
{
    typedef void (WINAPI *Fn)(void);
    ((Fn)p_glPopMatrix)();
    if (g_glMatrixMode == GL_PROJECTION_MODE_) {
        if (g_projStackDepth > 0)
            --g_projStackDepth;
        ProjRingNote('v', nullptr, 0, 0);
    }
}

// Diagnostic only: id Tech never multiplies onto the projection stack during
// a scene, so if it happens while an eye is armed the identification model
// is wrong -- say so once instead of silently mis-rendering.
static volatile LONG g_projMultSeen;

static void NoteProjectionMult()
{
    if (g_glMatrixMode == GL_PROJECTION_MODE_ && VR_ProjectionArmedEye() >= 0) {
        if (InterlockedIncrement(&g_projMultSeen) == 1)
            Log("VR: engine MULTIPLIED onto the projection stack during a 3D "
                "frame -- projection identification needs revisiting");
    }
}

extern "C" void WINAPI i_glMultMatrixf(const float* m)
{
    typedef void (WINAPI *Fn)(const float*);
    NoteProjectionMult();
    ((Fn)p_glMultMatrixf)(m);
}

extern "C" void WINAPI i_glMultMatrixd(const double* m)
{
    typedef void (WINAPI *Fn)(const double*);
    NoteProjectionMult();
    ((Fn)p_glMultMatrixd)(m);
}

extern "C" HGLRC WINAPI i_wglCreateContext(HDC hdc)
{
    // Milestone: the first time the GAME calls into the proxy on a normal
    // execution path -- well outside the loader lock, logger long ready.
    // This is the identity point the harness may rely on internally; the
    // DllMain build-id block above it is a secondary cross-check.
    static LONG announced = 0;
    if (!InterlockedExchange(&announced, 1))
        Log("=== PROXY FIRST CALL === (wglCreateContext)");
    typedef HGLRC (WINAPI *Fn)(HDC);
    return ((Fn)p_wglCreateContext)(hdc);
}

static HDC   g_curDC;
static HGLRC g_curRC;

extern "C" BOOL WINAPI i_wglMakeCurrent(HDC hdc, HGLRC rc)
{
    typedef BOOL (WINAPI *Fn)(HDC, HGLRC);
    BOOL ok = ((Fn)p_wglMakeCurrent)(hdc, rc);
    if (ok) {
        // The buffer mirror keys ids by context: the game and the Steam
        // overlay both render through this proxy, and their id namespaces
        // are unrelated. EVERY successful switch is noted, including the
        // (null) release.
        VBO_NoteMakeCurrent((void*)rc);
    }
    if (ok && hdc && rc) {
        g_curDC = hdc;
        g_curRC = rc;
        // A live context is the first moment the surface layer can be given
        // the real driver; before that every entry point is still unresolved.
        InstallGLSurfDriver();
    }
    return ok;
}

extern "C" BOOL WINAPI i_wglDeleteContext(HGLRC rc)
{
    typedef BOOL (WINAPI *Fn)(HGLRC);
    BOOL ok = ((Fn)p_wglDeleteContext)(rc);
    if (ok) VBO_NoteDeleteContext((void*)rc);   // that namespace's shadows die with it
    return ok;
}

extern "C" BOOL WINAPI i_wglSwapBuffers(HDC hdc)
{
    InterlockedIncrement(&g_frame);
    TryHookCgame();

    if (!Probe_Enabled()) {
        // Startup retries internally until the headset wakes, so putting the
        // Quest on after launching flat still brings VR up.
        if (!g_vrDisabled)
            VR_Startup(g_curDC ? g_curDC : hdc, g_curRC);

        // Whether the frame just finished carried a 3D scene -- captured
        // before the flag resets below; it is the input layer's menu/gameplay
        // discriminator for the COMING frame.
        const bool wasStereo = g_vrDrewStereo;

        if (VR_Active()) {
            // Size comes from the window, not the render hook. The hook only
            // runs while a 3D scene is being drawn, so trusting it left menus
            // and loading screens submitting a 0x0 rectangle -- i.e. black.
            // Prefer the engine's own render size over the window's client
            // rect. Fullscreen mode changes can leave the window larger than
            // what the engine actually draws into, and blitting past the drawn
            // region reads undefined pixels -- which showed up as a menu that
            // was offset and clipped. Fall back to the window only before any
            // scene has been rendered, when the engine size is still unknown.
            int bw = (int)g_viewW, bh = (int)g_viewH;
            if (bw < 2 || bh < 2) {
                HWND hwnd = WindowFromDC(hdc);
                RECT rc;
                if (hwnd && GetClientRect(hwnd, &rc)) {
                    bw = rc.right - rc.left;
                    bh = rc.bottom - rc.top;
                }
            }

            // Hand rendering back to the window before reading what was drawn,
            // then put the frame on the monitor as well. The mirror fills the
            // window's real client area, which is a little shorter than the
            // engine's render size once the title bar is accounted for.
            VR_UnbindSceneTarget();
            {
                int mw = bw, mh = bh;
                HWND hwnd = WindowFromDC(g_curDC ? g_curDC : hdc);
                RECT rc;
                if (hwnd && GetClientRect(hwnd, &rc)) {
                    const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
                    if (cw > 1 && ch > 1) { mw = cw; mh = ch; }
                }
                VR_MirrorToWindow(mw, mh);
            }

            // The frame is now complete, scene plus the 2D pass over it.
            // Capture and submit before the window swap consumes it.
            VR_SubmitFrame(bw, bh, g_vrDrewStereo, g_vrEye);

            static bool sizeLogged = false;
            if (!sizeLogged && bw > 1) {
                sizeLogged = true;
                Log("VR: back buffer %dx%d (from window), stereo=%d", bw, bh, (int)g_vrDrewStereo);
            }
        }
        // One-shot: enumerate the projection loads of the first stereo pair
        // (what the "7 replacements per eye" actually were).
        ProjRingDumpOnce();

        g_vrDrewStereo = false;
        CODVR_GLFrameReset();       // a frame is 2D until a scene render says otherwise
        FlatForceForeground(g_curDC ? g_curDC : hdc);
        SweepTick(hdc);              // back buffer still holds the finished frame
        GLSurf_BeginFrame();
        GLSurfCaptureTick(hdc);
        // Backstop for the per-scene reset in the render hook: if no scene was
        // rendered at all this frame (menu, loading screen), clear whatever was
        // captured so it cannot leak into the next frame's first scene.
        InterlockedExchange(&g_replayCount, 0);
        // Comfort: keys first (recenter, snap turn), then advance the smoothed
        // vignette so the next frame's overlay reflects current motion. All of
        // these are PER GAME FRAME: the smoothing is time-based (explicit dt),
        // so running twice per XR frame under pairing advances it by exactly
        // the same wall-clock amount, not twice as far.
        Comfort_PollInput();
        Comfort_Update();
        Comfort_NoteFrame();

        // Drive the XR frame loop for the frame the engine is about to build.
        // At a pair boundary this waits/begins/locates a new XR frame; between
        // the two eyes of a pair it deliberately does nothing, so no
        // xrWaitFrame ever lands between eye 0 and eye 1.
        VR_BeginFrame();

        // The pair machine owns eye order: the first game frame of an XR
        // frame renders eye 0, the second eye 1, and any abort, menu or
        // session hiccup restarts at eye 0. Ask it rather than toggling
        // blindly -- a blind toggle is exactly how a half-pair would desync.
        g_vrEye = VR_ExpectedEye();

        // Native VR input, AFTER the XR frame boundary above so both modules
        // consume the controller generation the coming pair renders with:
        // first the weapon/aim model, then the key/mouse injection layer
        // (movement, buttons, aim steering) that acts on it.
        {
            const double nowS = CODVR_NowSeconds();
            VRWeapon_Update(VRInput_State(), nowS, wasStereo ? 0 : 1);
            // The VR Options overlay (P4) owns the controllers while it is
            // open: nothing may be injected into the game underneath it.
            VRMenu_Update(VRInput_State(), nowS);
            VRSnap_Update(VRInput_State(), nowS);
            if (VRMenu_Open()) VRBinds_ReleaseAllInjectedInputs("VR options open");
            else               VRBinds_Update(nowS, wasStereo ? 0 : 1);
            UpdateTwoHandAdsSpread();
        }

        // Point the coming frame at the supersampled target. Done last, so the
        // engine draws every part of the next frame into it.
        if (VR_Active() && g_viewW > 1 && g_viewH > 1)
            VR_BindSceneTarget((int)g_viewW, (int)g_viewH);

        static bool announced = false;
        if (!announced && VR_Active()) {
            announced = true;
            Log("VR: ACTIVE - stereo submission running (%s)", VR_StatusText());
            Log("VR: rendering via syscall %d", g_renderSyscall);
        }
    }

    if (g_cgameHooked) {
        for (int n = 0; n < MAX_SYSCALL; ++n) {
            LONG c = InterlockedExchange(&g_callsThisFrame[n], 0);
            if (c > 0) {
                ++g_framesSeen[n];
                if (c == 1) ++g_framesOnce[n];
            }
        }

        // Record the call order of one real gameplay frame, in either mode.
        // This only observes -- nothing about the frame changes -- and it is
        // what identifies the entity adds the stereo path has to replay.
        if (!g_traceDone) {
            if (g_tracing) {
                g_tracing = 0;
                DumpTrace();
                g_traceDone = true;
            } else if (g_framesSeen[g_renderSyscall] > 100) {
                g_traceCount = 0;
                g_tracing = 1;
            }
        }

        // The diagnostic steps its own state machine and may consume the frame
        // outright while its camera authority test is running.
        if (Probe_FrameHook()) {
            typedef BOOL (WINAPI *SwapFn)(HDC);
            return ((SwapFn)p_wglSwapBuffers)(hdc);
        }
    }

    typedef BOOL (WINAPI *Fn)(HDC);
    return ((Fn)p_wglSwapBuffers)(hdc);
}

// ---------------------------------------------------------------- config

// codvr.cfg sits beside the DLL in the game folder. Everything here is
// something we may need to change without a compiler, most importantly which
// syscall is the render call -- that is still an educated guess.

// Shadow values for the grouped native-VR setters, so each single-value
// config line can re-apply the whole group. Defaults MUST match the module
// defaults in vr_weapon.cpp / vr_binds.cpp.
static float g_cfgCalPos[3]      = { 0, 0, 0 };
static float g_cfgCalRot[3]      = { 0, 0, 0 };        // pitch, yaw, roll
// Native hands: controller -> palm calibration, per OpenXR hand (indexed
// VRI_LEFT / VRI_RIGHT). Offline default is zero -- see VRNH_SetCalibration.
static float g_cfgNHCalPos[VRI_HANDS][3] = { { 0, 0, 0 }, { 0, 0, 0 } };
static float g_cfgNHCalRot[VRI_HANDS][3] = { { 0, 0, 0 }, { 0, 0, 0 } };
static float g_cfgAds[4]         = { 0.32f, 0.42f, 15.0f, 25.0f };
static int   g_cfgTwoHand        = 1;
static float g_cfgTwoHandDist    = 0.75f;
static float g_cfgTwoHandBlend   = 0.12f;
static float g_cfgTwoHandGrab    = 0.22f;
static float g_cfgTwoHandRelease = 0.32f;
static int   g_cfgHaptics        = 1;
static float g_cfgHapticStrength = 0.6f;
static int   g_cfgPhysCrouch     = 0;
static float g_cfgCrouchDrop     = 0.32f;
static float g_cfgSteerGain      = 0.55f;
static float g_cfgSteerCpd       = 9.09f;

// One VR binding line: "bindFire MOUSE1" etc. Returns true when the key
// matched one of the bind names (even if the value failed to parse).
static bool ParseBindConfig(const char* key, const char* text)
{
    static const struct { const char* name; int control; } kBinds[] = {
        { "bindFire",    VRB_FIRE },    { "bindAds",     VRB_ADS },
        { "bindJump",    VRB_JUMP },    { "bindCrouch",  VRB_CROUCH },
        { "bindProne",   VRB_PRONE },   { "bindMelee",   VRB_MELEE },
        { "bindReload",  VRB_RELOAD },  { "bindUse",     VRB_USE },
        { "bindGrenade", VRB_GRENADE }, { "bindMenu",    VRB_MENU },
    };
    for (size_t i = 0; i < sizeof(kBinds)/sizeof(kBinds[0]); ++i) {
        if (!_stricmp(key, kBinds[i].name)) {
            const int ok = VRBinds_SetBinding(kBinds[i].control, text);
            Log("config: %s = %s%s", kBinds[i].name, text,
                ok ? "" : " (UNRECOGNISED, unchanged)");
            return true;
        }
    }
    return false;
}

static void EnforceCameraStableBaseline();

// ONE config key applied. Extracted from LoadConfig (2026-09-04, P4) so the
// in-headset VR Options overlay can apply a changed setting through EXACTLY
// the path codvr.cfg takes at load -- no second copy of the semantics.
// The aim crosshair is an ENGINE cvar (cg_drawCrosshair); the proxy cannot
// set it live, but it owns main\autoexec.cfg, which the engine runs at every
// launch. codvr.cfg's vr_crosshair is mirrored into that file whenever it is
// applied (load or a VR Options change), so the choice takes effect at the
// next launch. Only that one line is touched.
static char g_configDir[MAX_PATH];
static int  g_cfgCrosshair;
static void SyncCrosshairAutoexec()
{
    if (!g_configDir[0]) return;
    char path[MAX_PATH];
    sprintf_s(path, "%smain\\autoexec.cfg", g_configDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        Log("config: vr_crosshair: no %s to mirror into (engine default stays)", path);
        return;
    }
    static char text[16384];
    const size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    if (n >= sizeof(text) - 1) { Log("config: vr_crosshair: autoexec too large to edit"); return; }
    const int changed = CodvrSetCvarLine(text, sizeof(text), "cg_drawCrosshair", g_cfgCrosshair ? "1" : "0");
    if (changed < 0) { Log("config: vr_crosshair: autoexec edit refused"); return; }
    if (!changed) return;
    if (fopen_s(&f, path, "wb") != 0 || !f) { Log("config: vr_crosshair: cannot write %s", path); return; }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
    Log("config: vr_crosshair %d mirrored into %s (takes effect at the next launch)", g_cfgCrosshair, path);
}

static void ApplyConfigKey(const char* key, const char* text, float val)
{
        char probeReason[256];
        probeReason[0] = 0;
        if (VRSnap_ConfigKey(key, val)) {
            Log("config: %s = %s (snapshot)", key, text);
            return;
        }
        if (!_stricmp(key, "recenterKey")) {
            const int vk = Comfort_ParseKey(text);
            Comfort_SetRecenterKey(vk);
            Log("config: recenter key = %s%s", text, vk ? "" : " (UNRECOGNISED, disabled)");
            return;
        }
        if (!_stricmp(key, "snapTurnKeys")) {
            // Two names separated by a comma, e.g. "Q,E".
            char left[32] = {0}, right[32] = {0};
            const char* comma = strchr(text, ',');
            if (comma) {
                size_t n = (size_t)(comma - text);
                if (n >= sizeof(left)) n = sizeof(left) - 1;
                memcpy(left, text, n);
                strncpy_s(right, comma + 1, _TRUNCATE);
            }
            const int lv = Comfort_ParseKey(left), rv = Comfort_ParseKey(right);
            Comfort_SetSnapKeys(lv, rv);
            Log("config: snap turn keys = %s / %s%s", left, right,
                (lv && rv) ? "" : " (UNRECOGNISED, disabled)");
            return;
        }

        if (!_stricmp(key, "renderSyscall")) {
            g_renderSyscall = (int)val;
            Log("config: renderSyscall = %d", g_renderSyscall);
        } else if (!_stricmp(key, "probe")) {
            Probe_SetEnabled(val != 0);
            Log("config: probe mode = %d", (int)Probe_Enabled());
            if (Probe_Enabled()) {
                Log("***********************************************************");
                Log("*  PROBE MODE: VR IS DISABLED. The game runs FLAT on the  *");
                Log("*  monitor and writes a diagnostic verdict to this log.    *");
                Log("*  No headset needed. Set 'probe 0' in codvr.cfg for VR.   *");
                Log("***********************************************************");
            }
        } else if (!_stricmp(key, "worldscale")) {
            VR_SetWorldScale(val);
            Log("config: worldscale = %.2f units/metre", val);
        } else if (!_stricmp(key, "positionalgain")) {
            VR_SetPositionalGain(val);
            Log("config: positional tracking gain = %.2f", val);
        } else if (!_stricmp(key, "cullmargin")) {
            VR_SetCullMargin(val);
            Log("config: culling fov margin = %.1f deg per side", val);
        } else if (!_stricmp(key, "vignette")) {
            Comfort_SetVignette(val != 0);
            Log("config: comfort vignette = %s", val != 0 ? "on" : "off");
        } else if (!_stricmp(key, "vignetteStrength")) {
            Comfort_SetVignetteStrength(val);
            Log("config: vignette strength = %.2f", val);
        } else if (!_stricmp(key, "vignetteSpeed")) {
            Comfort_SetSpeedThreshold(val);
            Log("config: vignette speed threshold = %.0f units/sec", val);
        } else if (!_stricmp(key, "vignetteTurn")) {
            Comfort_SetTurnThreshold(val);
            Log("config: vignette turn threshold = %.0f deg/sec", val);
        } else if (!_stricmp(key, "snapTurn")) {
            Comfort_SetSnapTurn((int)val);
            Log("config: snap turn = %d degrees%s", (int)val,
                (int)val == 0 ? " (off, smooth mouse turning)" : "");
        } else if (!_stricmp(key, "snapMouseScale")) {
            Comfort_SetSnapMouseScale(val);
            Log("config: snap turn mouse scale = %.2f counts/degree", val);
        } else if (!_stricmp(key, "hudAspect")) {
            g_hudAspect = val;
            // The flat-screen capture reads back from the same band.
            VR_SetPanelAspect(g_hudAspect);
            Log("config: HUD aspect = %.3f%s", g_hudAspect,
                g_hudAspect <= 0.0f ? " (off, HUD stretched to fill)" : "");
        } else if (!_stricmp(key, "hudshift")) {
            g_hudShiftPx = (int)val;
            Log("config: HUD shift = %d px per eye%s", g_hudShiftPx,
                g_hudShiftPx == 0 ? " (HUD sits at infinity)" : "");
        // ---------------------------------- render-surface (glsurf) ------
        // Every probe / flat-run key goes through the contract, which drops
        // it unless the config armed probes first. Nothing here can quietly
        // turn a shipping build into a self-closing capture rig.
        } else if (ProbeCfg_Apply(&g_probeCfg, key, text, val,
                                  probeReason, sizeof(probeReason))) {
            if (probeReason[0])
                Log("%s", probeReason);
        } else if (!_stricmp(key, "vr_glsurf_route")) {
            GLSurf_SetRoute((int)val);
            Log("config: glsurf transform route = %s",
                GLSurf_Route() == GLS_ROUTE_SCRATCH_ID ? "scratch arrays" : "modelview delta");
        } else if (!_stricmp(key, "vr_glsurf_hide_sleeves")) {
            GLSurf_SetClassHidden(GLSC_SLEEVE_L, val != 0);
            GLSurf_SetClassHidden(GLSC_SLEEVE_R, val != 0);
            Log("config: glsurf sleeves %s", val != 0 ? "HIDDEN" : "drawn");
        } else if (!_stricmp(key, "vr_native_hands")) {
            VRNH_SetEnabled(val != 0);
            Log("config: native hands (live controller -> rendered hand) = %s",
                val != 0 ? "ON" : "OFF (engine animation, unchanged)");
        } else if (!_stricmp(key, "vr_native_hands_track_grace")) {
            VRNH_SetTrackingGrace(val);
            Log("config: native hands tracking grace = %.2f s", val);
        } else if (!_stricmp(key, "vr_native_hands_cal_table")) {
            VRNH_SetCalibrationTable(val != 0);
            Log("config: native hands anatomical calibration table = %s",
                val != 0 ? "ON (grip -> palm bone from the shipped rig)"
                         : "OFF (raw grip frame, pre-2026-09-03)");
        } else if (!_stricmp(key, "vr_native_hands_palm_flip")) {
            VRNH_SetPalmFlip(val != 0);
            Log("config: vr_native_hands_palm_flip = %.0f -- IGNORED since 2026-09-04d "
                "(the palm calibration is derived against the OpenXR grip spec; "
                "there is no runtime flip)", val);
        } else if (!_stricmp(key, "vr_brightness")) {
            Comfort_SetBrightness(val);
            Log("config: HMD brightness = %.2f", Comfort_Brightness());
        } else if (!_stricmp(key, "vr_postprocess")) {
            VR_SetPostprocessEnabled(val != 0);
            Log("config: stereo material postprocess = %s", val != 0 ? "ON" : "OFF");
        } else if (!_stricmp(key, "vr_postprocess_sharpen")) {
            VR_SetPostprocessSharpen(val);
        } else if (!_stricmp(key, "vr_postprocess_bloom")) {
            VR_SetPostprocessBloom(val);
        } else if (!_stricmp(key, "vr_postprocess_contrast")) {
            VR_SetPostprocessContrast(val);
        } else if (!_stricmp(key, "vr_postprocess_saturation")) {
            VR_SetPostprocessSaturation(val);
        } else if (!_stricmp(key, "vr_postprocess_shadow_lift")) {
            VR_SetPostprocessShadowLift(val);
        } else if (!_stricmp(key, "vr_crosshair")) {
            g_cfgCrosshair = (val != 0.0f);
            SyncCrosshairAutoexec();
        } else if (!_stricmp(key, "vr_trigger_hand_lock")) {
            VRNH_SetTriggerHandLock(val != 0.0f);
            Log("config: trigger hand lock (right hand rides the weapon as applied) = %s",
                val != 0.0f ? "ON" : "OFF");
        } else if (!_stricmp(key, "vr_support_pose_lock")) {
            VRNH_SetSupportPoseLock(val != 0.0f);
            Log("config: support pose lock (left hand rides the weapon while attached) = %s",
                val != 0.0f ? "ON" : "OFF");
        } else if (!_stricmp(key, "vr_twohand_stability") || !_stricmp(key, "vr_twohand_recoil_reduction")) {
            float stability, recoil;
            VRNH_GetHandling(&stability, &recoil);
            if (!_stricmp(key, "vr_twohand_stability")) stability=val;
            else recoil=val;
            VRNH_SetHandling(stability,recoil);
            Log("config: supported stability %.2f, firing-motion reduction %.2f", stability, recoil);
        } else if (!_stricmp(key, "vr_twohand_grab_cm") || !_stricmp(key, "vr_twohand_release_cm")
                || !_stricmp(key, "vr_twohand_slide_cm") || !_stricmp(key, "vr_twohand_blend_s")) {
            float gr, rl, sl, bl;
            VRNH_GetTwoHandTuning(&gr, &rl, &sl, &bl);
            if (!_stricmp(key, "vr_twohand_grab_cm"))    gr = val / 100.0f;
            if (!_stricmp(key, "vr_twohand_release_cm")) rl = val / 100.0f;
            if (!_stricmp(key, "vr_twohand_slide_cm"))   sl = val / 100.0f;
            if (!_stricmp(key, "vr_twohand_blend_s"))    bl = val;
            VRNH_SetTwoHandTuning(gr, rl, sl, bl);
            Log("config: two-hand tuning grab %.0f cm release %.0f cm slide +/-%.0f cm blend %.2f s",
                gr * 100.0f, rl * 100.0f, sl * 100.0f, bl);
        } else if (!_stricmp(key, "vr_sleeve_anatomical")) {
            GLSurf_SetSleeveAnatomical(val != 0);
            Log("config: sleeves follow the wrist anatomically = %s",
                val != 0 ? "ON (forearm continues the knuckle line, pivot at the wrist)"
                         : "OFF (sleeve rigid with the hand, animation wrist bend kept)");
        } else if (!_stricmp(key, "vr_sleeve_visible")) {
            GLSurf_SetSleeveVisible((int)val);
            Log("config: sleeves drawn = %s",
                (int)val >= 2 ? "BOTH" : (int)val == 1 ? "RIGHT only (the left forearm is hidden; "
                "the left HAND is still controller-owned)" : "NONE (hands only)");
        } else if (!_stricmp(key, "vr_sleeve_hide_near_head")) {
            GLSurf_SetSleeveHideNearHead(val != 0);
            Log("config: hide a sleeve whose elbow reaches the head = %s", val != 0 ? "ON" : "OFF");
        } else if (!_stricmp(key, "vr_hand_r_visual_pitch") || !_stricmp(key, "vr_hand_r_visual_yaw")
                || !_stricmp(key, "vr_hand_r_visual_roll") || !_stricmp(key, "vr_hand_l_visual_pitch")
                || !_stricmp(key, "vr_hand_l_visual_yaw") || !_stricmp(key, "vr_hand_l_visual_roll")) {
            const int hand = !_strnicmp(key, "vr_hand_l_", 10) ? VRI_LEFT : VRI_RIGHT;
            float tr[3];
            VRNH_GetHandVisualTrim(hand, tr);
            if (strstr(key, "_pitch")) tr[0] = val;
            if (strstr(key, "_yaw"))   tr[1] = val;
            if (strstr(key, "_roll"))  tr[2] = val;
            VRNH_SetHandVisualTrim(hand, tr);
            Log("config: %s hand VISUAL trim (rendered hand only; weapon, aim and the other "
                "hand untouched) pitch %.1f yaw %.1f roll %.1f deg",
                hand == VRI_LEFT ? "LEFT" : "RIGHT", tr[0], tr[1], tr[2]);
        } else if (!_stricmp(key, "vr_gun_prekey_policy")) {
            GLSurf_SetGunPreKeyPolicy(val != 0);
            Log("config: gun pre-key batch policy = %s",
                val != 0 ? "CARRY previous scene's authored body (never vanilla)"
                         : "SUPPRESS the batch (never vanilla)");
        } else if (!_stricmp(key, "vr_native_hands_gun_follow_fix")) {
            VRNH_SetGunFollowsLimbFix(val != 0);
            Log("config: vr_native_hands_gun_follow_fix = %.0f -- IGNORED since "
                "2026-09-04d (hand and weapon share the one palm anchor)", val);
        } else if (!_stricmp(key, "vr_physical_aim")) {
            // THE PHYSICAL AIM SEAM (brief 2026-09-03, sections 10-13): the
            // rendered barrel's direction is converted against the body
            // facing into the game's OWN view angles through the ordinary
            // aim input (relative mouse), which is the state CoD's fire,
            // spread, recoil and hit logic already read. The camera is
            // composed on the body basis + HMD and never sees those angles
            // (the steering-leak watchdog holds that invariant live).
            VRBinds_SetExperimentalWeapon(val != 0);
            VRWeapon_SetEnabled(val != 0);
            Log("config: physical aim (rendered barrel -> game aim state) = %s",
                val != 0 ? "ON" : "OFF");
        } else if (!_stricmp(key, "vr_native_hands_twohand")) {
            VRNH_SetTwoHandEnabled(val != 0);
            Log("config: native hands two-hand support = %s",
                val != 0 ? "ON" : "OFF");
        } else if (!_stricmp(key, "vr_native_hands_reload")) {
            VRNH_SetReloadEnabled(val != 0);
            Log("config: native hands physical reload = %s",
                val != 0 ? "ON" : "OFF");
        } else if (!_stricmp(key, "vr_manual_reload_mode")) {
            VRNH_SetReloadMode((int)val);
            Log("config: manual reload mode = %d (0 off, 1 assisted, 2 physical)", VRNH_ReloadMode());
        } else if (!_stricmp(key, "vr_native_hands_l_off_fwd") || !_stricmp(key, "vr_native_hands_l_off_left")
                || !_stricmp(key, "vr_native_hands_l_off_up") || !_stricmp(key, "vr_native_hands_l_pitch")
                || !_stricmp(key, "vr_native_hands_l_yaw") || !_stricmp(key, "vr_native_hands_l_roll")
                || !_stricmp(key, "vr_native_hands_r_off_fwd") || !_stricmp(key, "vr_native_hands_r_off_left")
                || !_stricmp(key, "vr_native_hands_r_off_up") || !_stricmp(key, "vr_native_hands_r_pitch")
                || !_stricmp(key, "vr_native_hands_r_yaw") || !_stricmp(key, "vr_native_hands_r_roll")) {
            const int hand = !_strnicmp(key, "vr_native_hands_l_", 18) ? VRI_LEFT : VRI_RIGHT;
            if (!_stricmp(key, "vr_native_hands_l_off_fwd")  || !_stricmp(key, "vr_native_hands_r_off_fwd"))
                g_cfgNHCalPos[hand][0] = val;
            if (!_stricmp(key, "vr_native_hands_l_off_left") || !_stricmp(key, "vr_native_hands_r_off_left"))
                g_cfgNHCalPos[hand][1] = val;
            if (!_stricmp(key, "vr_native_hands_l_off_up")   || !_stricmp(key, "vr_native_hands_r_off_up"))
                g_cfgNHCalPos[hand][2] = val;
            if (!_stricmp(key, "vr_native_hands_l_pitch")    || !_stricmp(key, "vr_native_hands_r_pitch"))
                g_cfgNHCalRot[hand][0] = val;
            if (!_stricmp(key, "vr_native_hands_l_yaw")      || !_stricmp(key, "vr_native_hands_r_yaw"))
                g_cfgNHCalRot[hand][1] = val;
            if (!_stricmp(key, "vr_native_hands_l_roll")     || !_stricmp(key, "vr_native_hands_r_roll"))
                g_cfgNHCalRot[hand][2] = val;
            VRNH_SetCalibration(hand, g_cfgNHCalPos[hand], g_cfgNHCalRot[hand]);
            Log("config: native hands calibration %s = %.3f", key, val);
        } else if (!_stricmp(key, "vr_gun_off_fwd") || !_stricmp(key, "vr_gun_off_left")
                || !_stricmp(key, "vr_gun_off_up") || !_stricmp(key, "vr_gun_pitch")
                || !_stricmp(key, "vr_gun_yaw") || !_stricmp(key, "vr_gun_roll")) {
            // The weapon's own trim on top of the right palm's calibration
            // (VRNH_SetGunCalibration): offsets in metres, angles in degrees.
            if (!_stricmp(key, "vr_gun_off_fwd"))  g_cfgGunCalPos[0] = val;
            if (!_stricmp(key, "vr_gun_off_left")) g_cfgGunCalPos[1] = val;
            if (!_stricmp(key, "vr_gun_off_up"))   g_cfgGunCalPos[2] = val;
            if (!_stricmp(key, "vr_gun_pitch"))    g_cfgGunCalRot[0] = val;
            if (!_stricmp(key, "vr_gun_yaw"))      g_cfgGunCalRot[1] = val;
            if (!_stricmp(key, "vr_gun_roll"))     g_cfgGunCalRot[2] = val;
            VRNH_SetGunCalibration(g_cfgGunCalPos, g_cfgGunCalRot);
            Log("config: gun trim %s = %.3f", key, val);
        } else if (!_stricmp(key, "supersample")) {
            VR_SetSupersample(val);
            Log("config: supersample = %.2fx", val);
        } else if (!_stricmp(key, "replay")) {
            g_replayEnabled = val != 0;
            Log("config: scene replay = %s", g_replayEnabled ? "on" : "OFF");
        } else if (!_stricmp(key, "replayAdd")) {
            // Add one syscall to the entity-add capture set. Repeatable, one
            // per line. Lets the next run cover an entity add the default set
            // missed without needing a rebuild -- the log's per-syscall replay
            // breakdown is what tells us whether a new id is worth adding.
            int n = (int)val;
            if (n >= 0 && n < MAX_SYSCALL) {
                g_isReplaySyscall[n] = true;
                Log("config: replayAdd syscall %d", n);
            }
        }

        // ------------------------------ native VR interaction settings ----
        else if (ParseBindConfig(key, text)) {
            // logged inside
        } else if (!_stricmp(key, "vr_dominant_hand")) {
            const int hand = !_stricmp(text, "left") ? VRI_LEFT : VRI_RIGHT;
            VRWeapon_SetDominantHand(hand);
            Log("config: dominant hand = %s", hand == VRI_LEFT ? "left" : "right");
        } else if (!_stricmp(key, "vr_aim_mode")) {
            VRWeapon_SetEnabled(val != 0);
            Log("config: controller aim mode = %s", val != 0 ? "on" : "OFF (legacy head-aim)");
        } else if (!_stricmp(key, "vr_grenade_hold")) {
            VRBinds_SetGrenadeHold(val);
            Log("config: vr_grenade_hold = %.2f s (off-hand trigger must be held "
                "this long; a reflex squeeze does nothing)", val);
        } else if (!_stricmp(key, "vr_weapon_cycle_binding")) {
            const int legacy = !_stricmp(text,"x_hold") ? 2
                             : (!_stricmp(text,"stick") || val==1.0f) ? 1 : 0;
            VRBinds_SetWeaponCycleBinding(legacy);
            Log("config: vr_weapon_cycle_binding = %s", legacy==2 ? "X tap reload / hold cycle" : legacy
                ? "LEGACY dominant stick Y -- fires on diagonal turn flicks, not recommended"
                : "off-hand X button (default)");
        } else if (!_stricmp(key, "vr_weapon_transform")) {
            // RETIRED. The transform this used to enable wrote through
            // syscalls 37/38, which are CM_PointContents /
            // CM_TransformedPointContents on &cg.refdef.vieworg -- i.e. it
            // rewrote the CAMERA, not the gun. The key is accepted so old
            // configs still load, but it can no longer turn anything on.
            VRWeapon_SetTransformEnabled(0);
            Log("config: weapon entity transform = RETIRED "
                "(37/38 are collision queries on the camera; ignoring '%s')", text);
        } else if (!_stricmp(key, "vr_experimental_weapon")) {
            VRBinds_SetExperimentalWeapon(val != 0);
            Log("config: experimental weapon control (controller view steering) = %s",
                val != 0 ? "ON -- NOT the camera-stable baseline" : "OFF (baseline)");
        } else if (!_stricmp(key, "vr_weapon_anchor")) {
            const int keep = _stricmp(text, "hand") != 0;
            VRWeapon_SetAnchorMode(keep);
            Log("config: weapon anchor = %s", keep ? "model (keep baked offset)" : "hand");
        } else if (!_stricmp(key, "vr_weapon_off_fwd") || !_stricmp(key, "vr_weapon_off_left")
                || !_stricmp(key, "vr_weapon_off_up") || !_stricmp(key, "vr_weapon_pitch")
                || !_stricmp(key, "vr_weapon_yaw") || !_stricmp(key, "vr_weapon_roll")) {
            if (!_stricmp(key, "vr_weapon_off_fwd"))  g_cfgCalPos[0] = val;
            if (!_stricmp(key, "vr_weapon_off_left")) g_cfgCalPos[1] = val;
            if (!_stricmp(key, "vr_weapon_off_up"))   g_cfgCalPos[2] = val;
            if (!_stricmp(key, "vr_weapon_pitch"))    g_cfgCalRot[0] = val;
            if (!_stricmp(key, "vr_weapon_yaw"))      g_cfgCalRot[1] = val;
            if (!_stricmp(key, "vr_weapon_roll"))     g_cfgCalRot[2] = val;
            VRWeapon_SetCalibration(g_cfgCalPos[0], g_cfgCalPos[1], g_cfgCalPos[2],
                                    g_cfgCalRot[0], g_cfgCalRot[1], g_cfgCalRot[2]);
            Log("config: weapon calibration %s = %.2f", key, val);
        } else if (!_stricmp(key, "vr_physical_ads")) {
            VRWeapon_SetPhysicalAds(val != 0);
            Log("config: physical ADS = %s", val != 0 ? "on" : "OFF");
        } else if (!_stricmp(key, "vr_ads_enter_dist") || !_stricmp(key, "vr_ads_exit_dist")
                || !_stricmp(key, "vr_ads_enter_angle") || !_stricmp(key, "vr_ads_exit_angle")) {
            if (!_stricmp(key, "vr_ads_enter_dist"))  g_cfgAds[0] = val;
            if (!_stricmp(key, "vr_ads_exit_dist"))   g_cfgAds[1] = val;
            if (!_stricmp(key, "vr_ads_enter_angle")) g_cfgAds[2] = val;
            if (!_stricmp(key, "vr_ads_exit_angle"))  g_cfgAds[3] = val;
            VRWeapon_SetAdsThresholds(g_cfgAds[0], g_cfgAds[1], g_cfgAds[2], g_cfgAds[3]);
            Log("config: %s = %.2f", key, val);
        } else if (!_stricmp(key, "vr_twohand") || !_stricmp(key, "vr_twohand_enabled")
                || !_stricmp(key, "vr_twohand_dist")
                || !_stricmp(key, "vr_twohand_blend") || !_stricmp(key, "vr_twohand_blend_time")) {
            if (!_stricmp(key, "vr_twohand") || !_stricmp(key, "vr_twohand_enabled"))
                g_cfgTwoHand = (val != 0);
            if (!_stricmp(key, "vr_twohand_dist"))  g_cfgTwoHandDist = val;
            if (!_stricmp(key, "vr_twohand_blend") || !_stricmp(key, "vr_twohand_blend_time"))
                g_cfgTwoHandBlend = val;
            VRWeapon_SetTwoHand(g_cfgTwoHand, g_cfgTwoHandDist, g_cfgTwoHandBlend);
            Log("config: %s = %.2f", key, val);
        } else if (!_stricmp(key, "vr_twohand_grab_radius")
                || !_stricmp(key, "vr_twohand_release_radius")) {
            if (!_stricmp(key, "vr_twohand_grab_radius"))    g_cfgTwoHandGrab = val;
            if (!_stricmp(key, "vr_twohand_release_radius")) g_cfgTwoHandRelease = val;
            VRWeapon_SetTwoHandRadii(g_cfgTwoHandGrab, g_cfgTwoHandRelease);
            Log("config: %s = %.2f m", key, val);
        } else if (!_stricmp(key, "vr_ads_hint")) {
            VRBinds_SetAdsHintCorrection(val != 0);
            Log("config: ADS cgame-hint correction = %s", val != 0 ? "on" : "off (log-only)");
        } else if (!_stricmp(key, "cgameAdsHintRva")) {
            g_cgameAdsHintRva = (DWORD)strtoul(text, nullptr, 0);   // takes 0x...
            Log("config: cgame ADS hint RVA = 0x%lX%s", (unsigned long)g_cgameAdsHintRva,
                g_cgameAdsHintRva ? "" : " (disabled)");
        } else if (!_stricmp(key, "vr_haptics") || !_stricmp(key, "vr_haptics_strength")) {
            if (!_stricmp(key, "vr_haptics"))          g_cfgHaptics = (val != 0);
            if (!_stricmp(key, "vr_haptics_strength")) g_cfgHapticStrength = val;
            VRBinds_SetHaptics(g_cfgHaptics, g_cfgHapticStrength);
            Log("config: %s = %.2f", key, val);
        } else if (!_stricmp(key, "vr_turn_mode")) {
            const int mode = !_stricmp(text,"off") ? 2 : !_stricmp(text,"snap") ? 1 : 0;
            VRBinds_SetTurnMode(mode);
            Log("config: stick turn mode = %s", mode==2 ? "off" : mode==1 ? "snap" : "smooth");
        } else if (!_stricmp(key, "vr_snap_angle")) {
            VRBinds_SetSnapAngle(val);
            Log("config: snap angle = %.0f deg", val);
        } else if (!_stricmp(key, "vr_smooth_turn_speed")) {
            VRBinds_SetSmoothTurnSpeed(val);
            Log("config: smooth turn speed = %.0f deg/sec", val);
        } else if (!_stricmp(key, "vr_move_direction")) {
            const int off = !_stricmp(text, "offhand");
            VRBinds_SetMoveDirection(off);
            Log("config: move direction = %s", off ? "off-hand" : "hmd");
        } else if (!_stricmp(key, "vr_physical_crouch") || !_stricmp(key, "vr_crouch_drop")) {
            if (!_stricmp(key, "vr_physical_crouch")) g_cfgPhysCrouch = (val != 0);
            if (!_stricmp(key, "vr_crouch_drop"))     g_cfgCrouchDrop = val;
            VRBinds_SetPhysicalCrouch(g_cfgPhysCrouch, g_cfgCrouchDrop);
            Log("config: %s = %.2f", key, val);
        } else if (!_stricmp(key, "vr_height_adjust")) {
            VR_SetHeightAdjust(val);
            Log("config: height adjust = %.2f m", val);
        } else if (!_stricmp(key, "vr_comfort_vignette")) {
            Comfort_SetVignette(val != 0);
            Log("config: comfort vignette = %s (vr_comfort_vignette)", val != 0 ? "on" : "off");
        } else if (!_stricmp(key, "vr_ui_scale") || !_stricmp(key, "vr_ui_safe_x")
                || !_stricmp(key, "vr_ui_safe_y")) {
            float sc, sx, sy;
            VR_GetUiSafeArea(&sc, &sx, &sy);
            if (!_stricmp(key, "vr_ui_scale"))  sc = val;
            if (!_stricmp(key, "vr_ui_safe_x")) sx = val;
            if (!_stricmp(key, "vr_ui_safe_y")) sy = val;
            VR_SetUiSafeArea(sc, sx, sy);
            VR_GetUiSafeArea(&sc, &sx, &sy);
            Log("config: VR UI safe area scale %.2f safe x %.2f y %.2f (menus, loading, HUD; world untouched)",
                sc, sx, sy);
        } else if (!_stricmp(key, "vr_menu_scale")) {
            VR_SetMenuScale(val);
            Log("config: menu safe-area scale = %.2f", val);
        } else if (!_stricmp(key, "vr_menu_distance")) {
            VR_SetMenuDistance(val);
            Log("config: menu distance = %.1f m%s", val, val <= 0.0f ? " (infinity)" : "");
        } else if (!_stricmp(key, "vr_hud_scale")) {
            if (val > 0.3f && val <= 1.0f) g_hudScale = val;
            Log("config: HUD safe-area scale = %.2f", g_hudScale);
        } else if (!_stricmp(key, "vr_hud_horizontal_offset")) {
            if (val >= -0.25f && val <= 0.25f) g_hudOffsetX = val;
            Log("config: HUD horizontal offset = %+.3f of screen", g_hudOffsetX);
        } else if (!_stricmp(key, "vr_hud_vertical_offset")) {
            if (val >= -0.25f && val <= 0.25f) g_hudOffsetY = val;
            Log("config: HUD vertical offset = %+.3f of screen", g_hudOffsetY);
        } else if (!_stricmp(key, "vr_steer_gain") || !_stricmp(key, "vr_steer_cpd")) {
            if (!_stricmp(key, "vr_steer_gain")) g_cfgSteerGain = val;
            if (!_stricmp(key, "vr_steer_cpd"))  g_cfgSteerCpd = val;
            VRBinds_SetSteering(g_cfgSteerGain, g_cfgSteerCpd);
            Log("config: %s = %.2f", key, val);
        } else if (!_stricmp(key, "vr_menu_cursor_speed")) {
            VRBinds_SetMenuCursorSpeed(val);
            Log("config: menu cursor speed = %.0f px/sec", val);
        }
}


static void LoadConfig()
{
    // Start from the production contract, not from whatever the last run
    // or the loader left behind. A config that fails to open, fails to
    // parse, or simply is not there must land on these values.
    ProbeCfg_Production(&g_probeCfg);

    char path[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                       | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&LoadConfig, &self);
    // EVERY exit path below must still enforce the baseline. A missing or
    // unreadable codvr.cfg must not be able to ship the dangerous
    // configuration -- the safe state is what you get with NO configuration
    // at all, not something a successfully-parsed key has to switch on.
    if (!GetModuleFileNameA(self, path, MAX_PATH)) {
        EnforceCameraStableBaseline();
        return;
    }
    char* slash = strrchr(path, '\\');
    if (!slash) {
        EnforceCameraStableBaseline();
        return;
    }
    strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "codvr.cfg");
    {
        const size_t dirLen = (size_t)(slash + 1 - path);
        memcpy(g_configDir, path, dirLen);
        g_configDir[dirLen] = 0;
    }

    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) {
        Log("config: none at %s (using defaults)", path);
        EnforceCameraStableBaseline();
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '/' || line[0] == '\n')
            continue;
        // Every setting is "name value". Most values are numbers, but key
        // bindings are names ("F9", "HOME", "Q") -- nobody should have to look
        // up a virtual-key code to rebind recenter. So read the value as text
        // first and convert to a number only when it looks like one.
        char key[64], text[64];
        if (sscanf_s(line, "%63s %63s", key, (unsigned)sizeof(key),
                     text, (unsigned)sizeof(text)) != 2)
            continue;
        const float val = (float)atof(text);

        VRMenu_NoteConfig(key, text, val);
        ApplyConfigKey(key, text, val);
    }
    fclose(f);

    VRMenu_Init(path, &ApplyConfigKey);
    VRSnap_Init(path);
    EnforceCameraStableBaseline();
}


// ---------------------------------------------------------------------------
// CAMERA-STABLE BASELINE ENFORCEMENT
// ---------------------------------------------------------------------------
//
// Runs AFTER the whole config file is parsed, so it cannot be defeated by key
// ordering. Unless vr_experimental_weapon is explicitly on, every path that
// lets controller input write the game's view angles is forced off here --
// regardless of what any individual key asked for.
//
// The 2026-08-23 hardware run shipped with 'weapon entity transform = on' and
// controller steering live, and was physically intolerable. The rule this
// encodes: a weapon feature may never be the reason the camera moves.
// Put the probe system back to the production contract unless the config
// explicitly armed it. Runs after the whole file is parsed, so no ordering
// of keys can leave a player with a flat, self-closing build.
static void EnforceProbeProductionContract()
{
    const int wasArmed = g_probeCfg.armed;
    ProbeCfg_Enforce(&g_probeCfg);
    // Push whatever survived to the renderer. For an unarmed config that
    // is GLSC_NONE with a zero delta, i.e. the probe is switched off at
    // the layer as well as in the struct.
    ApplyGLProbe();
    if (wasArmed) {
        Log("");
        Log("*** PROBE ARMED -- this is an attended capture build, not a "
            "production one ***");
        Log("    vr_disable=%d  sweep=%d  autoquit=%d  probe class=%s",
            g_probeCfg.vrDisabled, g_probeCfg.sweep, g_probeCfg.autoQuit,
            GLSurf_ClassName(g_probeCfg.probeClass));
        // Milestone: the parsed probe configuration, machine-checkable.
        Log("");
        Log("=== VBOGAME PROBE CONFIG ===");
        Log("armed: YES");
        Log("handsolve: %s", g_probeCfg.handSolveProbe ? "YES" : "NO");
        Log("VBO shadow: YES (compiled in; wrappers install at first getproc)");
        Log("capture: frames=%d skip=%d quit_after=%d tag=%s autoquit=%d",
            g_probeCfg.capFrames, g_probeCfg.capSkip, g_probeCfg.quitAfter,
            g_glcapTag, g_probeCfg.autoQuit);
        Log("============================");
        return;
    }
    Log("config: PRODUCTION PROBE CONTRACT -- vr_disable=0 sweep=0 "
        "autoquit=0 probe class=none (verified %s)",
        ProbeCfg_IsProduction(&g_probeCfg) ? "OK" : "*** VIOLATED ***");
}

static void EnforceCameraStableBaseline()
{
    // Probe contract first, and outside every conditional below it: an
    // unarmed config must reach the production values whatever else is on.
    EnforceProbeProductionContract();

    // The 37/38 transform is retired outright -- not gated, not toggleable.
    VRWeapon_SetTransformEnabled(0);

    if (VRBinds_ExperimentalWeapon()) {
        Log("");
        Log("======= CODVR PHYSICAL AIM =======");
        Log("  camera: body basis (stick turn / recenter only) + HMD; the game's");
        Log("          view angles never reach the rendered camera");
        Log("  aim   : rendered barrel -> yaw/pitch offset vs body -> game view");
        Log("          angles via the ordinary aim input; CoD fires as usual");
        Log("  watch : steering-leak watchdog live (STEERING LEAK lines = fault)");
        Log("==================================");
        return;
    }

    // Baseline: HMD owns the camera; controllers move and shoot.
    VRWeapon_SetEnabled(0);                       // no decoupled body basis
    VRWeapon_SetTwoHand(0, g_cfgTwoHandDist, g_cfgTwoHandBlend);
    VRWeapon_SetPhysicalAds(0);                   // depends on the weapon pose

    Log("");
    Log("======= CODVR CAMERA-STABLE BASELINE =======");
    Log("  camera source of truth: game base pose + HMD centre + runtime eye offsets");
    Log("  ONE base camera latched per XR pair; nothing sampled between the");
    Log("  two game frames of a pair can reach the camera.");
    Log("  ENABLED : asymmetric OpenXR projection, 6DoF head tracking,");
    Log("            F1 pair scheduler, controller locomotion + buttons,");
    Log("            HUD/menu, comfort settings");
    Log("  DISABLED: 37/38 weapon transform (RETIRED), controller mouse");
    Log("            steering, two-hand weapon control, physical ADS");
    Log("  set vr_experimental_weapon 1 to re-arm (not recommended)");
    Log("===========================================");
}

// ------------------------------------------------------------- self test

// Offline exercise of the parts that are easy to get wrong and expensive to
// debug inside the game: the naked assembly interposer, argument passing,
// and save/restore of the struct. Runs without a GL context or the engine.
static int   g_fakeCalls;
static int   g_fakeW[4], g_fakeX[4];
static float g_fakeZ[4];
static float g_fakeOrg[4];     // float at +0: the entity origin under replay
static int   g_fakeA1[4];      // second argument, which replay must preserve

static int __cdecl FakeSyscall(int num, ...)
{
    va_list ap;
    va_start(ap, num);
    void* p = va_arg(ap, void*);
    int   a1 = va_arg(ap, int);
    va_end(ap);
    if (g_fakeCalls < 4 && p) {
        g_fakeW[g_fakeCalls] = ((int*)p)[RD_W];
        g_fakeX[g_fakeCalls] = ((int*)p)[RD_X];
        g_fakeZ[g_fakeCalls] = ((float*)p)[RD_ORG+2];
        g_fakeOrg[g_fakeCalls] = ((float*)p)[0];
        g_fakeA1[g_fakeCalls] = a1;
    }
    ++g_fakeCalls;
    return 0x5EED;
}

static const unsigned char* WINAPI TestNoAnisotropyExtensions(unsigned)
{
    return (const unsigned char*)"GL_EXT_texture_filter_anisotropic_suffix";
}

static void MakeRefdef(BYTE* buf)
{
    memset(buf, 0, SNAP_BYTES);
    int*   iv = (int*)buf;
    float* fv = (float*)buf;
    iv[RD_X] = 0; iv[RD_Y] = 0; iv[RD_W] = 1920; iv[RD_H] = 1080;
    fv[RD_FOVX] = 96.4f; fv[RD_FOVY] = 64.4f;
    fv[RD_ORG] = 100.0f; fv[RD_ORG+1] = 200.0f; fv[RD_ORG+2] = 300.0f;
    // 30-degree yaw: a genuine rotation, not the identity we deliberately reject
    const float c = 0.8660254f, s = 0.5f;
    float* m = (float*)(buf + RD_AXIS_BYTES);
    m[0]= c; m[1]= s; m[2]=0;
    m[3]=-s; m[4]= c; m[5]=0;
    m[6]= 0; m[7]= 0; m[8]=1;
}

void SelfTest()
{
    int pass = 0, fail = 0;
    #define CHECK(cond, name) do { \
        if (cond) { Log("  PASS  %s", name); ++pass; } \
        else      { Log("  FAIL  %s", name); ++fail; } } while (0)

    Log("=== SELF TEST ===");

    CHECK(fabsf(TwoHandAdsScaledSpread(4.0f) - 1.0f) < 0.0001f,
          "two-hand ADS reduces only the random spread component by 75 percent");
    CHECK(TwoHandAdsCondition(VRNH_2H_ATTACHED, 1, 0.5f),
          "two-hand ADS gate accepts attached plus active ADS");
    CHECK(!TwoHandAdsCondition(VRNH_2H_LEFT_FREE, 1, 0.5f)
          && !TwoHandAdsCondition(VRNH_2H_ATTACHED, 1, 0.0f)
          && !TwoHandAdsCondition(VRNH_2H_ATTACHED, 0, 0.5f),
          "two-hand ADS gate rejects one-hand, hip-fire, and missing game state");

    // The phase tests below exercise probe-mode behaviour specifically.
    const bool savedProbe = Probe_Enabled();
    Probe_SetEnabled(true);

    BYTE rd[SNAP_BYTES];
    MakeRefdef(rd);

    CHECK(IsRefdef(rd), "IsRefdef accepts a well-formed refdef");
    CHECK(FindOrthonormal(rd, SNAP_BYTES) == RD_AXIS_BYTES,
          "viewaxis located at byte offset 36");

    BYTE junk[SNAP_BYTES];
    memset(junk, 0xCD, sizeof(junk));
    CHECK(!IsRefdef(junk), "IsRefdef rejects garbage");
    CHECK(!IsRefdef(nullptr), "IsRefdef rejects null");

    // The vbogame banner's DLL self-hash must be trustworthy or the harness
    // would trust a wrong identity claim. Pin it to the FIPS-180 "abc" vector
    // and prove it fails closed on a missing file.
    {
        char tmpdir[MAX_PATH] = { 0 };
        char tf[MAX_PATH] = { 0 };
        char hex[65] = { 0 };
        GetTempPathA(MAX_PATH, tmpdir);
        sprintf_s(tf, "%scodvr_sha_selftest.tmp", tmpdir);
        FILE* shaf = nullptr;
        if (fopen_s(&shaf, tf, "wb") == 0 && shaf) {
            fwrite("abc", 1, 3, shaf);
            fclose(shaf);
        }
        CHECK(Sha256File(tf, hex)
              && _stricmp(hex, "BA7816BF8F01CFEA414140DE5DAE2223"
                               "B00361A396177A9CB410FF61F20015AD") == 0,
              "Sha256File matches the FIPS-180 'abc' vector");
        DeleteFileA(tf);
        CHECK(!Sha256File(tf, hex),
              "Sha256File fails closed on a missing file");
    }

    g_realSyscall = (SyscallFn)FakeSyscall;
    SyscallFn hook = (SyscallFn)SyscallHook;

    // Passthrough: an unrelated syscall must reach the engine untouched.
    g_fakeCalls = 0; Probe_TestSetPhase(PROBE_PH_BASE0);
    int rv = hook(999, rd);
    CHECK(g_fakeCalls == 1 && rv == 0x5EED, "unrelated syscall passes through");
    CHECK(g_fakeW[0] == 1920, "passthrough leaves width untouched");

    // Baseline phase: candidate diverts through our C handler but unmodified.
    g_fakeCalls = 0; Probe_TestSetPhase(PROBE_PH_BASE0);
    rv = hook(71, rd);
    CHECK(g_fakeCalls == 1 && rv == 0x5EED, "baseline forwards exactly once");
    CHECK(g_fakeW[0] == 1920, "baseline leaves width untouched");

    // Halve phase on A: one call, width halved, struct restored afterwards.
    g_fakeCalls = 0; Probe_TestSetPhase(PROBE_PH_HALF_A);
    rv = hook(71, rd);
    CHECK(g_fakeCalls == 1, "halve phase renders once");
    CHECK(g_fakeW[0] == 960, "halve phase passes half width");
    CHECK(((int*)rd)[RD_W] == 1920 && ((int*)rd)[RD_X] == 0,
          "struct restored after halve");

    // Halve phase must not touch the other candidate.
    g_fakeCalls = 0; Probe_TestSetPhase(PROBE_PH_HALF_A);
    hook(243, rd);
    CHECK(g_fakeCalls == 1 && g_fakeW[0] == 1920, "halve on A ignores B");

    // Double phase: two renders, second offset in x and lifted in z.
    g_fakeCalls = 0; Probe_TestSetPhase(PROBE_PH_DBL_A);
    rv = hook(71, rd);
    CHECK(g_fakeCalls == 2, "double phase renders twice");
    CHECK(g_fakeW[0] == 960 && g_fakeW[1] == 960, "both halves get half width");
    CHECK(g_fakeX[0] == 0 && g_fakeX[1] == 960, "second render offset to right half");
    CHECK(fabsf(g_fakeZ[0] - 300.0f) < 0.01f
          && fabsf(g_fakeZ[1] - 800.0f) < 0.01f, "second render camera displaced");
    CHECK(((int*)rd)[RD_W] == 1920 && ((int*)rd)[RD_X] == 0
          && fabsf(((float*)rd)[RD_ORG+2] - 300.0f) < 0.01f,
          "struct fully restored after double render");
    CHECK(rv == 0x5EED, "return value comes from the first render");

    // A non-refdef argument must never be modified, even for a candidate.
    g_fakeCalls = 0; Probe_TestSetPhase(PROBE_PH_HALF_A);
    hook(71, junk);
    CHECK(g_fakeCalls == 1, "non-refdef argument still forwarded");

    // --- entity classifier, against the REAL captured layout ---
    // These bytes are the shape recovered from a live capture: float origin at
    // +0, orthonormal axis at +12. The previous classifier looked for a Quake 3
    // reType int at +0 and matched nothing at all, so pin the real layout here.
    {
        BYTE ent[REPLAY_SNAP];
        memset(ent, 0, sizeof(ent));
        float* ef = (float*)ent;
        ef[0] = -396.997f; ef[1] = 7259.128f; ef[2] = -423.333f;   // origin
        float* ax = (float*)(ent + ENT_AXIS_OFF);
        ax[0]=-0.738f; ax[1]=-0.496f; ax[2]=-0.457f;
        ax[3]= 0.558f; ax[4]=-0.830f; ax[5]= 0.000f;
        ax[6]=-0.379f; ax[7]=-0.255f; ax[8]= 0.890f;
        CHECK(ReplayIsEntity(ent), "classifier accepts the real captured entity");

        // An unrotated entity carries an identity axis; it must NOT be
        // rejected the way the refdef scan rejects zeroed memory.
        BYTE idEnt[REPLAY_SNAP];
        memset(idEnt, 0, sizeof(idEnt));
        float* idf = (float*)idEnt;
        idf[0] = 10.0f; idf[1] = 20.0f; idf[2] = 30.0f;
        float* ida = (float*)(idEnt + ENT_AXIS_OFF);
        ida[0]=1; ida[4]=1; ida[8]=1;
        CHECK(ReplayIsEntity(idEnt), "classifier accepts an unrotated entity");

        // Syscall 7's argument was small ints and zeroes -- not an entity.
        BYTE notEnt[REPLAY_SNAP];
        memset(notEnt, 0, sizeof(notEnt));
        ((int*)notEnt)[0] = 491; ((int*)notEnt)[1] = 1; ((int*)notEnt)[4] = 48;
        CHECK(!ReplayIsEntity(notEnt), "classifier rejects syscall 7's argument");
        CHECK(!ReplayIsEntity(junk), "classifier rejects garbage");
    }

    // --- scene replay: capture entity adds, re-issue only entity-shaped ---
    // rd is entity-shaped (reType int at offset 0 is 0, orthonormal basis at
    // 36); junk is 0xCD-filled so its reType is out of range and it has no
    // basis -- exactly the case that would ERR_DROP if replayed.
    {
        g_realSyscall = (SyscallFn)FakeSyscall;
        memset(g_isReplaySyscall, 0, sizeof(g_isReplaySyscall));
        g_isReplaySyscall[34] = true;                 // pretend 34 is the add
        g_replayCapture = true;
        InterlockedExchange(&g_replayCount, 0);

        // Build a real entity (origin +0, axis +12) with a recognisable origin
        // so we can prove the SNAPSHOT is replayed, not the caller's pointer.
        BYTE liveEnt[REPLAY_SNAP];
        memset(liveEnt, 0, sizeof(liveEnt));
        float* lf = (float*)liveEnt;
        lf[0] = 1234.5f; lf[1] = 6789.0f; lf[2] = 42.0f;
        float* la = (float*)(liveEnt + ENT_AXIS_OFF);
        la[0]=1; la[4]=1; la[8]=1;

        // Arguments beyond the first matter: syscall 37 takes 2 and syscall 38
        // takes 4, and replaying zeros in those slots crashed the game.
        int entArgs[REPLAY_ARGS]  = { (int)(uintptr_t)liveEnt, 0xA1, 0xA2, 0xA3, 0, 0, 0, 0 };
        int junkArgs[REPLAY_ARGS] = { (int)(uintptr_t)junk,    0, 0, 0, 0, 0, 0, 0 };

        NoteSyscall(34, entArgs);                     // entity-shaped, in set
        NoteSyscall(34, junkArgs);                    // in set, not an entity
        NoteSyscall(99, entArgs);                     // entity-shaped, not in set
        CHECK(g_replayCount == 2, "replay captures only in-set syscalls");

        // Overwrite the caller's buffer: a correct replay uses its own copy.
        lf[0] = -999.0f;

        g_fakeCalls = 0; g_fakeOrg[0] = 0; g_fakeA1[0] = 0;
        ReplayCapturedEntities();
        // The whole in-set sequence is replayed in order -- dropping the
        // non-entity calls would strip the state-setting half of the
        // 34/38 pairing, which is what left the second eye empty.
        CHECK(g_fakeCalls == 2, "replay re-issues the whole captured sequence");
        CHECK(fabsf(g_fakeOrg[0] - 1234.5f) < 0.01f,
              "replay carries the snapshotted struct, not a stale ptr");
        CHECK(g_fakeA1[0] == 0xA1,
              "replay forwards later arguments instead of zeroing them");

        // A non-pointer argument must survive replay untouched: the 34 half of
        // the pair carries a plain integer, and substituting a snapshot
        // pointer there would corrupt the state it sets.
        InterlockedExchange(&g_replayCount, 0);
        int intArgs[REPLAY_ARGS] = { 0x1234, 0xB1, 0, 0, 0, 0, 0, 0 };
        NoteSyscall(34, intArgs);
        CHECK(g_replayCount == 1, "a non-pointer argument is still captured");
        CHECK(!g_replay[0].hasSnap, "no snapshot taken for a non-pointer argument");

        // The kill switch must actually stop replay.
        g_replayEnabled = false;
        g_fakeCalls = 0;
        ReplayCapturedEntities();
        CHECK(g_fakeCalls == 0, "replay kill switch suppresses re-issue");
        g_replayEnabled = true;

        // Captures must be scoped to one scene. The probe path shares the
        // render hook's restore-and-reset tail, so a completed render call is
        // what clears the buffer -- otherwise a second scene in the same frame
        // would replay the first scene's entities on top of its own.
        InterlockedExchange(&g_replayCount, 0);
        int rdArgs[REPLAY_ARGS] = { (int)(uintptr_t)rd, 0, 0, 0, 0, 0, 0, 0 };
        NoteSyscall(34, rdArgs);
        CHECK(g_replayCount == 1, "capture accumulates within a scene");
        Probe_TestSetPhase(PROBE_PH_DBL_A);
        hook(71, rd);                                 // completes a render call
        CHECK(g_replayCount == 0, "capture resets when a scene finishes");

        g_replayCapture = false;
        memset(g_isReplaySyscall, 0, sizeof(g_isReplaySyscall));
        InterlockedExchange(&g_replayCount, 0);
    }

    // --- collision-query transparency (the asm's third branch) ---
    // Syscalls 37 and 38 must reach the engine UNTOUCHED. They are
    // CM_PointContents / CM_TransformedPointContents on &cg.refdef.vieworg,
    // so the old "entity rewrite" was a camera rewrite -- see
    // RETIRED_INVALID_37_38_WEAPON_PATH. The production divert set is empty;
    // the divert branch itself survives only so the self-test can prove a
    // flagged id still forwards exactly once and mutates nothing.
    {
        VRWeapon_TestReset();
        g_realSyscall = (SyscallFn)FakeSyscall;

        BYTE ent[REPLAY_SNAP];
        memset(ent, 0, sizeof(ent));
        float* eo = (float*)ent;
        eo[0] = 100.0f; eo[1] = 200.0f; eo[2] = 50.0f;      // == vieworg
        float* ea = (float*)(ent + ENT_AXIS_OFF);
        ea[0] = 1; ea[4] = 1; ea[8] = 1;
        float* eang = (float*)(ent + ENT_ANGLES_OFF);
        eang[0] = eang[1] = eang[2] = 0.0f;

        const float ident[9] = { 1,0,0, 0,1,0, 0,0,1 };
        const float vieworg[3] = { 100.0f, 200.0f, 50.0f };
        VRWeapon_NoteGameAim(ident, vieworg, 1.0);

        // The struct above is deliberately shaped EXACTLY like the 2026-08-23
        // hardware failure: entity origin == vieworg, axis orthonormal. That
        // is what &cg.refdef.vieworg looks like to a hook, and it is what the
        // retired classifier scored as "the view weapon, 0.0 u away".
        //
        // PRODUCTION INVARIANT: syscalls 37 and 38 must not be diverted at
        // all. Check the real divert set first.
        SetupWeaponDivertSet();
        CHECK(g_entDivert[37] == 0 && g_entDivert[38] == 0,
              "entdivert: 37/38 are NOT diverted (RETIRED_INVALID_37_38_WEAPON_PATH)");
        {
            int flagged = 0;
            for (int n = 0; n < MAX_SYSCALL; ++n)
                if (g_entDivert[n]) ++flagged;
            CHECK(flagged == 0, "entdivert: production divert set is empty");
        }

        // 37 and 38 reach the engine untouched, with the camera bytes intact,
        // even while a live weapon pose exists. This is the regression that
        // must never go green again with a rewrite in place.
        float wAxis[9] = { 0,1,0, -1,0,0, 0,0,1 };
        const float wOrg[3] = { 300.0f, 400.0f, 60.0f };
        VRWeapon_TestForcePose(wOrg, wAxis);
        for (int n = 37; n <= 38; ++n) {
            eo[0] = 100.0f; eo[1] = 200.0f; eo[2] = 50.0f;
            memset(ea, 0, 9 * sizeof(float));
            ea[0] = 1; ea[4] = 1; ea[8] = 1;
            eang[0] = eang[1] = eang[2] = 0.0f;
            g_fakeCalls = 0;
            const int rv37 = hook(n, ent);
            CHECK(g_fakeCalls == 1 && rv37 == 0x5EED,
                  n == 37 ? "entdivert: syscall 37 forwards exactly once"
                          : "entdivert: syscall 38 forwards exactly once");
            CHECK(fabsf(eo[0] - 100.0f) < 1e-4f && fabsf(eo[1] - 200.0f) < 1e-4f
                  && fabsf(eo[2] - 50.0f) < 1e-4f,
                  n == 37 ? "entdivert: 37 leaves cg.refdef.vieworg BYTE-EXACT"
                          : "entdivert: 38 leaves cg.refdef.vieworg BYTE-EXACT");
            CHECK(fabsf(ea[0] - 1.0f) < 1e-4f && fabsf(ea[1]) < 1e-4f
                  && fabsf(ea[4] - 1.0f) < 1e-4f,
                  n == 37 ? "entdivert: 37 leaves cg.refdef.viewaxis BYTE-EXACT"
                          : "entdivert: 38 leaves cg.refdef.viewaxis BYTE-EXACT");
            CHECK(fabsf(eang[0]) < 1e-4f && fabsf(eang[1]) < 1e-4f,
                  n == 37 ? "entdivert: 37 writes no euler angles"
                          : "entdivert: 38 writes no euler angles");
            CHECK(fabsf(g_fakeOrg[0] - 100.0f) < 1e-4f,
                  n == 37 ? "entdivert: engine sees the game's own camera on 37"
                          : "entdivert: engine sees the game's own camera on 38");
        }

        // The divert machinery itself is still wired -- a flagged id still
        // reaches the branch -- but the branch is now a transparent
        // pass-through, so even a diverted call cannot mutate the struct.
        g_entDivert[36] = 1;    // a test id outside the render candidates
        eo[0] = 100.0f;
        g_fakeCalls = 0;
        const int erv = hook(36, ent);
        CHECK(g_fakeCalls == 1 && erv == 0x5EED,
              "entdivert: a flagged id still forwards exactly once");
        CHECK(fabsf(eo[0] - 100.0f) < 1e-4f,
              "entdivert: the divert branch mutates nothing");
        g_entDivert[36] = 0;

        CHECK(CODVR_CollisionHookCameraMutationCount() == 0,
              "entdivert: collisionHookCameraMutationCount == 0");
        VRWeapon_TestReset();
    }

    // Against the real module: locate the syscall slot the same way we will
    // at runtime, and check it lands inside cgamex86.dll's .data.
    // The game folder differs per machine, so it is not baked in: build.ps1
    // passes it in CODVR_GAME_DIR. Unset, this check logs a skip -- it is a
    // bonus pass against the real module, not a gate.
    char cgPath[MAX_PATH] = {0};
    const char* codvrGameDir = getenv("CODVR_GAME_DIR");
    if (codvrGameDir && *codvrGameDir) {
        snprintf(cgPath, sizeof(cgPath), "%s\\cgamex86.dll", codvrGameDir);
    }
    HMODULE cg = cgPath[0] ? LoadLibraryA(cgPath) : nullptr;
    if (cg) {
        SyscallFn* slot = FindSyscallSlot(cg);
        uintptr_t rva = slot ? (uintptr_t)slot - (uintptr_t)cg : 0;
        Log("  cgamex86.dll base %p, derived slot %p (RVA 0x%IX)", cg, slot, rva);
        CHECK(slot != nullptr, "syscall slot recovered from real cgamex86.dll");
        CHECK(rva == 0x657E8, "slot RVA matches the statically analysed 0x657E8");
        FreeLibrary(cg);
    } else {
        Log("  (cgamex86.dll not loadable here; skipping live-module check)");
    }

    // --- coordinate conversion, the part hardest to debug in a headset ---
    // OpenXR is +Y up with forward at -Z; Quake is +Z up with viewaxis rows
    // forward/left/up. An identity rotation must therefore come out as the
    // identity basis in Quake's convention -- if this is wrong, the world
    // appears rotated or mirrored and it is miserable to diagnose while wearing
    // the thing.
    {
        const float identity[4] = { 0, 0, 0, 1 };
        float axis[9];
        VR_TestQuatToAxis(identity, axis);
        bool okId = fabsf(axis[0]-1) < 1e-4f && fabsf(axis[1]) < 1e-4f && fabsf(axis[2]) < 1e-4f
                 && fabsf(axis[3]) < 1e-4f && fabsf(axis[4]-1) < 1e-4f && fabsf(axis[5]) < 1e-4f
                 && fabsf(axis[6]) < 1e-4f && fabsf(axis[7]) < 1e-4f && fabsf(axis[8]-1) < 1e-4f;
        CHECK(okId, "identity pose maps to the identity Quake basis");
        if (!okId)
            Log("        got fwd(%.3f %.3f %.3f) left(%.3f %.3f %.3f) up(%.3f %.3f %.3f)",
                axis[0],axis[1],axis[2], axis[3],axis[4],axis[5], axis[6],axis[7],axis[8]);

        // Yaw about OpenXR's +Y (which is Quake's up): the up row must not move.
        const float s = 0.3826834f, c = 0.9238795f;   // 45 degrees
        const float yaw[4] = { 0, s, 0, c };
        VR_TestQuatToAxis(yaw, axis);
        CHECK(fabsf(axis[6]) < 1e-4f && fabsf(axis[7]) < 1e-4f && fabsf(axis[8]-1) < 1e-4f,
              "yaw-only pose leaves the up vector along Quake +Z");

        // Whatever the rotation, the result must stay a valid basis, since the
        // engine's own orthonormality assumptions depend on it.
        const float tilt[4] = { 0.2f, 0.3f, 0.1f, 0.9273f };
        VR_TestQuatToAxis(tilt, axis);
        CHECK(FindOrthonormal((const BYTE*)axis, sizeof(axis)) == 0,
              "arbitrary pose still yields an orthonormal basis");
    }

    // --- aim model: mouse aims, the head free-looks on top ---
    // Mouse pitch used to be discarded entirely (only the game's yaw survived),
    // so aiming up or down moved the weapon but never the view.
    {
        const float ident[9] = { 1,0,0, 0,1,0, 0,0,1 };

        // A 30-degree upward game pitch: forward tilts up, up tilts back.
        const float cp = 0.8660254f, sp = 0.5f;
        const float gamePitchUp[9] = {  cp, 0, sp,
                                         0, 1,  0,
                                       -sp, 0, cp };

        // With the head centred, the view must be EXACTLY the game's aim --
        // this is the property that makes the gun sit where you look.
        float out[9];
        VR_TestComposeAxis(ident, gamePitchUp, out);
        bool same = true;
        for (int i = 0; i < 9; ++i)
            if (fabsf(out[i] - gamePitchUp[i]) > 1e-4f) same = false;
        CHECK(same, "aim: centred head leaves the game's aim untouched");

        // And the game's pitch must actually reach the view.
        CHECK(fabsf(out[2] - sp) < 1e-4f, "aim: mouse pitch reaches the view");

        // A neutral game aim with a pitched head gives the head's own view.
        float out2[9];
        VR_TestComposeAxis(gamePitchUp, ident, out2);
        bool headOnly = true;
        for (int i = 0; i < 9; ++i)
            if (fabsf(out2[i] - gamePitchUp[i]) > 1e-4f) headOnly = false;
        CHECK(headOnly, "aim: head rotation alone drives the view");

        // Composition must preserve orthonormality, or the engine's own
        // assumptions about the view basis break.
        const float yc = 0.7071068f, ys = 0.7071068f;
        const float headYaw[9] = {  yc, ys, 0,
                                   -ys, yc, 0,
                                     0,  0, 1 };
        float out3[9];
        VR_TestComposeAxis(headYaw, gamePitchUp, out3);
        CHECK(FindOrthonormal((const BYTE*)out3, sizeof(out3)) == 0,
              "aim: composed head+aim basis stays orthonormal");

        // Head yaw must rotate the view away from the aim, not cancel it.
        const float viewYaw = atan2f(out3[1], out3[0]);
        CHECK(fabsf(viewYaw) > 0.1f, "aim: head yaw turns the view off the aim");
    }

    // --- HUD aspect remap ---
    // A square render buffer would stretch the engine's wide 2D layout
    // vertically, so the HUD is remapped into a correctly-proportioned band.
    {
        const LONG savedW = g_viewW, savedH = g_viewH;
        const float savedAspect = g_hudAspect;
        g_viewW = 1440; g_viewH = 1440;      // the square buffer
        g_hudAspect = 1.7777f;

        int x = 0, y = 0, w = 1440, h = 1440;
        HudRemap(&x, &y, &w, &h);
        CHECK(w == 1440, "hud: full-width band keeps the screen width");
        CHECK(h == 810,  "hud: square screen letterboxes to 16:9 height");
        CHECK(y == 315,  "hud: the band is centred vertically");
        CHECK(x == 0,    "hud: no horizontal offset when width already fits");
        if (h != 810 || y != 315)
            Log("        got x=%d y=%d w=%d h=%d", x, y, w, h);

        // A sub-rectangle must scale and offset with the band, not jump out of it.
        int sx = 720, sy = 0, sw = 720, sh = 720;
        HudRemap(&sx, &sy, &sw, &sh);
        CHECK(sx == 720 && sw == 720, "hud: sub-rect keeps its horizontal place");
        CHECK(sh == 405, "hud: sub-rect scales vertically with the band");
        CHECK(sy == 315, "hud: sub-rect is offset into the band");

        // GAMEPLAY HUD safe area: during a 3D frame the band shrinks by
        // vr_hud_scale so HUD corners come in from the lens edge; menu
        // frames keep the full band (they have their own angular fit).
        {
            const bool savedStereo = g_vrDrewStereo;
            const float savedScale = g_hudScale;
            const float savedOffX = g_hudOffsetX, savedOffY = g_hudOffsetY;
            float savedUi, savedUx, savedUy;
            VR_GetUiSafeArea(&savedUi, &savedUx, &savedUy);
            VR_SetUiSafeArea(1.0f, 1.0f, 1.0f);   // reference numbers without the UI inset
            g_vrDrewStereo = true;
            g_hudScale = 0.70f;          // fixed reference scale for the numbers
            g_hudOffsetX = g_hudOffsetY = 0.0f;
            int hx = 0, hy = 0, hw = 1440, hh = 1440;
            HudRemap(&hx, &hy, &hw, &hh);
            CHECK(hw == 1008 && hh == 567, "hud: 3D-frame band shrinks by vr_hud_scale (0.70)");
            CHECK(hx == 216, "hud: shrunk band stays centred");
            CHECK(fabsf((float)hw / (float)hh - 1.7777f) < 0.01f,
                  "hud: shrunk band preserves the configured aspect");
            // VR UI SAFE AREA (P3, 2026-09-04): a further uniform inset,
            // per-axis bounds honoured through the smaller factor.
            {
                VR_SetUiSafeArea(0.92f, 1.0f, 1.0f);
                int ux = 0, uy = 0, uw = 1440, uh = 1440;
                HudRemap(&ux, &uy, &uw, &uh);
                CHECK(uw == 927 && uh == 522, "hud: vr_ui_scale 0.92 insets the band to 927x522");
                CHECK(ux == (1440 - 927) / 2 && uy == (1440 - 522) / 2, "hud: the inset band stays centred");
                CHECK(fabsf((float)uw / (float)uh - 1.7777f) < 0.01f, "hud: the inset band keeps the aspect");
                VR_SetUiSafeArea(1.0f, 1.0f, 0.80f);
                int vx = 0, vy = 0, vw = 1440, vh = 1440;
                HudRemap(&vx, &vy, &vw, &vh);
                CHECK(vw == 806 && vh == 454, "hud: vr_ui_safe_y 0.80 bounds both axes through the aspect");
                VR_SetUiSafeArea(1.0f, 1.0f, 1.0f);   // the offset checks below are pre-inset
            }

            // Offsets nudge the whole band, as fractions of the screen.
            g_hudOffsetX = 0.10f;
            g_hudOffsetY = -0.05f;
            int ox = 0, oy = 0, ow = 1440, oh = 1440;
            HudRemap(&ox, &oy, &ow, &oh);
            CHECK(ox == 216 + 144, "hud: horizontal offset moves the band right");
            CHECK(ow == 1008 && oh == 567, "hud: offsets never change the band size");
            int cy = 0, cx = 0, cw = 1440, ch = 1440;
            g_hudOffsetX = 0.0f; g_hudOffsetY = 0.0f;
            HudRemap(&cx, &cy, &cw, &ch);
            CHECK(oy == cy - 72, "hud: vertical offset moves the band up by 5%");

            g_vrDrewStereo = false;
            g_hudOffsetX = 0.10f;        // offsets are 3D-frame-only, like scale
            int mx = 0, my = 0, mw = 1440, mh = 1440;
            HudRemap(&mx, &my, &mw, &mh);
            CHECK(mh == 810 && my == 315, "hud: menu frames keep the full band");
            CHECK(mx == 0, "hud: menu frames ignore the HUD offsets");
            g_vrDrewStereo = savedStereo;
            g_hudScale = savedScale;
            g_hudOffsetX = savedOffX;
            g_hudOffsetY = savedOffY;
            VR_SetUiSafeArea(savedUi, savedUx, savedUy);
        }

        // Turning it off must leave coordinates completely untouched.
        g_hudAspect = 0.0f;
        int ux = 10, uy = 20, uw = 30, uh = 40;
        HudRemap(&ux, &uy, &uw, &uh);
        CHECK(ux == 10 && uy == 20 && uw == 30 && uh == 40,
              "hud: aspect 0 leaves the HUD untouched");

        // A screen already the right shape must also be left alone.
        g_hudAspect = 1.7777f;
        g_viewW = 2560; g_viewH = 1440;
        int wx = 0, wy = 0, ww = 2560, wh = 1440;
        HudRemap(&wx, &wy, &ww, &wh);
        CHECK(ww == 2560 && wh == 1440 && wx == 0 && wy == 0,
              "hud: a 16:9 screen needs no remap");

        g_viewW = savedW; g_viewH = savedH; g_hudAspect = savedAspect;
    }

    // --- comfort settings ---
    // Key names have to be settable as text; nobody should be looking up
    // virtual-key codes to rebind recenter.
    CHECK(Comfort_ParseKey("F9")   == VK_F9,   "comfort: parses a function key");
    CHECK(Comfort_ParseKey("f12")  == VK_F12,  "comfort: key names are case-insensitive");
    CHECK(Comfort_ParseKey("HOME") == VK_HOME, "comfort: parses a named key");
    CHECK(Comfort_ParseKey("Q")    == 'Q',     "comfort: parses a letter key");
    CHECK(Comfort_ParseKey("none") == 0,       "comfort: 'none' disables a binding");
    CHECK(Comfort_ParseKey("")     == 0,       "comfort: empty binding is disabled");
    CHECK(Comfort_ParseKey("nonsense") == 0,   "comfort: nonsense binding is disabled");

    // The tunnel works by shrinking the APERTURE, not by fading a fixed mask:
    // greying the periphery does not remove the peripheral motion that causes
    // sickness, whereas closing the opening does. Wide open must sit entirely
    // off screen (past the 1.414 corner) so nothing is visible at rest, and
    // fully closed should land near the 0.25/0.65 tunnel the research describes.
    {
        float rin = 0, rout = 0;
        Comfort_TestAperture(0.0f, &rin, &rout);
        CHECK(rin > 1.45f, "comfort: at rest the aperture is off screen entirely");

        Comfort_TestAperture(1.0f, &rin, &rout);
        CHECK(rin > 0.15f && rin < 0.35f, "comfort: closed aperture matches the research");
        CHECK(rout > rin, "comfort: the aperture edge is soft, not a hard ring");
        CHECK(rout < 0.80f, "comfort: closed tunnel actually restricts the view");

        // It has to close monotonically, or the tunnel would jitter with speed.
        float prevIn = 99.0f;
        bool monotonic = true;
        for (int i = 0; i <= 10; ++i) {
            float a = i / 10.0f, ri = 0, ro = 0;
            Comfort_TestAperture(a, &ri, &ro);
            if (ri > prevIn) monotonic = false;
            prevIn = ri;
        }
        CHECK(monotonic, "comfort: the aperture closes smoothly with motion");
    }

    // The vignette must settle to zero when the player is still, or it would
    // sit permanently closed and crush the field of view.
    {
        Comfort_TestSetVignette(1.0f);
        for (int i = 0; i < 400; ++i)
            Comfort_Update();          // no motion reported without a game
        CHECK(Comfort_VignetteAmount() < 0.02f,
              "comfort: vignette opens back up when motion stops");
    }

    // --- live OpenGL paths ---
    // The capture/blit path only ever ran inside the game before, so every
    // mistake in it cost a play session to find. The self-test host now brings
    // up a real GL context on a hidden window, which lets the same code that
    // runs in-game be exercised here against a real driver.
    {
        typedef HGLRC (WINAPI *GetCurCtxFn)(void);
        GetCurCtxFn getCtx = (GetCurCtxFn)p_wglGetCurrentContext;
        HGLRC ctx = getCtx ? getCtx() : nullptr;
        if (!ctx) {
            Log("  (no GL context; skipping the GL capture/blit tests)");
        } else {
            typedef unsigned (WINAPI *GetErrFn)(void);
            typedef void (WINAPI *EnableFn)(unsigned);
            typedef unsigned char (WINAPI *IsEnabledFn)(unsigned);
            typedef void (WINAPI *ScissorFn)(int,int,int,int);
            GetErrFn    glErr    = (GetErrFn)p_glGetError;
            EnableFn    glEn     = (EnableFn)p_glEnable;
            EnableFn    glDis    = (EnableFn)p_glDisable;
            IsEnabledFn glIsEn   = (IsEnabledFn)p_glIsEnabled;
            ScissorFn   glSciss  = (ScissorFn)p_glScissor;
            enum { GL_SCISSOR_TEST_T = 0x0C11 };

            if (glErr) while (glErr()) {}      // drain anything pre-existing

            // Exercise the exported texture setter against actual texture
            // objects. A forwarding-only export reproduces the game's ignored
            // anisotropy; neither a source-text check nor a mock can prove the
            // sampler property the driver will really use.
            {
                typedef void (WINAPI *ParamFn)(unsigned,unsigned,int);
                typedef void (WINAPI *FloatParamFn)(unsigned,unsigned,float);
                typedef void (WINAPI *GetTexFn)(unsigned,unsigned,float*);
                typedef void (WINAPI *GetIntFn)(unsigned,int*);
                typedef void (WINAPI *GetFloatFn)(unsigned,float*);
                typedef const unsigned char* (WINAPI *StringFn)(unsigned);
                typedef void (WINAPI *GenFn)(int,unsigned*);
                typedef void (WINAPI *BindFn)(unsigned,unsigned);
                typedef void (WINAPI *DeleteFn)(int,const unsigned*);
                typedef void (WINAPI *ImageFn)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*);
                typedef void (WINAPI *ReadTexFn)(unsigned,int,unsigned,unsigned,void*);
                typedef BOOL (WINAPI *MakeCurrentFn)(HDC,HGLRC);
                typedef HDC (WINAPI *GetDCFn)(void);
                HMODULE self = nullptr;
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                    | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    (LPCSTR)&SelfTest, &self);
                ParamFn parameter = self ? (ParamFn)GetProcAddress(self,"glTexParameteri") : nullptr;
                FloatParamFn rawFloat = (FloatParamFn)p_glTexParameterf;
                GetTexFn getTexture = (GetTexFn)p_glGetTexParameterfv;
                GetIntFn getInteger = (GetIntFn)p_glGetIntegerv;
                GetFloatFn getFloat = (GetFloatFn)p_glGetFloatv;
                StringFn getString = (StringFn)p_glGetString;
                GenFn generate = (GenFn)p_glGenTextures;
                BindFn bind = (BindFn)p_glBindTexture;
                DeleteFn erase = (DeleteFn)p_glDeleteTextures;
                ImageFn image = (ImageFn)p_glTexImage2D;
                ReadTexFn readTexture = (ReadTexFn)p_glGetTexImage;
                const char* extensions = getString ? (const char*)getString(0x1F03) : nullptr;
                const bool supported = extensions && strstr(extensions,"GL_EXT_texture_filter_anisotropic ");
                CHECK(parameter && rawFloat && getTexture && getInteger && getFloat
                    && generate && bind && erase && image && readTexture && supported,
                    "AF: real driver and exported texture setter are available");
                if (parameter && rawFloat && getTexture && getInteger && getFloat
                    && generate && bind && erase && image && readTexture && supported) {
                    float maximum=1, got=0;
                    getFloat(0x84FF,&maximum);
                    CHECK(_finite(maximum) && maximum>=2,"AF: driver reports a legitimate anisotropy maximum");
                    int savedBinding=0, savedActive=0, savedViewport[4];
                    getInteger(0x8069,&savedBinding); getInteger(0x84E0,&savedActive);
                    getInteger(0x0BA2,savedViewport);
                    unsigned texture=0; generate(1,&texture); bind(0x0DE1,texture);
                    const unsigned char pixels[16]={12,34,56,177,78,90,12,177,34,56,78,177,90,12,34,177};
                    const unsigned char mip[4]={54,48,45,177};
                    image(0x0DE1,0,0x1908,2,2,0,0x1908,0x1401,pixels);
                    image(0x0DE1,1,0x1908,1,1,0,0x1908,0x1401,mip);
                    const int mipFilters[]={0x2700,0x2701,0x2702,0x2703};
                    for (int i=0;i<4;++i) {
                        rawFloat(0x0DE1,0x84FE,1);
                        parameter(0x0DE1,0x2801,mipFilters[i]);
                        getTexture(0x0DE1,0x84FE,&got);
                        CHECK(fabsf(got-maximum)<0.001f,"AF: each mipmapped filter uses the driver's real maximum");
                        getTexture(0x0DE1,0x2801,&got);
                        CHECK((int)got==mipFilters[i],"AF: requested mip filter remains unchanged");
                    }
                    const int nonMipFilters[]={0x2600,0x2601};
                    for (int i=0;i<2;++i) {
                        rawFloat(0x0DE1,0x84FE,2);
                        parameter(0x0DE1,0x2801,nonMipFilters[i]);
                        getTexture(0x0DE1,0x84FE,&got);
                        CHECK(got==1,"AF: nearest and linear non-mip textures stay isotropic");
                    }
                    rawFloat(0x0DE1,0x84FE,2);
                    parameter(0x0DE1,0x2800,0x2601);
                    getTexture(0x0DE1,0x84FE,&got);
                    CHECK(got==2,"AF: unrelated magnification parameter cannot change anisotropy");
                    unsigned char readPixels[16]={}; readTexture(0x0DE1,0,0x1908,0x1401,readPixels);
                    CHECK(!memcmp(pixels,readPixels,sizeof(pixels)),"AF: texture pixels and alpha remain byte-identical");
                    int binding=0, active=0, viewport[4];
                    getInteger(0x8069,&binding); getInteger(0x84E0,&active); getInteger(0x0BA2,viewport);
                    CHECK(binding==(int)texture && active==savedActive
                        && !memcmp(viewport,savedViewport,sizeof(viewport)),"AF: texture binding, active unit and viewport remain unchanged");
                    bind(0x0DE1,0); rawFloat(0x0DE1,0x84FE,2);
                    parameter(0x0DE1,0x2801,0x2703); getTexture(0x0DE1,0x84FE,&got);
                    CHECK(got==2,"AF: default texture zero is not assigned forced anisotropy");
                    rawFloat(0x0DE1,0x84FE,1); bind(0x0DE1,texture);
                    rawFloat(0x0DE1,0x84FE,1);
                    FARPROC savedString=p_glGetString;
                    p_glGetString=(FARPROC)&TestNoAnisotropyExtensions;
                    parameter(0x0DE1,0x2801,0x2703);
                    p_glGetString=savedString;
                    getTexture(0x0DE1,0x84FE,&got);
                    CHECK(got==1,"AF: unsupported extension or suffix lookalike leaves sampler unchanged");
                    GetDCFn getDC=(GetDCFn)p_wglGetCurrentDC;
                    MakeCurrentFn current=(MakeCurrentFn)p_wglMakeCurrent;
                    if (getDC && current) {
                        HDC dc=getDC();
                        CHECK(current(nullptr,nullptr)!=0,"AF: no-context regression fixture detaches safely");
                        parameter(0x0DE1,0x2801,0x2703);
                        CHECK(current(dc,ctx)!=0,"AF: a setter without a context does not fault or strand the context");
                        getTexture(0x0DE1,0x84FE,&got);
                        CHECK(got==1,"AF: no-context call cannot change the texture object");
                    }
                    bind(0x0DE1,(unsigned)savedBinding); erase(1,&texture);
                    CHECK(!glErr || glErr()==0,"AF: texture and unsupported-context regressions raise no GL errors");
                }
            }

            // Reproduce the in-game state exactly: the engine leaves the
            // scissor test ENABLED with a box smaller than the eye image, which
            // silently clipped every blit and cut the top off the view.
            bool scissorTested = false, scissorRestored = false;
            if (glEn && glSciss && glIsEn) {
                glSciss(0, 0, 64, 64);
                glEn(GL_SCISSOR_TEST_T);
                scissorTested = true;
            }

            const bool captured = VR_CaptureEye(0, 256, 256, false);
            CHECK(captured, "GL: capture into an eye texture succeeds");

            unsigned err = glErr ? glErr() : 0;
            CHECK(err == 0, "GL: capture raises no GL error");
            if (err)
                Log("        glGetError -> 0x%X", err);

            if (scissorTested) {
                scissorRestored = glIsEn(GL_SCISSOR_TEST_T) != 0;
                CHECK(scissorRestored,
                      "GL: capture restores the engine's scissor state");
                glDis(GL_SCISSOR_TEST_T);
            }

            // A second capture must reuse the texture rather than leak one.
            CHECK(VR_CaptureEye(1, 256, 256, false), "GL: second eye captures too");
            CHECK(!glErr || glErr() == 0, "GL: second capture is clean");

            // The letterboxed path (2D screens) must also be clean.
            CHECK(VR_CaptureEye(0, 320, 200, true), "GL: letterboxed capture works");
            CHECK(!glErr || glErr() == 0, "GL: letterboxed capture is clean");

            // COVERAGE. A capture that fills only a corner of the eye texture
            // still returns success, and that is exactly how a hugely zoomed
            // view once shipped. Paint the source a known colour, capture it
            // full-frame, then sample the FAR corner: if the copy only covered
            // part of the texture, that corner is still black.
            typedef void (WINAPI *ClearColorFn)(float,float,float,float);
            typedef void (WINAPI *ClearFn)(unsigned);
            ClearColorFn glClearCol = (ClearColorFn)p_glClearColor;
            ClearFn      glClearNow = (ClearFn)p_glClear;
            if (glClearCol && glClearNow) {
                glClearCol(1.0f, 0.0f, 0.0f, 1.0f);         // red back buffer
                glClearNow(0x4000 /*GL_COLOR_BUFFER_BIT*/);
                VR_CaptureEye(0, 256, 256, false);

                int tw = 0, th = 0;
                CHECK(VR_TestEyeTextureSize(&tw, &th) && tw > 0 && th > 0,
                      "GL: eye texture has a size");
                const int nearPx = VR_TestSampleEyeTexture(0, 2, 2);
                const int farPx  = VR_TestSampleEyeTexture(0, tw - 3, th - 3);
                CHECK(nearPx > 128, "GL: capture fills the near corner of the eye");
                CHECK(farPx > 128,  "GL: capture fills the FAR corner of the eye");
                if (farPx <= 128)
                    Log("        far corner (%d,%d) read %d -- the copy covered "
                        "only part of the texture", tw - 3, th - 3, farPx);
            }

            // The supersampled offscreen target: creation, completeness, and
            // that rendering can be pointed at it and handed back.
            const bool bound = VR_BindSceneTarget(256, 144);
            CHECK(bound, "GL: supersampled scene target binds");
            if (bound) {
                CHECK(VR_Supersample() > 1.0f,
                      "GL: supersample factor is live while bound");
                CHECK(!glErr || glErr() == 0, "GL: scene target creation is clean");
                // Capturing from the offscreen target must work too.
                CHECK(VR_CaptureEye(1, 256, 144, false),
                      "GL: capture reads back from the scene target");
                CHECK(!glErr || glErr() == 0, "GL: capture from scene target is clean");
                // Viewport scaling: the engine's own screen coordinates must
                // land scaled inside the larger target, or it would draw into
                // a corner of it. Verified by reading the state back.
                typedef void (WINAPI *GetIntFn)(unsigned, int*);
                GetIntFn getInt = (GetIntFn)p_glGetIntegerv;
                if (getInt) {
                    // Derived from the live factors, not hardcoded: the value
                    // comes from codvr.cfg and changes with the tuning. The
                    // two axes differ now -- the scene target is shaped to the
                    // runtime eye aspect, not to the engine's square refdef.
                    float sx = 1.0f, sy = 1.0f;
                    VR_SupersampleXY(&sx, &sy);
                    const int wantW = (int)(100 * sx + 0.5f);
                    const int wantH = (int)(50  * sy + 0.5f);
                    int vp[4] = {0,0,0,0};
                    i_glViewport(0, 0, 100, 50);
                    getInt(0x0BA2 /*GL_VIEWPORT*/, vp);
                    CHECK(vp[2] == wantW && vp[3] == wantH,
                          "GL: viewport is scaled per-axis into the scene target");
                    if (vp[2] != wantW || vp[3] != wantH)
                        Log("        got %d,%d %dx%d, wanted %dx%d",
                            vp[0], vp[1], vp[2], vp[3], wantW, wantH);
                }

                // GL_BACK is illegal on a framebuffer object; it has to become
                // the colour attachment or every draw this frame is dropped.
                CHECK(TranslateBuffer(GL_BACK_B) == GL_COLOR_ATTACHMENT0_B,
                      "GL: GL_BACK is translated for the offscreen target");

                VR_UnbindSceneTarget();
                CHECK(VR_Supersample() == 1.0f,
                      "GL: unbinding restores unscaled coordinates");
                CHECK(TranslateBuffer(GL_BACK_B) == GL_BACK_B,
                      "GL: GL_BACK is left alone once back on the window");

                if (getInt) {
                    int vp[4] = {0,0,0,0};
                    i_glViewport(0, 0, 100, 50);
                    getInt(0x0BA2 /*GL_VIEWPORT*/, vp);
                    CHECK(vp[2] == 100 && vp[3] == 50,
                          "GL: viewport is unscaled once back on the window");
                }
            }

            // The vignette draws with fixed-function GL into the eye texture
            // and must leave the engine's state exactly as it found it -- the
            // engine keeps drawing with this context the moment we hand it back.
            {
                enum { GL_BLEND_T = 0x0BE2, GL_DEPTH_TEST_T = 0x0B71 };

                // Give the eye texture real content first. Once the scene
                // target exists, capture reads from IT rather than the window,
                // so clearing the window would leave the eye black and the
                // comparison below meaningless.
                //
                // The scissor must be OFF for this: glClear is scissored just
                // like a blit is, and leaving a small box set would fill only a
                // corner -- which is exactly how this test first misled me.
                if (glDis) glDis(GL_SCISSOR_TEST_T);
                if (glClearCol && glClearNow) {
                    VR_BindSceneTarget(256, 144);
                    glClearCol(1.0f, 1.0f, 1.0f, 1.0f);
                    glClearNow(0x4000 /*GL_COLOR_BUFFER_BIT*/);
                    VR_UnbindSceneTarget();
                }
                VR_CaptureEye(0, 256, 256, false);

                // Now dirty the state the vignette has to put back.
                if (glEn && glSciss) { glSciss(0, 0, 64, 64); glEn(GL_SCISSOR_TEST_T); }
                if (glEn) glEn(GL_DEPTH_TEST_T);
                if (glDis) glDis(GL_BLEND_T);
                Comfort_TestSetVignette(0.75f);
                const bool drew = Comfort_DrawVignette(0);
                CHECK(drew, "comfort: vignette draws into the eye texture");
                CHECK(!glErr || glErr() == 0, "comfort: vignette raises no GL error");

                if (glIsEn) {
                    CHECK(glIsEn(GL_SCISSOR_TEST_T) != 0,
                          "comfort: vignette restores the scissor test");
                    CHECK(glIsEn(GL_DEPTH_TEST_T) != 0,
                          "comfort: vignette restores the depth test");
                    CHECK(glIsEn(GL_BLEND_T) == 0,
                          "comfort: vignette restores blending to off");
                }

                // It must actually darken the edge while leaving the centre
                // alone -- that is the whole point of a tunnel.
                int tw = 0, th = 0;
                VR_TestEyeTextureSize(&tw, &th);
                const int centre = VR_TestSampleEyeTexture(0, tw/2, th/2);
                const int corner = VR_TestSampleEyeTexture(0, 2, 2);
                CHECK(corner < centre,
                      "comfort: vignette darkens the periphery, not the centre");
                if (corner >= centre)
                    Log("        centre %d, corner %d", centre, corner);

                Comfort_TestSetVignette(0.0f);
                if (glDis) { glDis(GL_SCISSOR_TEST_T); glDis(GL_DEPTH_TEST_T); }
            }

            // Prove the actual cause of the missing top of the view: with the
            // engine's scissor box left enabled, a blit is clipped to it and
            // everything outside stays unwritten. This is the whole reason the
            // eye image was only filled up to the game's height.
            const int clipped   = VR_TestBlitScissorBehaviour(false);
            const int unclipped = VR_TestBlitScissorBehaviour(true);
            if (clipped < 0 || unclipped < 0) {
                Log("  (scissor/blit interaction not testable here)");
            } else {
                CHECK(clipped == 0,
                      "GL: an enabled scissor DOES clip a blit (the root cause)");
                CHECK(unclipped == 1,
                      "GL: disabling the scissor lets the blit cover the target");
            }

            // --- projection interception through the real GL stack ---
            // These are the exact wrapper functions the engine's import table
            // binds to, driven against a live driver: an armed perspective
            // load must land replaced in actual GL state, and everything
            // else -- ortho, modelview, disarmed -- must pass through.
            {
                typedef void (WINAPI *GetFloatFn)(unsigned, float*);
                typedef void (WINAPI *LoadIdFn)(void);
                GetFloatFn glGetF = (GetFloatFn)p_glGetFloatv;
                LoadIdFn   glId   = (LoadIdFn)p_glLoadIdentity;
                if (glGetF && glId) {
                    const float d2r = 0.01745329252f;
                    const float t96 = tanf(48.0f * d2r);
                    float persp[16] = { 0 };
                    persp[0]  = 1.0f / t96;
                    persp[5]  = 1.0f / t96;
                    persp[10] = -(3000.0f + 4.0f) / (3000.0f - 4.0f);
                    persp[11] = -1.0f;
                    persp[14] = -2.0f * 3000.0f * 4.0f / (3000.0f - 4.0f);

                    VR_TestArmProjection(0, tanf(-40.0f * d2r), tanf(54.0f * d2r),
                                         tanf(44.0f * d2r), tanf(-55.0f * d2r));
                    const float tl = tanf(-40.0f * d2r), tr = tanf(54.0f * d2r);
                    const float wantM8 = (tr + tl) / (tr - tl);

                    // Replacement now also requires the SCENE phase (a
                    // perspective load during the 2D overlay keeps the
                    // engine's matrix), so simulate the render hook's
                    // announcement the way production makes it.
                    CODVR_SceneQueued();

                    float got[16];
                    i_glMatrixMode(0x1701 /*GL_PROJECTION*/);
                    i_glLoadMatrixf(persp);
                    glGetF(0x0BA7 /*GL_PROJECTION_MATRIX*/, got);
                    CHECK(fabsf(got[8] - wantM8) < 1e-4f
                          && fabsf(got[10] - persp[10]) < 1e-5f,
                          "proj: armed perspective load lands REPLACED in real GL");

                    float ortho[16] = { 0 };
                    ortho[0] = 2.0f / 640.0f; ortho[5] = -2.0f / 480.0f;
                    ortho[10] = -1.0f; ortho[12] = -1.0f; ortho[13] = 1.0f;
                    ortho[15] = 1.0f;
                    i_glLoadMatrixf(ortho);
                    glGetF(0x0BA7, got);
                    CHECK(fabsf(got[15] - 1.0f) < 1e-5f && fabsf(got[12] + 1.0f) < 1e-5f,
                          "proj: ortho (menu/HUD) load passes through while armed");

                    i_glMatrixMode(0x1700 /*GL_MODELVIEW*/);
                    i_glLoadMatrixf(persp);
                    glGetF(0x0BA6 /*GL_MODELVIEW_MATRIX*/, got);
                    CHECK(fabsf(got[8]) < 1e-5f,
                          "proj: a perspective-shaped MODELVIEW load is never touched");

                    i_glMatrixMode(0x1701);
                    glId();
                    CODVR_SceneQueued();      // the ortho above ended the scene
                    i_glFrustum(-1.0, 1.0, -1.0, 1.0, 4.0, 3000.0);
                    glGetF(0x0BA7, got);
                    CHECK(fabsf(got[8] - wantM8) < 1e-4f,
                          "proj: the glFrustum path is replaced the same way");

                    VR_TestDisarmProjection();
                    CODVR_SceneQueued();      // scene phase, but DISARMED
                    i_glLoadMatrixf(persp);
                    glGetF(0x0BA7, got);
                    CHECK(fabsf(got[8]) < 1e-5f && fabsf(got[0] - persp[0]) < 1e-5f,
                          "proj: a disarmed perspective load passes through");

                    glId();
                    i_glMatrixMode(0x1700);
                    glId();

                    // --- GL pass classification --------------------------
                    // THE regression for the 2026-08-19 hardware failure:
                    // the engine executes the scene's glViewport AFTER the
                    // render syscall returns, and the old syscall-time flag
                    // classified all of it as HUD -- the world rendered into
                    // the shifted 16:9 band (viewport 187,454 1440x810 in
                    // the headset log), black-bordered and pushed ~16 deg
                    // sideways per eye in OPPOSITE directions.
                    {
                        typedef void (WINAPI *GetIntFn)(unsigned, int*);
                        GetIntFn getInt = (GetIntFn)p_glGetIntegerv;
                        const LONG savedW = g_viewW, savedH = g_viewH;
                        const float savedAspect = g_hudAspect;
                        g_viewW = 1440; g_viewH = 1440;
                        g_hudAspect = 1.7777f;
                        CODVR_GLFrameReset();          // menu-shaped start

                        int vp[4] = {0,0,0,0}, sc[4] = {0,0,0,0};
                        float ortho2[16] = { 0 };
                        ortho2[0] = 2.0f / 640.0f; ortho2[5] = -2.0f / 480.0f;
                        ortho2[10] = -1.0f; ortho2[12] = -1.0f;
                        ortho2[13] = 1.0f; ortho2[15] = 1.0f;

                        i_glMatrixMode(0x1701);
                        i_glLoadMatrixf(ortho2);
                        i_glViewport(0, 0, 1440, 1440);
                        getInt(0x0BA2, vp);
                        CHECK(vp[0] == 0 && vp[1] == 315 && vp[2] == 1440 && vp[3] == 810,
                              "phase: menu viewport lands in the 16:9 band");

                        // A 3D frame is queued: from that moment -- before
                        // any projection load -- scene rects cover the target.
                        VR_TestArmProjection(0, tanf(-54.0f * d2r), tanf(40.0f * d2r),
                                             tanf(44.0f * d2r), tanf(-55.0f * d2r));
                        CODVR_SceneQueued();
                        i_glViewport(0, 0, 1440, 1440);
                        i_glScissor(0, 0, 1440, 1440);
                        getInt(0x0BA2, vp);
                        getInt(0x0C10, sc);
                        CHECK(vp[0] == 0 && vp[1] == 0 && vp[2] == 1440 && vp[3] == 1440,
                              "phase: scene viewport covers the target after the hook");
                        CHECK(sc[0] == 0 && sc[1] == 0 && sc[2] == 1440 && sc[3] == 1440,
                              "phase: scene scissor covers the target too");

                        i_glLoadMatrixf(persp);
                        getInt(0x0BA2, vp);
                        glGetF(0x0BA7, got);
                        CHECK(fabsf(got[8]) > 0.1f,
                              "phase: the scene load carries the eye's asymmetric m8");
                        CHECK(vp[0] == 0 && vp[1] == 0 && vp[2] == 1440 && vp[3] == 1440,
                              "phase: REGRESSION -- world viewport stays FULL after the "
                              "projection load (was 187,454 1440x810 on hardware)");

                        // Flare-style excursion: push/ortho/pop mid-scene.
                        i_glPushMatrix();
                        i_glLoadIdentity();
                        i_glOrtho(0, 640, 480, 0, 0, 1);
                        i_glPopMatrix();
                        i_glViewport(0, 0, 1440, 1440);
                        getInt(0x0BA2, vp);
                        CHECK(vp[1] == 0 && vp[3] == 1440,
                              "phase: a pushed ortho (flares) does not end the scene");

                        // The overlay begins: ortho at depth 0. The stored
                        // scene rect is re-issued into the band WITHOUT a new
                        // engine viewport call...
                        i_glLoadMatrixf(ortho2);
                        getInt(0x0BA2, vp);
                        CHECK(vp[0] == 0 && vp[1] == 315 && vp[2] == 1440 && vp[3] == 810,
                              "phase: the overlay's ortho re-issues the band viewport");

                        // ...and a LATER perspective load is a HUD element's
                        // own projection: never replaced, never scene-mapped.
                        const unsigned replBefore = VR_TestProjectionReplacedCount(0);
                        i_glLoadMatrixf(persp);
                        glGetF(0x0BA7, got);
                        CHECK(VR_TestProjectionReplacedCount(0) == replBefore
                              && fabsf(got[0] - persp[0]) < 1e-5f && fabsf(got[8]) < 1e-5f,
                              "phase: a HUD-phase perspective load keeps the engine's matrix");
                        i_glViewport(0, 0, 1440, 1440);
                        getInt(0x0BA2, vp);
                        CHECK(vp[1] == 315 && vp[3] == 810,
                              "phase: after the overlay begins the scene never resumes");

                        VR_TestDisarmProjection();
                        CODVR_GLFrameReset();
                        i_glMatrixMode(0x1700);
                        g_viewW = savedW; g_viewH = savedH; g_hudAspect = savedAspect;
                    }
                } else {
                    Log("  (no glGetFloatv; skipping projection interception tests)");
                }
            }
        }
    }

    g_realSyscall = nullptr;
    Probe_TestSetPhase(PROBE_PH_BASE0);
    g_fakeCalls = 0;
    Probe_SetEnabled(savedProbe);

    Log("=== SELF TEST: %d passed, %d failed ===", pass, fail);
    #undef CHECK
}

// ------------------------------------------------------------- entry point

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        LogInit();

        char exe[MAX_PATH] = {0};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        Log("=== COD VR camera probe ===");
        Log("host: %s  pid %lu", exe, GetCurrentProcessId());
        // The flat automation depends on `+spmap` reaching the engine. If
        // Steam strips the arguments the run sits in the main menu forever
        // and every capture comes back empty -- so record what the process
        // was actually started with rather than assuming.
        Log("cmdline: %s", GetCommandLineA() ? GetCommandLineA() : "(null)");
        // Milestone: identity at ATTACH -- before any GL, any config parse,
        // any scene. Every later milestone can be missing for its own
        // reasons; this one existing with the wrong hash is the ONLY thing
        // that means "wrong binary". (2026-08-27: a healthy attended run was
        // killed on exactly that misdiagnosis, made from a scene-dependent
        // banner.) Self-contained SHA-256 -- safe under the loader lock.
        {
            char self[MAX_PATH] = { 0 };
            char sha[65] = { 0 };
            if (!PathBesideModule("opengl32.dll", self, sizeof(self))
                || !Sha256File(self, sha))
                strcpy_s(sha, "UNAVAILABLE");
            Log("=== CODVR BUILD ID ===");
            Log("build id/hash: %s", sha);
            Log("module path: %s", self[0] ? self : "(unknown)");
            Log("probe arm requested: (config not parsed yet)");
            Log("======================");
        }

        char sys[MAX_PATH];
        if (!GetSystemDirectoryA(sys, MAX_PATH))
            return FALSE;
        strcat_s(sys, "\\opengl32.dll");
        g_realGL = LoadLibraryA(sys);
        if (!g_realGL) {
            Log("FATAL: real GL load failed (%lu)", GetLastError());
            return FALSE;
        }
        int ok = 0;
        for (int i = 0; i < GL_PROXY_EXPORT_COUNT; ++i) {
            *kExportSlots[i] = GetProcAddress(g_realGL, kExportNames[i]);
            if (*kExportSlots[i]) ++ok;
        }
        Log("resolved %d/%d exports from %s", ok, GL_PROXY_EXPORT_COUNT, sys);
        if (ok != GL_PROXY_EXPORT_COUNT)
            Log("WARNING: some exports unresolved; forwarding may fault");

        LoadConfig();
        SetupReplaySet();
        SetupWeaponDivertSet();
        // Ground-truth ADS reads come from cgame's zoom-transition float; the
        // provider is wired here (after config, which may disable the RVA).
        VRBinds_SetAdsHintProvider(CODVR_GetCgameAdsHint);

        char flag[8];
        if (GetEnvironmentVariableA("CODVR_SELFTEST", flag, sizeof(flag)) > 0)
            SelfTest();
    }
    else if (reason == DLL_PROCESS_DETACH) {
        RestoreTwoHandAdsSpread();
        if (g_cgameHooked)
            Probe_ConcludeIfRunning();
        if (g_syscallSlot && g_realSyscall) {
            DWORD old;
            if (VirtualProtect(g_syscallSlot, sizeof(*g_syscallSlot), PAGE_READWRITE, &old)) {
                *g_syscallSlot = g_realSyscall;
                VirtualProtect(g_syscallSlot, sizeof(*g_syscallSlot), old, &old);
            }
        }

        // Close the OpenXR session so the runtime is not left waiting on an
        // xrEndFrame that never comes.
        //
        // ONLY when the DLL is being unloaded on its own (reserved == NULL).
        // On process termination Windows may already have torn down the loader
        // and other threads, and calling back into the delay-loaded OpenXR
        // loader there can fault or deadlock -- a worse failure than the stall
        // this avoids. In that case the kernel reclaims everything anyway.
        // Injected key/button state outlives the process at the OS level (a
        // synthetic MOUSE1 down with no up keeps the system button down until
        // the physical mouse clicks), so release-all runs in BOTH branches --
        // it only touches user32, never the delay-loaded XR loader.
        VRBinds_ReleaseAllInjectedInputs("dll detach");
        if (reserved == nullptr) {
            VR_Shutdown();
            Log("VR: session shut down cleanly");
        } else {
            Log("VR: process terminating; leaving the runtime to the OS");
        }
        Log("=== detached after %ld frames ===", g_frame);
        if (g_log) fclose(g_log);
    }
    return TRUE;
}






