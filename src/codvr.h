// Internal interfaces shared between the mod's translation units.
//
// dllmain.cpp was doing five jobs at once -- proxy plumbing, the cgame syscall
// hook, GL interception, the diagnostic probe, and VR orchestration -- which
// made the parts that matter hard to find and easy to break. The probe and the
// self test are the two largest pieces that nothing else depends on, so they
// live in their own files now and reach back through this header.
//
// Public VR/OpenXR interfaces live in vr.h; this is strictly the mod's own
// internals.
#pragma once

#include <windows.h>

// ----------------------------------------------------------------- logging

void Log(const char* fmt, ...);

// Resolves a GL entry point from the real driver. dllmain owns that handle, so
// everything else asks through here rather than keeping its own copy.
extern "C" void* CODVR_GetGLProc(const char* name);

// Reads cgame's ADS/zoom transition float (RVA 0x11E15C: 0.0 at hip-fire,
// nonzero while the ADS zoom is engaged or transitioning). Returns 1 with the
// value, or 0 when unreadable/implausible. SEH-guarded; safe on any build.
extern "C" int CODVR_GetCgameAdsHint(float* value);

// Monotonic high-resolution seconds (QueryPerformanceCounter). GetTickCount's
// 15.6 ms granularity quantised every per-frame dt at 5-14 ms frame times,
// which made the motion estimates (and anything smoothed by dt) noisy.
double CODVR_NowSeconds();

// Testing seam: freeze CODVR_NowSeconds at an explicit value (advance it by
// calling again), so cadence and pair-separation arithmetic can be verified
// with exact virtual time instead of racing the wall clock. Negative restores
// the real clock.
void CODVR_TestSetNow(double seconds);

// ------------------------------------------- collision-hook camera invariant
//
// The number of times a collision/content syscall hook has mutated camera
// bytes. It must remain EXACTLY ZERO for the life of the process.
//
// Syscalls 37 and 38 are CM_PointContents and CM_TransformedPointContents,
// and the pointer they carry is &cg.refdef.vieworg -- not a view-weapon
// entity. Treating them as entity adds rewrote the CAMERA, which is what
// made the 2026-08-23 hardware run physically intolerable. See
// RETIRED_INVALID_37_38_WEAPON_PATH in dllmain.cpp.
//
// Nothing in the shipped build calls the Note function; it exists so that any
// future reintroduction of a write-through collision hook is caught by the
// stereo fail-safe and the self-test rather than by the player's inner ear.
unsigned CODVR_CollisionHookCameraMutationCount();
void     CODVR_NoteCollisionHookCameraMutation();

// Testing seam: clear the invariant counter between self-test scenarios.
void     CODVR_TestResetCollisionHookCameraMutation();

// ---------------------------------------------------------- refdef layout

// Byte-identical to Quake 3's refdef_t, confirmed against the live module:
// ints x,y,w,h at +0; fov_x/fov_y at +16/+20; vieworg at +24; a 3x3 orthonormal
// viewaxis at +36.
enum {
    RD_X = 0, RD_Y = 1, RD_W = 2, RD_H = 3,       // int indices
    RD_FOVX = 4, RD_FOVY = 5,                      // float indices
    RD_ORG = 6,                                    // vieworg[3] -> floats 6,7,8
    RD_AXIS_BYTES = 36,                            // viewaxis byte offset
    RD_MIN_BYTES = 72
};

enum { SNAP_BYTES = 256, MAX_SYSCALL = 512 };

bool Readable(const void* p, size_t n);
int  FindOrthonormal(const BYTE* buf, size_t bytes);
bool IsRefdef(const void* p);

// ------------------------------------------------------ cgame syscall hook

typedef int (__cdecl *SyscallFn)(int, ...);

extern SyscallFn g_realSyscall;      // engine entry point we forward to
extern int       g_renderSyscall;    // which trap is R_RenderScene (71)
extern LONG      g_viewW, g_viewH;   // the engine's own render size

// -------------------------------------------- projection-stack interception
//
// The engine renders 3D through a symmetric projection built from refdef's
// symmetric fov; a headset eye is an ASYMMETRIC frustum. The refdef fov is
// therefore culling-only, and the real optics are installed here: dllmain's
// opengl32 wrappers call this for every matrix the ENGINE loads onto the
// GL_PROJECTION stack, and vr.cpp replaces it exactly when (a) the frame
// hook armed an eye -- a 3D scene render is in flight between the render
// syscall and the buffer swap -- and (b) the matrix is structurally a
// perspective projection (VRM_IsPerspective). Returns true and rewrites m in
// place when replaced. Menu/HUD orthographic loads fail (b); menu frames
// never arm (a).
bool CODVR_MaybeReplaceProjection(float m[16]);

// The viewport/scissor the scene is really being rendered with, recorded by
// the GL wrapper at the FIRST projection replacement of each eye's render
// window (that is the state the world pass draws under). Per eye, because
// the 2026-08-19 hardware run showed a single shared slot pinned the optics
// verdict to whatever pass happened to load a matrix last.
// viewport/scissor are x,y,w,h.
void CODVR_NoteProjectionGLState(int eye, const int viewport[4],
                                 const int scissor[4], int scissorEnabled);

// --------------------------------------------------------------- the probe
//
// A self-scoring diagnostic that runs INSTEAD of VR (codvr.cfg "probe 1"). It
// needs no headset -- the game runs flat on the monitor and writes a verdict to
// the log -- which makes it the cheap way to answer questions that would
// otherwise cost a play session.

bool Probe_Enabled();
void Probe_SetEnabled(bool on);

// Called from the render hook when the engine renders a scene. Returns true if
// the probe handled the call itself, with the engine's return value in *ret.
bool Probe_RenderHook(int num, void* refdef,
                      int a2, int a3, int a4, int a5, int a6, int a7, int* ret);

// Called once per presented frame. Returns true if the probe consumed the
// frame and the caller should present immediately without doing VR work.
bool Probe_FrameHook();

// Writes partial findings when the game exits mid-experiment.
void Probe_ConcludeIfRunning();

// Testing seam: lets the self test drive the phase machine directly, so the
// halve/double behaviour can be verified without a game or a headset.
enum { PROBE_PH_BASE0 = 0, PROBE_PH_HALF_A, PROBE_PH_HALF_B,
       PROBE_PH_DBL_A, PROBE_PH_DBL_B, PROBE_PH_DONE };
void Probe_TestSetPhase(int phase);

// ------------------------------------------------------------- comfort
//
// The options that decide whether this is pleasant for an hour or unbearable
// after ten minutes: a tunnelling vignette driven by the player's virtual
// motion, a recenter key, and optional snap turning.

void  Comfort_Update();                  // advance the smoothed vignette
bool  Comfort_DrawVignette(int eye);     // overlay it on a finished eye image
float Comfort_VignetteAmount();          // 0..1, for logging and tests
void  Comfort_PollInput();               // recenter / snap-turn keys
void  Comfort_NoteFrame();               // frame-rate watch (alternating eyes)
float Comfort_FrameRate();

void Comfort_SetVignette(bool on);
void  Comfort_SetBrightness(float b);    // 0.1..1 multiplicative gain on the eye image
float Comfort_Brightness();
void Comfort_SetVignetteStrength(float s);
void Comfort_SetSpeedThreshold(float unitsPerSec);
void Comfort_SetTurnThreshold(float degPerSec);
void Comfort_SetRecenterKey(int vk);
void Comfort_SetSnapTurn(int deg);
void Comfort_SetSnapKeys(int leftVk, int rightVk);
void Comfort_SetSnapMouseScale(float countsPerDegree);

int  Comfort_ParseKey(const char* name); // "F9", "HOME", "Q" -> virtual key
void Comfort_TestSetVignette(float v);   // testing seam
void Comfort_TestAperture(float amount, float* inner, float* outer);

// Testing seam: one smoothing step of the vignette amount over an explicit
// dt, so frame-rate independence can be proven at simulated 72/90/120 Hz.
float Comfort_TestStep(float current, float target, float dtSeconds);

// Testing seam: the motion -> target-vignette mapping for explicit inputs.
// Semantics: nothing below the configured onset threshold, fully closed at
// four times it, linear in between.
float Comfort_TestTarget(float speedUnitsPerSec, float turnDegPerSec);

// --------------------------------------------------------------- self test

// Offline exercise of everything that is expensive to debug inside the game.
// Runs at DLL_PROCESS_ATTACH when CODVR_SELFTEST is set; the harness brings up
// a real GL context first so the capture/blit paths are covered too.
void SelfTest();
