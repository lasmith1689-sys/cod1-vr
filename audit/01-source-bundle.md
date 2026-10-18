# Source bundle — CoD1 VR mod
Generated for external audit. `src/gl_thunks.inc` and `src/opengl32.def`
are machine-generated (368 export thunks) and omitted.


---

## `src/codvr.h`  (110 lines)

Shared internal interfaces between translation units.

```cpp
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

// --------------------------------------------------------------- self test

// Offline exercise of everything that is expensive to debug inside the game.
// Runs at DLL_PROCESS_ATTACH when CODVR_SELFTEST is set; the harness brings up
// a real GL context first so the capture/blit paths are covered too.
void SelfTest();

```


---

## `src/vr.h`  (131 lines)

Public interface of the OpenXR / present layer.

```cpp
﻿// OpenXR side of the mod, kept behind a narrow interface so the hook code in
// dllmain.cpp never touches OpenXR types directly.
//
// Coordinate systems differ and the conversion is the easiest thing to get
// wrong, so it lives in one place (vr.cpp, XrToQuake):
//   OpenXR : right-handed, +Y up, forward is -Z, metres
//   Quake  : +Z up, viewaxis rows are forward/left/up, ~39.37 units per metre
#pragma once

#include <windows.h>

// Called once a GL context exists. Safe to call repeatedly; it retries until
// the headset actually shows up, so launching flat then putting the headset on
// still works.
bool VR_Startup(HDC hdc, HGLRC glrc);

// True once a session is running and frames should be submitted.
bool VR_Active();

// Begin an XR frame and locate both eyes. Returns false when the runtime says
// to skip rendering this frame (throttled, or session not visible).
bool VR_BeginFrame();

// Per-eye camera, already converted into Quake's convention and composed on
// top of the game's own view. Pass the game's refdef values in; get the eye's
// values out. fovX/fovY are degrees, matching refdef's fov_x/fov_y.
// viewW/viewH are the per-eye viewport the engine will render into. The
// vertical FOV is derived from them rather than asserted, because cgame
// computes fov_y from fov_x and the viewport aspect and the engine may do the
// same -- asserting a value it then recomputes puts the compositor and the
// renderer into disagreement, which crops the top and bottom of the view.
void VR_GetEyeCamera(int eye,
                     const float gameOrigin[3], const float gameAxis[9],
                     int viewW, int viewH,
                     float outOrigin[3], float outAxis[9],
                     float* fovXdeg, float* fovYdeg);

// Present the frame. fbWidth/fbHeight describe the back buffer.
//
// Frames ALTERNATE between eyes: each frame the engine renders one eye across
// the FULL back buffer, which is then captured into that eye's texture. Both
// eyes are submitted every frame, one from this frame and one from the last.
// This sidesteps the engine consuming its entity list on the first render (the
// reason the second eye had no soldiers or weapon), doubles the horizontal
// resolution per eye, and lets each eye keep a complete 2D/HUD pass instead of
// half of one.
//
// renderedEye : which eye the back buffer currently holds.
// stereo=false: nothing 3D was drawn (menu, loading screen). The frame goes to
//               BOTH eyes, aspect-preserved, so 2D screens stay readable.
void VR_SubmitFrame(int fbWidth, int fbHeight, bool stereo, int renderedEye);

// Copy the finished frame into one eye's texture, at the headset's own per-eye
// resolution. Called by the frame hook once the 2D pass has been drawn over the
// scene. baseW/baseH describe the game's own buffer, used only when the
// supersampled target is unavailable. letterbox fits a flat 2D screen at its
// own aspect instead of stretching it across the eye.
bool VR_CaptureEye(int eye, int baseW, int baseH, bool letterbox);

// ------------------------------------------------- supersampled scene target
//
// The game draws at its own resolution (2560x1440), but each eye needs about
// 3072x3264 -- so the vertical was being stretched ~2.3x and detail that was
// never rendered could not be recovered. These redirect the engine's rendering
// into an offscreen target at a multiple of its resolution; glViewport and
// glScissor are scaled to match, so the engine is unaware and the 2D pass
// sharpens along with the scene.
bool  VR_BindSceneTarget(int baseW, int baseH);
void  VR_UnbindSceneTarget();
bool  VR_SceneTargetActive();
float VR_Supersample();          // 1.0 when the target is not in use
void  VR_SetSupersample(float s);

// The aspect the 2D layer occupies inside the render buffer. Flat screens are
// read back from just that band, so a menu already drawn in correct proportions
// is not letterboxed a second time on its way into the eye. 0 = fills the buffer.
void  VR_SetPanelAspect(float aspect);

// Copy the offscreen frame onto the game window, so the monitor still shows
// what is happening rather than a black window.
void  VR_MirrorToWindow(int winW, int winH);

void VR_Shutdown();

// Units per metre. Quake-family engines are built in inches (~39.37).
void VR_SetWorldScale(float unitsPerMetre);

// Re-zero the headset: the player's current position and facing become neutral.
// Seated tracking drifts and people start sessions facing the wrong way, so
// this is bound to a key.
void VR_Recenter();

// How far the head is turned away from the game's aim direction, in degrees
// (positive yaw left, positive pitch up). Drives head-aim steering and the
// comfort vignette.
void VR_GetHeadAimOffset(float* yawDeg, float* pitchDeg);

// Virtual motion the player cannot physically feel -- engine units per second
// of travel and degrees per second of turning. This, not head movement, is what
// causes simulator sickness, so the comfort vignette keys off it.
void VR_GetPlayerMotion(float* speed, float* turnRateDeg);

// The per-eye texture and its size, so the comfort layer can draw an overlay
// into the finished frame before it reaches the compositor.
unsigned VR_EyeTexture(int eye);
void     VR_EyeTextureSize(int* w, int* h);

// Testing seam: exposes the OpenXR-quaternion to Quake-viewaxis conversion
// so it can be verified offline, without a headset.
void VR_TestQuatToAxis(const float quat[4], float outAxis[9]);

// Testing seam: composes a head rotation onto the game's view basis, the
// operation that makes mouse aim and head free-look coexist.
void VR_TestComposeAxis(const float head[9], const float game[9], float out[9]);

// Testing seam: proves whether an enabled scissor box clips glBlitFramebuffer.
// 1 = a pixel outside the box was written, 0 = it was clipped, -1 = no context.
int VR_TestBlitScissorBehaviour(bool disableScissor);

// Testing seams for eye-texture coverage: read one pixel back, and report the
// texture's size. A capture that fills only a corner still "succeeds", so
// coverage has to be checked by sampling, not by return codes.
int VR_TestSampleEyeTexture(int eye, int x, int y);
int VR_TestEyeTextureSize(int* w, int* h);

// Diagnostics for the log.
const char* VR_StatusText();




```


---

## `src/vr.cpp`  (1490 lines)

ALL OpenXR + the GL present path: scene target, capture, blit, submit, per-eye camera, FOV. THE MOST IMPORTANT FILE.

```cpp
﻿#include "vr.h"
#include "codvr.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

extern void Log(const char* fmt, ...);

// Resolved through dllmain, which owns the handle to the real driver.
extern "C" void* CODVR_GetGLProc(const char* name);

enum { EYES = 2 };

enum {
    GL_TEXTURE_2D_       = 0x0DE1,
    GL_BACK_             = 0x0405,
    GL_RGBA8_            = 0x8058,
    GL_SRGB8_ALPHA8_     = 0x8C43,
    GL_READ_FRAMEBUFFER_ = 0x8CA8,
    GL_DRAW_FRAMEBUFFER_ = 0x8CA9,
    GL_COLOR_ATTACHMENT0_= 0x8CE0,
    GL_COLOR_BUFFER_BIT_ = 0x4000,
    GL_LINEAR_           = 0x2601,
    GL_FRAMEBUFFER_COMPLETE_ = 0x8CD5
};

typedef void     (WINAPI *PFN_glReadBuffer)(unsigned);
typedef void     (WINAPI *PFN_glGenFramebuffers)(int, unsigned*);
typedef void     (WINAPI *PFN_glDeleteFramebuffers)(int, const unsigned*);
typedef void     (WINAPI *PFN_glBindFramebuffer)(unsigned, unsigned);
typedef void     (WINAPI *PFN_glFramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int);
typedef unsigned (WINAPI *PFN_glCheckFramebufferStatus)(unsigned);
typedef void     (WINAPI *PFN_glBlitFramebuffer)(int,int,int,int, int,int,int,int, unsigned, unsigned);
typedef unsigned (WINAPI *PFN_glGetError)(void);
typedef void     (WINAPI *PFN_glClearColor)(float,float,float,float);
typedef void     (WINAPI *PFN_glClear)(unsigned);
typedef void     (WINAPI *PFN_glEnable)(unsigned);
typedef void     (WINAPI *PFN_glDisable)(unsigned);
typedef unsigned char (WINAPI *PFN_glIsEnabled)(unsigned);
typedef void     (WINAPI *PFN_glGenTextures)(int, unsigned*);
typedef void     (WINAPI *PFN_glBindTexture)(unsigned, unsigned);
typedef void     (WINAPI *PFN_glTexImage2D)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*);
typedef void     (WINAPI *PFN_glTexParameteri)(unsigned, unsigned, int);

static PFN_glReadBuffer             gl_ReadBuffer;
static PFN_glGenFramebuffers        gl_GenFramebuffers;
static PFN_glDeleteFramebuffers     gl_DeleteFramebuffers;
static PFN_glBindFramebuffer        gl_BindFramebuffer;
static PFN_glFramebufferTexture2D   gl_FramebufferTexture2D;
static PFN_glCheckFramebufferStatus gl_CheckFramebufferStatus;
static PFN_glBlitFramebuffer        gl_BlitFramebuffer;
static PFN_glGetError               gl_GetError;
static PFN_glClearColor             gl_ClearColor;
static PFN_glClear                  gl_Clear;
static PFN_glEnable                 gl_Enable;
static PFN_glDisable                gl_Disable;
static PFN_glIsEnabled              gl_IsEnabled;
static PFN_glGenTextures            gl_GenTextures;
static PFN_glBindTexture            gl_BindTexture;
static PFN_glTexImage2D             gl_TexImage2D;
static PFN_glTexParameteri          gl_TexParameteri;

static unsigned g_blitFbo;
static unsigned g_capFbo;
static bool     g_glLoaded;

// One persistent colour texture per eye, holding the last full frame rendered
// for that eye (scene AND the 2D pass drawn over it). Frames alternate between
// eyes, so at any moment one texture is from this frame and the other from the
// previous one; both are submitted every frame.
enum { GL_SCISSOR_TEST_ = 0x0C11, GL_TEXTURE_MIN_FILTER_ = 0x2801,
       GL_TEXTURE_MAG_FILTER_ = 0x2800, GL_CLAMP_TO_EDGE_ = 0x812F,
       GL_TEXTURE_WRAP_S_ = 0x2802, GL_TEXTURE_WRAP_T_ = 0x2803,
       GL_UNSIGNED_BYTE_ = 0x1401, GL_RGBA_ = 0x1908 };
static unsigned g_eyeTex[EYES];
static int      g_capW, g_capH;
static bool     g_eyeTexValid[EYES];

// ------------------------------------------------- supersampled scene target
//
// Sharpness was limited by the game's own back buffer. Each eye needs roughly
// 3072x3264, but the engine draws into 2560x1440 -- so the vertical was being
// stretched about 2.3x on the way to the headset, and no amount of FOV work
// could recover detail that was never rendered.
//
// So stop rendering into the back buffer. We own opengl32, so we bind an
// offscreen colour+depth target of our own and scale every glViewport and
// glScissor the engine issues by the same factor. The engine still believes it
// is drawing at 2560x1440; every pixel it produces is simply larger. The 2D
// pass scales with it, so the HUD sharpens too, and geometry is untouched
// because the scale is uniform.
static unsigned g_sceneFbo, g_sceneTex, g_sceneDepth;
static int      g_sceneW, g_sceneH;       // supersampled size
static int      g_sceneBaseW, g_sceneBaseH;
static float    g_superSample = 2.0f;     // codvr.cfg "supersample"

// Aspect the 2D layer is drawn at inside the render buffer. Flat screens are
// read back from just that band, so a menu is not letterboxed twice on its way
// to the eye. 0 means the 2D layer fills the buffer.
static float    g_panelAspect = 0.0f;
static bool     g_sceneBound;
static bool     g_sceneFailed;

// The game's half-width will not match the headset's per-eye resolution, and
// glCopyTexSubImage2D cannot rescale. A framebuffer blit can, so that is what
// moves pixels from the back buffer into the swapchain.
static bool LoadGLFunctions()
{
    if (g_glLoaded)
        return gl_BlitFramebuffer != nullptr;
    g_glLoaded = true;

    gl_ReadBuffer             = (PFN_glReadBuffer)CODVR_GetGLProc("glReadBuffer");
    gl_GenFramebuffers        = (PFN_glGenFramebuffers)CODVR_GetGLProc("glGenFramebuffers");
    gl_DeleteFramebuffers     = (PFN_glDeleteFramebuffers)CODVR_GetGLProc("glDeleteFramebuffers");
    gl_BindFramebuffer        = (PFN_glBindFramebuffer)CODVR_GetGLProc("glBindFramebuffer");
    gl_FramebufferTexture2D   = (PFN_glFramebufferTexture2D)CODVR_GetGLProc("glFramebufferTexture2D");
    gl_CheckFramebufferStatus = (PFN_glCheckFramebufferStatus)CODVR_GetGLProc("glCheckFramebufferStatus");
    gl_BlitFramebuffer        = (PFN_glBlitFramebuffer)CODVR_GetGLProc("glBlitFramebuffer");
    gl_GetError               = (PFN_glGetError)CODVR_GetGLProc("glGetError");
    gl_ClearColor             = (PFN_glClearColor)CODVR_GetGLProc("glClearColor");
    gl_Clear                  = (PFN_glClear)CODVR_GetGLProc("glClear");
    gl_Enable                 = (PFN_glEnable)CODVR_GetGLProc("glEnable");
    gl_Disable                = (PFN_glDisable)CODVR_GetGLProc("glDisable");
    gl_IsEnabled              = (PFN_glIsEnabled)CODVR_GetGLProc("glIsEnabled");
    gl_GenTextures            = (PFN_glGenTextures)CODVR_GetGLProc("glGenTextures");
    gl_BindTexture            = (PFN_glBindTexture)CODVR_GetGLProc("glBindTexture");
    gl_TexImage2D             = (PFN_glTexImage2D)CODVR_GetGLProc("glTexImage2D");
    gl_TexParameteri          = (PFN_glTexParameteri)CODVR_GetGLProc("glTexParameteri");

    bool ok = gl_GenFramebuffers && gl_BindFramebuffer && gl_FramebufferTexture2D
           && gl_BlitFramebuffer && gl_ReadBuffer
           && gl_GenTextures && gl_BindTexture && gl_TexImage2D;
    Log("VR: GL blit path %s", ok ? "available" : "UNAVAILABLE (no FBO entry points)");
    return ok;
}

// ---------------------------------------------------------------- tunables

// Quake-family engines are built in inches: ~39.37 units per metre. Player eye
// height in CoD1 sits around 60 units, which lines up with a ~1.5 m standing
// eye once scaled this way.
static float g_unitsPerMetre = 39.37f;

// --------------------------------------------------------------- xr state

static XrInstance   g_instance   = XR_NULL_HANDLE;
static XrSystemId   g_systemId   = XR_NULL_SYSTEM_ID;
static XrSession    g_session    = XR_NULL_HANDLE;
static XrSpace      g_space      = XR_NULL_HANDLE;
static XrSessionState g_sessionState = XR_SESSION_STATE_UNKNOWN;

static XrSwapchain  g_swapchain[EYES] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
static XrSwapchainImageOpenGLKHR* g_images[EYES];
static uint32_t     g_imageCount[EYES];
static uint32_t     g_eyeW, g_eyeH;
static float        g_submitHalfX[EYES], g_submitHalfY[EYES];  // radians, what we rendered

static XrFrameState g_frameState = {};
static XrView       g_views[EYES];
static bool         g_viewsValid;
static bool         g_everLocated;   // have we ever had a good pose?
static bool         g_frameBegun;
static bool         g_sessionRunning;
static bool         g_startupFailed;
static DWORD        g_lastRetryTick;
static char         g_status[256] = "not started";

static HDC   g_hdc;
static HGLRC g_glrc;

// Head position at startup, in Quake units, used as the neutral point.
//
// The LOCAL reference space puts its origin wherever the headset was when the
// session began -- typically 1.2-1.7 m off the floor. The game's vieworg is
// ALREADY the player's eye position (~60 units up), so adding the head's
// absolute position on top lifted the camera another ~50-67 units, into or
// through the ceiling: the view renders black and only the 2D HUD survives.
// Subtracting this reference makes the startup pose the neutral point, so the
// game's own eye height is respected and only real leaning/crouching/stepping
// moves the camera. Captured from eye 0 and applied to BOTH eyes, so the
// inter-eye separation (IPD) is preserved.
static float g_headRef[3];
static bool  g_headRefValid;

// Yaw the headset was facing when the session began. Cancelling it makes
// "looking straight ahead" mean along the gun, whichever way the player happened
// to be facing at the time. Re-latched by VR_Recenter().
static float g_headYawRef;
static bool  g_headYawRefValid;

// Where the head is pointing in the game's world, in Quake basis form. The
// input layer reads this to steer the game's own aim toward the view.
static float g_headWorldAxis[9];
static bool  g_headWorldValid;

// How far the head is turned away from the game's aim, in degrees. Positive
// yaw is to the left, positive pitch is up. Used both for head-aim steering and
// for the comfort vignette.
static float g_headYawOffsetDeg;
static float g_headPitchOffsetDeg;

// Player motion, measured from the game's own camera between frames. Vection --
// the sick feeling from seeing movement you cannot feel -- tracks translation
// speed and turn rate, so the comfort vignette keys off both.
static float g_prevOrigin[3];
static bool  g_prevOriginValid;
static float g_prevAimYaw;
static bool  g_prevAimYawValid;
static DWORD g_prevMotionTick;
static float g_playerSpeed;       // engine units per second
static float g_turnRate;          // degrees per second

// Pose the 2D panel is pinned to. A flat screen (menu, briefing, loading) has
// no world position of its own, so it was submitted at the live head pose --
// which nails it to your face and makes it swim as you look around. Latching
// the pose once, when the panel first appears, leaves it hanging in the world
// so you can look around it instead of dragging it with you.
static XrPosef g_panelPose[EYES];
static bool    g_panelPinned;

// The pose each eye's texture was actually RENDERED with. Because frames
// alternate, one eye's image is always a frame old; submitting it with the
// current pose would tell the compositor the picture is newer than it is and
// leave that eye visibly offset. Handing back the pose it was drawn at lets
// reprojection put it where it belongs.
static XrPosef g_eyePose[EYES];
static bool    g_eyePoseValid[EYES];

static const char* ResStr(XrResult r)
{
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (g_instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(g_instance, r, buf)))
        return buf;
    sprintf_s(buf, "XrResult(%d)", (int)r);
    return buf;
}

#define XR_TRY(expr, what)                                              \
    do {                                                                \
        XrResult _r = (expr);                                           \
        if (XR_FAILED(_r)) {                                            \
            Log("VR: %s failed -> %s", what, ResStr(_r));               \
            sprintf_s(g_status, "%s failed: %s", what, ResStr(_r));     \
            return false;                                               \
        }                                                               \
    } while (0)

const char* VR_StatusText() { return g_status; }

void VR_SetWorldScale(float u) { if (u > 1.0f && u < 1000.0f) g_unitsPerMetre = u; }

// Re-zero the headset: whatever the player is facing and wherever they are
// sitting becomes "straight ahead, centred". Essential in seated VR -- tracking
// drifts, and people start a session facing the wrong way.
void VR_Recenter()
{
    g_headRefValid    = false;
    g_headYawRefValid = false;
    Log("VR: recentred -- current head position and facing are now neutral");
}

// How far the head is currently turned away from the game's aim.
void VR_GetHeadAimOffset(float* yawDeg, float* pitchDeg)
{
    if (yawDeg)   *yawDeg   = g_headYawOffsetDeg;
    if (pitchDeg) *pitchDeg = g_headPitchOffsetDeg;
}

// Virtual motion the player cannot physically feel: units/second of travel and
// degrees/second of turning.
void VR_GetPlayerMotion(float* speed, float* turnRateDeg)
{
    if (speed)       *speed       = g_playerSpeed;
    if (turnRateDeg) *turnRateDeg = g_turnRate;
}

// The eye texture the comfort layer draws its overlay into.
unsigned VR_EyeTexture(int eye)
{
    return (eye >= 0 && eye < EYES) ? g_eyeTex[eye] : 0;
}

void VR_EyeTextureSize(int* w, int* h)
{
    if (w) *w = g_capW;
    if (h) *h = g_capH;
}

// Testing seam: does an enabled scissor box actually clip glBlitFramebuffer?
// This is the exact mechanism blamed for the missing top of the view -- the
// engine leaves a 2560x1440 scissor set while we blit into a 3072x3264 eye
// image -- so prove it against a real driver instead of trusting the spec.
//
// Paints the back buffer red, sets a 64x64 scissor box, blits the full 256x256
// into a texture, then samples a pixel far outside that box.
// Returns 1 if the far pixel was written, 0 if it was clipped away, -1 if the
// check could not run.
// Testing seam: read one pixel back out of an eye texture.
//
// The capture and the blit that follows it have to cover the WHOLE eye texture.
// A source rectangle left in the wrong coordinate space silently fills only a
// corner and stretches it over the eye -- which shipped once as a hugely zoomed
// view, and no existing test noticed because every call still "succeeded".
// Sampling a far corner is what actually proves coverage.
int VR_TestSampleEyeTexture(int eye, int x, int y)
{
    if (eye < 0 || eye >= EYES || !g_eyeTex[eye])
        return -1;
    if (!LoadGLFunctions())
        return -1;
    typedef void (WINAPI *ReadPixFn)(int,int,int,int,unsigned,unsigned,void*);
    static ReadPixFn glReadPx = (ReadPixFn)CODVR_GetGLProc("glReadPixels");
    if (!glReadPx)
        return -1;
    if (!g_capFbo)
        gl_GenFramebuffers(1, &g_capFbo);

    unsigned char px[4] = { 0, 0, 0, 0 };
    gl_BindFramebuffer(GL_READ_FRAMEBUFFER_, g_capFbo);
    gl_FramebufferTexture2D(GL_READ_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_,
                            GL_TEXTURE_2D_, g_eyeTex[eye], 0);
    glReadPx(x, y, 1, 1, GL_RGBA_, GL_UNSIGNED_BYTE_, px);
    gl_BindFramebuffer(GL_READ_FRAMEBUFFER_, 0);
    return px[0];
}

int VR_TestEyeTextureSize(int* w, int* h)
{
    if (w) *w = g_capW;
    if (h) *h = g_capH;
    return g_capW > 0 && g_capH > 0;
}

int VR_TestBlitScissorBehaviour(bool disableScissor)
{
    if (!LoadGLFunctions())
        return -1;

    typedef void (WINAPI *ScissorFn)(int,int,int,int);
    typedef void (WINAPI *ReadPixFn)(int,int,int,int,unsigned,unsigned,void*);
    ScissorFn glSciss  = (ScissorFn)CODVR_GetGLProc("glScissor");
    ReadPixFn glReadPx = (ReadPixFn)CODVR_GetGLProc("glReadPixels");
    if (!glSciss || !glReadPx || !gl_Enable || !gl_Disable
        || !gl_ClearColor || !gl_Clear)
        return -1;

    const int N = 256;
    static unsigned tex = 0, fbo = 0;
    if (!tex) {
        gl_GenTextures(1, &tex);
        gl_BindTexture(GL_TEXTURE_2D_, tex);
        gl_TexImage2D(GL_TEXTURE_2D_, 0, GL_RGBA8_, N, N, 0,
                      GL_RGBA_, GL_UNSIGNED_BYTE_, nullptr);
        gl_BindTexture(GL_TEXTURE_2D_, 0);
    }
    if (!fbo)
        gl_GenFramebuffers(1, &fbo);

    // Known non-zero source, and a destination cleared to something else so an
    // unwritten pixel is unmistakable.
    gl_BindFramebuffer(GL_DRAW_FRAMEBUFFER_, fbo);
    gl_FramebufferTexture2D(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_,
                            GL_TEXTURE_2D_, tex, 0);
    gl_Disable(GL_SCISSOR_TEST_);
    gl_ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl_Clear(GL_COLOR_BUFFER_BIT_);
    gl_BindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0);

    gl_ClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    gl_Clear(GL_COLOR_BUFFER_BIT_);

    glSciss(0, 0, 64, 64);
    gl_Enable(GL_SCISSOR_TEST_);
    if (disableScissor)
        gl_Disable(GL_SCISSOR_TEST_);

    gl_BindFramebuffer(GL_DRAW_FRAMEBUFFER_, fbo);
    gl_FramebufferTexture2D(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_,
                            GL_TEXTURE_2D_, tex, 0);
    gl_BindFramebuffer(GL_READ_FRAMEBUFFER_, 0);
    gl_ReadBuffer(GL_BACK_);
    gl_BlitFramebuffer(0, 0, N, N, 0, 0, N, N, GL_COLOR_BUFFER_BIT_, GL_LINEAR_);

    gl_Disable(GL_SCISSOR_TEST_);

    // Sample well outside the scissor box.
    unsigned char px[4] = { 0, 0, 0, 0 };
    gl_BindFramebuffer(GL_READ_FRAMEBUFFER_, fbo);
    gl_FramebufferTexture2D(GL_READ_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_,
                            GL_TEXTURE_2D_, tex, 0);
    glReadPx(200, 200, 1, 1, GL_RGBA_, GL_UNSIGNED_BYTE_, px);

    gl_BindFramebuffer(GL_READ_FRAMEBUFFER_, 0);
    gl_BindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0);

    return px[0] > 128 ? 1 : 0;
}

// ------------------------------------------------------ coordinate mapping

// OpenXR (+X right, +Y up, -Z forward) -> Quake (+X forward, +Y left, +Z up).
static inline void XrToQuake(const XrVector3f& v, float out[3])
{
    out[0] = -v.z;
    out[1] = -v.x;
    out[2] =  v.y;
}

// Rotate a quaternion's basis vectors, then convert each into Quake space.
static void QuatToQuakeAxis(const XrQuaternionf& q, float axis[9])
{
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    // Columns of the rotation matrix: right, up, back (forward is -back).
    XrVector3f right = { 1 - 2*(y*y + z*z),     2*(x*y + z*w),     2*(x*z - y*w) };
    XrVector3f up    = {     2*(x*y - z*w), 1 - 2*(x*x + z*z),     2*(y*z + x*w) };
    XrVector3f back  = {     2*(x*z + y*w),     2*(y*z - x*w), 1 - 2*(x*x + y*y) };
    XrVector3f fwd   = { -back.x, -back.y, -back.z };
    XrVector3f left  = { -right.x, -right.y, -right.z };

    float f[3], l[3], u[3];
    XrToQuake(fwd,  f);
    XrToQuake(left, l);
    XrToQuake(up,   u);
    axis[0]=f[0]; axis[1]=f[1]; axis[2]=f[2];
    axis[3]=l[0]; axis[4]=l[1]; axis[5]=l[2];
    axis[6]=u[0]; axis[7]=u[1]; axis[8]=u[2];
}

// Rotate a vector about Quake's up axis (+Z).
static void RotateZ(float yawCos, float yawSin, const float in[3], float out[3])
{
    out[0] = in[0]*yawCos - in[1]*yawSin;
    out[1] = in[0]*yawSin + in[1]*yawCos;
    out[2] = in[2];
}

// Compose a head rotation onto the game's own view orientation.
//
// Both are Quake-style bases: three rows (forward, left, up) giving those
// directions in the parent frame. Treating the head's rows as coefficients over
// the game's rows applies the head rotation *within* the game's view frame,
// which is what makes head movement behave as free-look on top of wherever the
// player has aimed.
//
// The previous version kept only the game's YAW and let the headset own pitch
// and roll entirely, which is why mouse pitch did nothing: aiming up or down
// moved the weapon but never the view. Composing the full basis keeps the mouse
// in charge of aim -- with the head centred the view looks exactly along the
// gun -- while the head still adds a free delta on top.
static void ComposeAxis(const float head[9], const float game[9], float out[9])
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out[r*3 + c] = head[r*3 + 0] * game[0*3 + c]
                         + head[r*3 + 1] * game[1*3 + c]
                         + head[r*3 + 2] * game[2*3 + c];
        }
    }
}

// Transpose of a rotation basis, i.e. its inverse.
static void TransposeAxis(const float in[9], float out[9])
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[r*3 + c] = in[c*3 + r];
}

void VR_TestQuatToAxis(const float quat[4], float outAxis[9])
{
    XrQuaternionf q; q.x = quat[0]; q.y = quat[1]; q.z = quat[2]; q.w = quat[3];
    QuatToQuakeAxis(q, outAxis);
}

void VR_TestComposeAxis(const float head[9], const float game[9], float out[9])
{
    ComposeAxis(head, game, out);
}

// ------------------------------------------------------------ swapchains

static int64_t PickSwapchainFormat()
{
    uint32_t count = 0;
    if (XR_FAILED(xrEnumerateSwapchainFormats(g_session, 0, &count, nullptr)) || !count)
        return GL_RGBA8_;
    int64_t* formats = new int64_t[count];
    xrEnumerateSwapchainFormats(g_session, count, &count, formats);

    // Prefer LINEAR RGBA8. The engine draws into a linear back buffer and we
    // raw-blit those bytes across, so an sRGB swapchain makes the compositor
    // re-interpret linear values as encoded ones and the whole image reads
    // dark. Only fall back to sRGB if the runtime offers nothing else.
    int64_t chosen = formats[0];
    for (uint32_t i = 0; i < count; ++i) {
        if (formats[i] == GL_RGBA8_)        { chosen = formats[i]; break; }
        if (formats[i] == GL_SRGB8_ALPHA8_) { chosen = formats[i]; }
    }
    delete[] formats;
    return chosen;
}

static bool CreateSwapchains()
{
    uint32_t viewCount = 0;
    XR_TRY(xrEnumerateViewConfigurationViews(g_instance, g_systemId,
             XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr),
           "xrEnumerateViewConfigurationViews");
    if (viewCount != EYES) {
        Log("VR: expected %d views, runtime reports %u", EYES, viewCount);
        return false;
    }

    XrViewConfigurationView vv[EYES];
    for (int i = 0; i < EYES; ++i) { vv[i] = {}; vv[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW; }
    XR_TRY(xrEnumerateViewConfigurationViews(g_instance, g_systemId,
             XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, EYES, &viewCount, vv),
           "xrEnumerateViewConfigurationViews(fill)");

    g_eyeW = vv[0].recommendedImageRectWidth;
    g_eyeH = vv[0].recommendedImageRectHeight;
    const int64_t format = PickSwapchainFormat();
    Log("VR: per-eye %ux%u, swapchain format 0x%llX", g_eyeW, g_eyeH, (long long)format);

    for (int i = 0; i < EYES; ++i) {
        XrSwapchainCreateInfo sci = {};
        sci.type        = XR_TYPE_SWAPCHAIN_CREATE_INFO;
        sci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT
                        | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sci.format      = format;
        sci.sampleCount = 1;
        sci.width       = g_eyeW;
        sci.height      = g_eyeH;
        sci.faceCount   = 1;
        sci.arraySize   = 1;
        sci.mipCount    = 1;
        XR_TRY(xrCreateSwapchain(g_session, &sci, &g_swapchain[i]), "xrCreateSwapchain");

        XR_TRY(xrEnumerateSwapchainImages(g_swapchain[i], 0, &g_imageCount[i], nullptr),
               "xrEnumerateSwapchainImages");
        g_images[i] = new XrSwapchainImageOpenGLKHR[g_imageCount[i]];
        for (uint32_t k = 0; k < g_imageCount[i]; ++k) {
            g_images[i][k] = {};
            g_images[i][k].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        }
        XR_TRY(xrEnumerateSwapchainImages(g_swapchain[i], g_imageCount[i], &g_imageCount[i],
                 (XrSwapchainImageBaseHeader*)g_images[i]),
               "xrEnumerateSwapchainImages(fill)");
    }
    return true;
}

// ---------------------------------------------------------------- startup

bool VR_Startup(HDC hdc, HGLRC glrc)
{
    if (g_session != XR_NULL_HANDLE)
        return true;
    if (g_startupFailed)
        return false;
    if (!hdc || !glrc)
        return false;

    // The headset may be asleep when the game launches. Retry slowly rather
    // than giving up, so putting it on mid-session still works.
    DWORD now = GetTickCount();
    if (g_lastRetryTick && (now - g_lastRetryTick) < 3000)
        return false;
    g_lastRetryTick = now;

    // openxr_loader.dll is delay-loaded, so touching any xr* function without
    // it present would raise an exception rather than fail politely. Check for
    // it once, and if it is absent stay out of the way: the game then runs
    // exactly as it would with no mod installed.
    static int loaderState = 0;          // 0 unknown, 1 present, -1 absent
    if (loaderState == 0) {
        loaderState = LoadLibraryA("openxr_loader.dll") ? 1 : -1;
        if (loaderState < 0) {
            Log("VR: openxr_loader.dll not found beside the game exe.");
            Log("VR: continuing flat -- the game is unaffected.");
            sprintf_s(g_status, "openxr_loader.dll missing");
            g_startupFailed = true;
        }
    }
    if (loaderState < 0)
        return false;

    g_hdc = hdc;
    g_glrc = glrc;

    if (g_instance == XR_NULL_HANDLE) {
        const char* exts[] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
        XrInstanceCreateInfo ici = {};
        ici.type = XR_TYPE_INSTANCE_CREATE_INFO;
        ici.enabledExtensionCount = 1;
        ici.enabledExtensionNames = exts;
        strcpy_s(ici.applicationInfo.applicationName, "Call of Duty VR");
        ici.applicationInfo.applicationVersion = 1;
        strcpy_s(ici.applicationInfo.engineName, "idTech3/IW");
        ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
        XR_TRY(xrCreateInstance(&ici, &g_instance), "xrCreateInstance");

        XrInstanceProperties ip = {};
        ip.type = XR_TYPE_INSTANCE_PROPERTIES;
        if (XR_SUCCEEDED(xrGetInstanceProperties(g_instance, &ip)))
            Log("VR: runtime %s", ip.runtimeName);
    }

    XrSystemGetInfo sgi = {};
    sgi.type = XR_TYPE_SYSTEM_GET_INFO;
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult r = xrGetSystem(g_instance, &sgi, &g_systemId);
    if (XR_FAILED(r)) {
        // Not fatal: the headset simply is not presenting itself yet. Say so
        // out loud, though -- staying silent here previously made a plain
        // "headset not connected" look like a mod failure.
        static DWORD lastMoan = 0;
        static int   moans = 0;
        if (moans < 3 || (now - lastMoan) > 15000) {
            lastMoan = now;
            ++moans;
            Log("VR: no headset yet (%s)", ResStr(r));
            if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE)
                Log("VR: the runtime is reachable but no headset is presenting. With "
                    "Virtual Desktop you must launch the VD app INSIDE the Quest and "
                    "connect to this PC -- the PC-side Streamer alone is not enough.");
        }
        sprintf_s(g_status, "waiting for headset (%s)", ResStr(r));
        return false;
    }
    Log("VR: headset detected, creating session");

    // The GL context must be current on this thread when the session is made.
    XrGraphicsBindingOpenGLWin32KHR bind = {};
    bind.type  = XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR;
    bind.hDC   = g_hdc;
    bind.hGLRC = g_glrc;

    // Runtimes require the graphics requirements query before session creation
    // even though we ignore the returned version range.
    PFN_xrGetOpenGLGraphicsRequirementsKHR getReq = nullptr;
    if (XR_SUCCEEDED(xrGetInstanceProcAddr(g_instance, "xrGetOpenGLGraphicsRequirementsKHR",
                                           (PFN_xrVoidFunction*)&getReq)) && getReq) {
        XrGraphicsRequirementsOpenGLKHR req = {};
        req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR;
        getReq(g_instance, g_systemId, &req);
    }

    XrSessionCreateInfo sci = {};
    sci.type = XR_TYPE_SESSION_CREATE_INFO;
    sci.next = &bind;
    sci.systemId = g_systemId;
    XR_TRY(xrCreateSession(g_instance, &sci, &g_session), "xrCreateSession");

    XrReferenceSpaceCreateInfo rsci = {};
    rsci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;   // seated-origin
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    XR_TRY(xrCreateReferenceSpace(g_session, &rsci, &g_space), "xrCreateReferenceSpace");

    if (!CreateSwapchains()) {
        g_startupFailed = true;
        return false;
    }

    for (int i = 0; i < EYES; ++i) { g_views[i] = {}; g_views[i].type = XR_TYPE_VIEW; }

    sprintf_s(g_status, "session created, waiting for READY");
    Log("VR: session created");
    return true;
}

bool VR_Active()
{
    // Deliberately not gated on this frame's pose validity. xrWaitFrame can
    // legitimately report nothing to render, and if that flipped us back to
    // the flat menu panel mid-mission the layout would visibly switch between
    // stereo and a small bordered screen from one frame to the next. Once we
    // have located the views even once, we keep rendering stereo and reuse the
    // last good pose.
    return g_session != XR_NULL_HANDLE && g_sessionRunning && g_everLocated;
}

// ------------------------------------------------------------- frame loop

static void PumpEvents()
{
    XrEventDataBuffer ev = {};
    for (;;) {
        ev = {};
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        if (xrPollEvent(g_instance, &ev) != XR_SUCCESS)
            break;

        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged* s =
                (const XrEventDataSessionStateChanged*)&ev;
            g_sessionState = s->state;
            Log("VR: session state -> %d", (int)g_sessionState);

            if (g_sessionState == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = {};
                bi.type = XR_TYPE_SESSION_BEGIN_INFO;
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(g_session, &bi))) {
                    g_sessionRunning = true;
                    sprintf_s(g_status, "running");
                    Log("VR: session begun");
                }
            } else if (g_sessionState == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g_session);
                g_sessionRunning = false;
                sprintf_s(g_status, "stopped");
            } else if (g_sessionState == XR_SESSION_STATE_EXITING
                    || g_sessionState == XR_SESSION_STATE_LOSS_PENDING) {
                g_sessionRunning = false;
                sprintf_s(g_status, "exiting");
            }
        }
    }
}

bool VR_BeginFrame()
{
    if (g_session == XR_NULL_HANDLE)
        return false;

    PumpEvents();
    if (!g_sessionRunning)
        return false;

    g_frameState = {};
    g_frameState.type = XR_TYPE_FRAME_STATE;
    XrFrameWaitInfo fwi = {};
    fwi.type = XR_TYPE_FRAME_WAIT_INFO;
    if (XR_FAILED(xrWaitFrame(g_session, &fwi, &g_frameState)))
        return false;

    XrFrameBeginInfo fbi = {};
    fbi.type = XR_TYPE_FRAME_BEGIN_INFO;
    if (XR_FAILED(xrBeginFrame(g_session, &fbi)))
        return false;
    g_frameBegun = true;

    g_viewsValid = false;
    if (!g_frameState.shouldRender)
        return false;

    XrViewLocateInfo vli = {};
    vli.type = XR_TYPE_VIEW_LOCATE_INFO;
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = g_frameState.predictedDisplayTime;
    vli.space = g_space;

    XrViewState vs = {};
    vs.type = XR_TYPE_VIEW_STATE;
    uint32_t got = 0;
    if (XR_FAILED(xrLocateViews(g_session, &vli, &vs, EYES, &got, g_views)) || got != EYES)
        return false;

    const XrViewStateFlags need = XR_VIEW_STATE_POSITION_VALID_BIT
                                | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    g_viewsValid = (vs.viewStateFlags & need) == need;
    if (g_viewsValid)
        g_everLocated = true;   // keep these poses as the fallback
    return g_viewsValid;
}

void VR_GetEyeCamera(int eye,
                     const float gameOrigin[3], const float gameAxis[9],
                     int viewW, int viewH,
                     float outOrigin[3], float outAxis[9],
                     float* fovXdeg, float* fovYdeg)
{
    // ---- orientation: mouse aims, the head free-looks on top of it ----
    float headAxis[9];
    QuatToQuakeAxis(g_views[eye].pose.orientation, headAxis);

    // Re-zero the headset's yaw so "looking straight ahead" means along the
    // gun, wherever the player happened to be facing when the session began.
    // Latched from eye 0 so both eyes share one reference; re-latched by
    // VR_Recenter(). Only yaw is cancelled -- cancelling pitch or roll would
    // bake a tilted starting posture into the horizon permanently.
    if (!g_headYawRefValid && eye == 0) {
        g_headYawRefValid = true;
        g_headYawRef = atan2f(headAxis[1], headAxis[0]);
        Log("VR: head yaw neutral latched at %.1f degrees", g_headYawRef * 57.2957795f);
    }
    float headLocal[9];
    if (g_headYawRefValid) {
        const float rc = cosf(-g_headYawRef), rs = sinf(-g_headYawRef);
        for (int row = 0; row < 3; ++row)
            RotateZ(rc, rs, &headAxis[row * 3], &headLocal[row * 3]);
    } else {
        memcpy(headLocal, headAxis, sizeof(headLocal));
    }

    // The game's own view basis carries the player's aim -- BOTH yaw and pitch
    // from the mouse. Composing the head onto all of it (rather than the yaw
    // alone, as before) is what finally lets mouse pitch move the view.
    ComposeAxis(headLocal, gameAxis, outAxis);

    // Remember where the head is pointing in world terms, and by how much it
    // differs from the game's aim. The input layer steers the game toward this;
    // the comfort vignette watches how fast it changes.
    if (eye == 0) {
        memcpy(g_headWorldAxis, outAxis, sizeof(g_headWorldAxis));
        g_headWorldValid = true;

        const float rad2deg = 57.2957795f;
        // Yaw difference between the composed view and the game's own aim.
        const float viewYaw = atan2f(outAxis[1], outAxis[0]);
        const float aimYaw  = atan2f(gameAxis[1], gameAxis[0]);
        float dy = viewYaw - aimYaw;
        while (dy >  3.14159265f) dy -= 6.28318531f;
        while (dy < -3.14159265f) dy += 6.28318531f;
        g_headYawOffsetDeg = dy * rad2deg;

        // Pitch is the elevation of each forward vector.
        const float viewPitch = asinf(outAxis[2]  > 1.0f ? 1.0f :
                                     (outAxis[2]  < -1.0f ? -1.0f : outAxis[2]));
        const float aimPitch  = asinf(gameAxis[2] > 1.0f ? 1.0f :
                                     (gameAxis[2] < -1.0f ? -1.0f : gameAxis[2]));
        g_headPitchOffsetDeg = (viewPitch - aimPitch) * rad2deg;

        // Translation speed and turn rate, for the comfort vignette. Measured
        // from the game's camera rather than the head, because it is the
        // *virtual* motion the player cannot feel that causes sickness --
        // physically turning your own head never does.
        const DWORD now = GetTickCount();
        if (g_prevMotionTick != 0) {
            float dt = (now - g_prevMotionTick) * 0.001f;
            if (dt > 0.001f && dt < 0.5f) {
                if (g_prevOriginValid) {
                    const float dx = gameOrigin[0] - g_prevOrigin[0];
                    const float dy = gameOrigin[1] - g_prevOrigin[1];
                    const float dz = gameOrigin[2] - g_prevOrigin[2];
                    const float dist = sqrtf(dx*dx + dy*dy + dz*dz);
                    // Ignore teleports (level load, respawn), which would
                    // otherwise slam the vignette shut for a moment.
                    const float inst = (dist < 400.0f) ? dist / dt : 0.0f;
                    g_playerSpeed += (inst - g_playerSpeed) * 0.25f;   // smoothed
                }
                if (g_prevAimYawValid) {
                    float dyaw = aimYaw - g_prevAimYaw;
                    while (dyaw >  3.14159265f) dyaw -= 6.28318531f;
                    while (dyaw < -3.14159265f) dyaw += 6.28318531f;
                    const float inst = fabsf(dyaw) * rad2deg / dt;
                    g_turnRate += (inst - g_turnRate) * 0.25f;
                }
            }
        }
        g_prevMotionTick = now;
        g_prevOrigin[0] = gameOrigin[0];
        g_prevOrigin[1] = gameOrigin[1];
        g_prevOrigin[2] = gameOrigin[2];
        g_prevOriginValid = true;
        g_prevAimYaw = aimYaw;
        g_prevAimYawValid = true;
    }

    // Sanity-gate the pose before trusting its position. A real run reported
    // y = -1592.886 m, i.e. the runtime handed us a head position a mile
    // underground. Latching that as the neutral point happened to work only
    // because we subtract it again; had a later pose come back sane, the delta
    // would have flung the camera 1.6 km. Anything beyond a plausible room is
    // rejected and we fall back to rotation-only for that frame.
    const XrVector3f& raw = g_views[eye].pose.position;
    const bool posSane = fabsf(raw.x) < 10.0f && fabsf(raw.y) < 10.0f
                      && fabsf(raw.z) < 10.0f;

    float headDelta[3] = { 0, 0, 0 };
    if (posSane) {
        float headPos[3];
        XrToQuake(raw, headPos);
        for (int i = 0; i < 3; ++i)
            headPos[i] *= g_unitsPerMetre;

        // Latch the neutral point from the first SANE eye-0 pose, then work
        // relative to it. Both eyes share the reference so IPD survives.
        if (!g_headRefValid && eye == 0) {
            g_headRefValid = true;
            g_headRef[0] = headPos[0];
            g_headRef[1] = headPos[1];
            g_headRef[2] = headPos[2];
            Log("VR: head neutral point latched at %.1f %.1f %.1f units "
                "(raw pose %.3f %.3f %.3f m)",
                headPos[0], headPos[1], headPos[2], raw.x, raw.y, raw.z);
        }
        if (g_headRefValid) {
            // Clamp to a believable amount of leaning/stepping, so a tracking
            // glitch can never shove the camera through the level.
            const float limit = 2.0f * g_unitsPerMetre;
            for (int i = 0; i < 3; ++i) {
                float d = headPos[i] - g_headRef[i];
                if (d >  limit) d =  limit;
                if (d < -limit) d = -limit;
                headDelta[i] = d;
            }
        }
    } else {
        static bool moaned = false;
        if (!moaned) {
            moaned = true;
            Log("VR: implausible head position %.3f %.3f %.3f m -- ignoring "
                "positional tracking, using rotation only", raw.x, raw.y, raw.z);
        }
    }

    // Lean and step have to land in world space. The delta is in room space, so
    // rotate it by the body's facing (the game's aim yaw) minus the neutral yaw
    // that was cancelled out of the orientation, keeping the two consistent --
    // otherwise leaning left would move the camera in the wrong direction by
    // however far the player was turned when the session started.
    const float bodyYaw = atan2f(gameAxis[1], gameAxis[0])
                        - (g_headYawRefValid ? g_headYawRef : 0.0f);
    const float byc = cosf(bodyYaw), bys = sinf(bodyYaw);

    float rotated[3];
    RotateZ(byc, bys, headDelta, rotated);
    outOrigin[0] = gameOrigin[0] + rotated[0];
    outOrigin[1] = gameOrigin[1] + rotated[1];
    outOrigin[2] = gameOrigin[2] + rotated[2];

    // One-shot: prove on paper where the eye camera actually ended up
    // relative to where the game wanted it. A large offset here means the
    // view is being pushed into geometry, which reads as a black screen.
    static bool originLogged = false;
    if (!originLogged && eye == 1) {
        originLogged = true;
        Log("VR: game eye origin %.1f %.1f %.1f -> eye1 origin %.1f %.1f %.1f "
            "(offset %.1f %.1f %.1f)",
            gameOrigin[0], gameOrigin[1], gameOrigin[2],
            outOrigin[0], outOrigin[1], outOrigin[2],
            rotated[0], rotated[1], rotated[2]);
    }

    // refdef carries only a symmetric fov, while OpenXR frusta are asymmetric.
    // Render the smallest SYMMETRIC frustum that CONTAINS the runtime's, then
    // submit the sub-rectangle matching the real one (see VR_SubmitFrame).
    //
    // Both angles come from the runtime, NOT from the viewport aspect. The
    // superset must contain the headset's frustum on both axes or the submitted
    // sub-rectangle falls outside the rendered image and the picture gets
    // stretched. The viewport aspect no longer matches the frustum aspect, which
    // simply means non-square pixels -- harmless, because we report the exact
    // angles we rendered. (Deriving fov_y from the aspect was safe only while
    // each eye owned a tall half of the back buffer; on the full 16:9 buffer it
    // yields roughly 75 degrees against a headset that wants 110.)
    (void)viewW; (void)viewH;
    const XrFovf& f = g_views[eye].fov;
    float halfX = fabsf(f.angleLeft) > fabsf(f.angleRight)
                ? fabsf(f.angleLeft) : fabsf(f.angleRight);
    float halfY = fabsf(f.angleUp) > fabsf(f.angleDown)
                ? fabsf(f.angleUp) : fabsf(f.angleDown);

    g_submitHalfX[eye] = halfX;
    g_submitHalfY[eye] = halfY;

    const float rad2deg = 57.2957795f;
    *fovXdeg = 2.0f * halfX * rad2deg;
    *fovYdeg = 2.0f * halfY * rad2deg;
}

// --------------------------------------------------------------- submit

// Blit one half of the back buffer into an eye's swapchain image, rescaling to
// the headset's per-eye resolution. The engine has already drawn both eyes
// side by side by this point, so this stays entirely on the GPU.
// centred=true fits the source into the middle of the eye image preserving its
// aspect ratio, clearing the rest. Used for 2D screens: stretching a 16:9 menu
// across a nearly square eye buffer put it off-centre and clipped, and filling
// the entire field of view with a flat panel is uncomfortable to read.
float VR_Supersample() { return g_sceneBound ? g_superSample : 1.0f; }

void VR_SetSupersample(float s)
{
    if (s >= 1.0f && s <= 4.0f)
        g_superSample = s;
}

void VR_SetPanelAspect(float a) { g_panelAspect = (a > 0.0f) ? a : 0.0f; }

// Create (or resize) the offscreen target the engine will draw into, sized to
// the game's resolution times the supersample factor.
static bool EnsureSceneTarget(int baseW, int baseH)
{
    if (g_sceneFailed || baseW < 2 || baseH < 2)
        return false;
    if (!LoadGLFunctions())
        return false;
    if (g_sceneFbo && baseW == g_sceneBaseW && baseH == g_sceneBaseH)
        return true;

    typedef void (WINAPI *GenRbFn)(int, unsigned*);
    typedef void (WINAPI *BindRbFn)(unsigned, unsigned);
    typedef void (WINAPI *RbStorageFn)(unsigned, unsigned, int, int);
    typedef void (WINAPI *FbRbFn)(unsigned, unsigned, unsigned, unsigned);
    static GenRbFn     glGenRb   = (GenRbFn)CODVR_GetGLProc("glGenRenderbuffers");
    static BindRbFn    glBindRb  = (BindRbFn)CODVR_GetGLProc("glBindRenderbuffer");
    static RbStorageFn glRbStore = (RbStorageFn)CODVR_GetGLProc("glRenderbufferStorage");
    static FbRbFn      glFbRb    = (FbRbFn)CODVR_GetGLProc("glFramebufferRenderbuffer");
    enum { GL_RENDERBUFFER_ = 0x8D41, GL_DEPTH24_STENCIL8_ = 0x88F0,
           GL_DEPTH_STENCIL_ATTACHMENT_ = 0x821A, GL_FRAMEBUFFER_ = 0x8D40 };

    if (!glGenRb || !glBindRb || !glRbStore || !glFbRb) {
        Log("VR: no renderbuffer entry points; supersampling unavailable");
        g_sceneFailed = true;
        return false;
    }

    int w = (int)(baseW * g_superSample + 0.5f);
    int h = (int)(baseH * g_superSample + 0.5f);

    if (!g_sceneFbo)   gl_GenFramebuffers(1, &g_sceneFbo);
    if (!g_sceneTex)   gl_GenTextures(1, &g_sceneTex);
    if (!g_sceneDepth) glGenRb(1, &g_sceneDepth);

    gl_BindTexture(GL_TEXTURE_2D_, g_sceneTex);
    gl_TexImage2D(GL_TEXTURE_2D_, 0, GL_RGBA8_, w, h, 0,
                  GL_RGBA_, GL_UNSIGNED_BYTE_, nullptr);
    if (gl_TexParameteri) {
        gl_TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, GL_LINEAR_);
        gl_TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MAG_FILTER_, GL_LINEAR_);
        gl_TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_S_, GL_CLAMP_TO_EDGE_);
        gl_TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_T_, GL_CLAMP_TO_EDGE_);
    }
    gl_BindTexture(GL_TEXTURE_2D_, 0);

    glBindRb(GL_RENDERBUFFER_, g_sceneDepth);
    glRbStore(GL_RENDERBUFFER_, GL_DEPTH24_STENCIL8_, w, h);
    glBindRb(GL_RENDERBUFFER_, 0);

    gl_BindFramebuffer(GL_FRAMEBUFFER_, g_sceneFbo);
    gl_FramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_,
                            GL_TEXTURE_2D_, g_sceneTex, 0);
    glFbRb(GL_FRAMEBUFFER_, GL_DEPTH_STENCIL_ATTACHMENT_, GL_RENDERBUFFER_, g_sceneDepth);

    unsigned st = gl_CheckFramebufferStatus ? gl_CheckFramebufferStatus(GL_FRAMEBUFFER_)
                                            : GL_FRAMEBUFFER_COMPLETE_;
    gl_BindFramebuffer(GL_FRAMEBUFFER_, 0);

    if (st != GL_FRAMEBUFFER_COMPLETE_) {
        Log("VR: supersample target %dx%d INCOMPLETE (0x%X); "
            "falling back to the back buffer", w, h, st);
        g_sceneFailed = true;
        return false;
    }

    g_sceneW = w; g_sceneH = h;
    g_sceneBaseW = baseW; g_sceneBaseH = baseH;
    Log("VR: scene target %dx%d (%.2fx supersample of %dx%d)",
        w, h, g_superSample, baseW, baseH);
    return true;
}

// Point the engine's rendering at the offscreen target for the coming frame.
bool VR_BindSceneTarget(int baseW, int baseH)
{
    if (!EnsureSceneTarget(baseW, baseH))
        return false;
    gl_BindFramebuffer(0x8D40 /*GL_FRAMEBUFFER*/, g_sceneFbo);
    g_sceneBound = true;
    return true;
}

// Hand rendering back to the window's own buffer so we can read what was drawn.
void VR_UnbindSceneTarget()
{
    if (!g_sceneBound)
        return;
    gl_BindFramebuffer(0x8D40 /*GL_FRAMEBUFFER*/, 0);
    g_sceneBound = false;
}

// Put the frame on the monitor too. Everything now renders offscreen, so
// without this the desktop window is simply black and there is no way to see
// what the game is doing without wearing the headset.
void VR_MirrorToWindow(int winW, int winH)
{
    if (!g_sceneFbo || g_sceneFailed || winW < 2 || winH < 2)
        return;
    if (!LoadGLFunctions())
        return;
    if (!g_blitFbo)
        gl_GenFramebuffers(1, &g_blitFbo);

    const bool hadScissor = gl_IsEnabled && gl_IsEnabled(GL_SCISSOR_TEST_) != 0;
    if (hadScissor && gl_Disable) gl_Disable(GL_SCISSOR_TEST_);

    gl_BindFramebuffer(GL_READ_FRAMEBUFFER_, g_blitFbo);
    gl_FramebufferTexture2D(GL_READ_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_,
                            GL_TEXTURE_2D_, g_sceneTex, 0);
    gl_BindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0);
    gl_BlitFramebuffer(0, 0, g_sceneW, g_sceneH, 0, 0, winW, winH,
                       GL_COLOR_BUFFER_BIT_, GL_LINEAR_);
    gl_BindFramebuffer(GL_READ_FRAMEBUFFER_, 0);

    if (hadScissor && gl_Enable) gl_Enable(GL_SCISSOR_TEST_);
}

bool VR_SceneTargetActive() { return g_sceneFbo != 0 && !g_sceneFailed; }

// Copy the finished frame -- scene plus everything the 2D pass drew over it --
// into this eye's persistent texture. Called once per frame, for whichever eye
// that frame belongs to.
bool VR_CaptureEye(int eye, int baseW, int baseH, bool letterbox)
{
    if (eye < 0 || eye >= EYES || baseW < 2 || baseH < 2)
        return false;
    if (!LoadGLFunctions())
        return false;

    // Hold each eye at the headset's own per-eye resolution, so the detail the
    // supersampled target produced survives all the way to the compositor
    // instead of being thrown away at the game's resolution on the way past.
    // Before a session exists that size is unknown, so fall back to the game's
    // own -- otherwise capture fails silently and the eye stays black.
    const int w = (g_eyeW >= 2) ? (int)g_eyeW : baseW;
    const int h = (g_eyeH >= 2) ? (int)g_eyeH : baseH;
    if (w < 2 || h < 2)
        return false;

    if (!g_eyeTex[eye] || w != g_capW || h != g_capH) {
        if (!g_eyeTex[eye])
            gl_GenTextures(1, &g_eyeTex[eye]);
        gl_BindTexture(GL_TEXTURE_2D_, g_eyeTex[eye]);
        gl_TexImage2D(GL_TEXTURE_2D_, 0, GL_RGBA8_, w, h, 0,
                      GL_RGBA_, GL_UNSIGNED_BYTE_, nullptr);
        if (gl_TexParameteri) {
            gl_TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, GL_LINEAR_);
            gl_TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MAG_FILTER_, GL_LINEAR_);
            gl_TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_S_, GL_CLAMP_TO_EDGE_);
            gl_TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_T_, GL_CLAMP_TO_EDGE_);
        }
        gl_BindTexture(GL_TEXTURE_2D_, 0);
        g_capW = w; g_capH = h;
        g_eyeTexValid[0] = g_eyeTexValid[1] = false;
    }

    if (!g_capFbo)
        gl_GenFramebuffers(1, &g_capFbo);
    if (!g_blitFbo)
        gl_GenFramebuffers(1, &g_blitFbo);

    // The engine leaves the scissor test enabled with its own box, and
    // glBlitFramebuffer IS subject to scissoring -- that silently clipped every
    // copy to the game's rectangle and left the rest of the target untouched.
    const bool hadScissor = gl_IsEnabled && gl_IsEnabled(GL_SCISSOR_TEST_) != 0;
    if (hadScissor && gl_Disable) gl_Disable(GL_SCISSOR_TEST_);

    gl_BindFramebuffer(GL_DRAW_FRAMEBUFFER_, g_capFbo);
    gl_FramebufferTexture2D(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_,
                            GL_TEXTURE_2D_, g_eyeTex[eye], 0);

    // Prefer the supersampled target; only fall back to the window's buffer if
    // it could not be created.
    int srcW = baseW, srcH = baseH;
    if (g_sceneFbo && !g_sceneFailed) {
        gl_BindFramebuffer(GL_READ_FRAMEBUFFER_, g_blitFbo);
        gl_FramebufferTexture2D(GL_READ_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_,
                                GL_TEXTURE_2D_, g_sceneTex, 0);
        srcW = g_sceneW; srcH = g_sceneH;
    } else {
        gl_BindFramebuffer(GL_READ_FRAMEBUFFER_, 0);
        gl_ReadBuffer(GL_BACK_);
    }

    // A 3D scene fills the eye (its frustum is reported at submit time). A flat
    // screen is fitted in the middle at its own aspect, since stretching a 16:9
    // menu across a nearly square eye leaves it distorted and hard to read.
    int sx0 = 0, sy0 = 0, sx1 = srcW, sy1 = srcH;
    int dx0 = 0, dy0 = 0, dx1 = w, dy1 = h;
    if (letterbox && srcW > 0 && srcH > 0) {
        if (gl_ClearColor && gl_Clear) {
            gl_ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            gl_Clear(GL_COLOR_BUFFER_BIT_);
        }

        float srcAspect = (float)srcW / (float)srcH;

        // The 2D layer is already drawn into a correctly-proportioned band of
        // the square buffer, so the rest of that buffer is empty margin. Read
        // only the band -- otherwise the panel gets letterboxed a second time
        // here and ends up needlessly small in the headset.
        if (g_panelAspect > 0.0f && fabsf(srcAspect - g_panelAspect) > 0.01f) {
            float bw = (float)srcW, bh = (float)srcH;
            if (srcAspect > g_panelAspect) bw = bh * g_panelAspect;
            else                           bh = bw / g_panelAspect;
            sx0 = (int)((srcW - bw) * 0.5f);
            sy0 = (int)((srcH - bh) * 0.5f);
            sx1 = sx0 + (int)bw;
            sy1 = sy0 + (int)bh;
            srcAspect = g_panelAspect;
        }

        float ph = h * 0.70f;
        float pw = ph * srcAspect;
        if (pw > w * 0.95f) { pw = w * 0.95f; ph = pw / srcAspect; }
        dx0 = (int)((w - pw) * 0.5f);
        dy0 = (int)((h - ph) * 0.5f);
        dx1 = dx0 + (int)pw;
        dy1 = dy0 + (int)ph;
    }

    gl_BlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1,
                       GL_COLOR_BUFFER_BIT_, GL_LINEAR_);
    gl_BindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0);
    gl_BindFramebuffer(GL_READ_FRAMEBUFFER_, 0);

    if (hadScissor && gl_Enable) gl_Enable(GL_SCISSOR_TEST_);

    g_eyeTexValid[eye] = true;

    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("VR: per-eye capture %dx%d -> texture (scissor was %s)",
            w, h, hadScissor ? "ON, temporarily disabled" : "off");
    }
    return true;
}

// dstEye selects the swapchain written to; srcEye selects the captured texture
// read from. They differ only while one eye is still waiting for its first
// capture, so that early frames are not half black.
static bool BlitToEye(int dstEye, int srcEye)
{
    if (!LoadGLFunctions())
        return false;
    if (!g_eyeTex[srcEye])
        return false;
    const int eye = dstEye;

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai = {};
    ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
    if (XR_FAILED(xrAcquireSwapchainImage(g_swapchain[eye], &ai, &index)))
        return false;

    XrSwapchainImageWaitInfo wi = {};
    wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(g_swapchain[eye], &wi)))
        return false;

    if (!g_blitFbo)
        gl_GenFramebuffers(1, &g_blitFbo);
    if (!g_capFbo)
        gl_GenFramebuffers(1, &g_capFbo);

    // Scissoring applies to blits, and the engine leaves its own box set.
    // Without this the copy is clipped to the game's rectangle and the rest of
    // the (much larger) eye image is never written -- which is precisely why
    // the top of the view was missing.
    const bool hadScissor = gl_IsEnabled && gl_IsEnabled(GL_SCISSOR_TEST_) != 0;
    if (hadScissor && gl_Disable) gl_Disable(GL_SCISSOR_TEST_);

    gl_BindFramebuffer(GL_DRAW_FRAMEBUFFER_, g_blitFbo);
    gl_FramebufferTexture2D(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_,
                            GL_TEXTURE_2D_, g_images[eye][index].image, 0);
    // Source is this eye's captured frame, not the live back buffer: the back
    // buffer only ever holds the most recent eye.
    gl_BindFramebuffer(GL_READ_FRAMEBUFFER_, g_capFbo);
    gl_FramebufferTexture2D(GL_READ_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_,
                            GL_TEXTURE_2D_, g_eyeTex[srcEye], 0);

    // The eye texture is ALREADY the swapchain's size and already composed --
    // scaling and any letterboxing happened during capture -- so this is a
    // whole-texture 1:1 copy. It previously still used the game's back-buffer
    // rectangle as the source, which read a 1440x1431 corner out of a 3072x3264
    // texture and stretched it over the whole eye: the "way too zoomed in"
    // view, and the menu reduced to a fragment in a corner.
    static bool firstBlit = true;
    if (firstBlit && gl_CheckFramebufferStatus) {
        unsigned st = gl_CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER_);
        Log("VR: first blit - dst FBO status 0x%X (%s), src 0,0..%d,%d -> %ux%u",
            st, st == GL_FRAMEBUFFER_COMPLETE_ ? "COMPLETE" : "INCOMPLETE",
            g_capW, g_capH, g_eyeW, g_eyeH);
    }

    gl_BlitFramebuffer(0, 0, g_capW, g_capH,
                       0, 0, (int)g_eyeW, (int)g_eyeH,
                       GL_COLOR_BUFFER_BIT_, GL_LINEAR_);

    if (firstBlit) {
        firstBlit = false;
        if (gl_GetError) {
            unsigned e = gl_GetError();
            if (e) Log("VR: first blit raised GL error 0x%X", e);
            else   Log("VR: first blit completed with no GL error");
        }
    }

    // Leave the engine's state as we found it: it renders to the default
    // framebuffer with its own scissor box, and would break if we left either
    // of them changed.
    gl_BindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0);
    gl_BindFramebuffer(GL_READ_FRAMEBUFFER_, 0);
    if (hadScissor && gl_Enable) gl_Enable(GL_SCISSOR_TEST_);

    XrSwapchainImageReleaseInfo ri = {};
    ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
    xrReleaseSwapchainImage(g_swapchain[eye], &ri);
    return true;
}

void VR_SubmitFrame(int fbWidth, int fbHeight, bool stereo, int renderedEye)
{
    if (renderedEye < 0 || renderedEye >= EYES)
        renderedEye = 0;
    if (!g_frameBegun)
        return;

    if (fbWidth < 2 || fbHeight < 2) {
        // Nothing sensible to copy. Still end the frame, or the runtime stalls
        // waiting on a frame that never arrives.
        static bool moaned = false;
        if (!moaned) { moaned = true; Log("VR: back buffer %dx%d - nothing to submit", fbWidth, fbHeight); }
        XrFrameEndInfo fei = {};
        fei.type = XR_TYPE_FRAME_END_INFO;
        fei.displayTime = g_frameState.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        xrEndFrame(g_session, &fei);
        g_frameBegun = false;
        return;
    }

    XrCompositionLayerProjectionView views[EYES] = {};
    XrCompositionLayerProjection layer = {};
    const XrCompositionLayerBaseHeader* layers[1];
    uint32_t layerCount = 0;

    if (g_everLocated) {
        // Pin the panel where the player was looking when it appeared, and
        // drop the pin as soon as real 3D resumes so the next panel re-pins.
        if (stereo) {
            g_panelPinned = false;
        } else if (!g_panelPinned) {
            g_panelPinned = true;
            for (int e = 0; e < EYES; ++e)
                g_panelPose[e] = g_views[e].pose;
        }

        // Capture the finished frame into the eye it was rendered for. A 2D
        // screen belongs to neither eye, so it goes to both.
        if (stereo) {
            VR_CaptureEye(renderedEye, fbWidth, fbHeight, false);
            // Tunnel the periphery while the player is moving or turning. Done
            // after capture so it lands on the finished frame, and only on 3D:
            // narrowing a flat menu would be pointless and annoying.
            Comfort_DrawVignette(renderedEye);
            g_eyePose[renderedEye] = g_views[renderedEye].pose;
            g_eyePoseValid[renderedEye] = true;
        } else {
            for (int e = 0; e < EYES; ++e) {
                VR_CaptureEye(e, fbWidth, fbHeight, true);
                g_eyePose[e] = g_views[e].pose;
                g_eyePoseValid[e] = true;
            }
        }

        for (int eye = 0; eye < EYES; ++eye) {
            // Until an eye has been captured even once, show it the eye we do
            // have, so the first frames are not half black.
            const int src = g_eyeTexValid[eye] ? eye : renderedEye;
            if (!g_eyeTexValid[src])
                continue;
            BlitToEye(eye, src);

            // Report exactly the frustum VR_GetEyeCamera handed the engine.
            const XrFovf& f = g_views[eye].fov;
            float halfX = g_submitHalfX[eye], halfY = g_submitHalfY[eye];
            // A 2D panel is fitted to the eye image, not rendered through the
            // game's frustum, so it must be reported with the headset's own
            // field of view. Reusing the angles left over from the last 3D
            // scene would crop the panel by whatever that frustum overshot.
            if (!stereo || halfX <= 0.0f || halfY <= 0.0f) {
                halfX = fabsf(f.angleLeft) > fabsf(f.angleRight)
                      ? fabsf(f.angleLeft) : fabsf(f.angleRight);
                halfY = fabsf(f.angleUp) > fabsf(f.angleDown)
                      ? fabsf(f.angleUp) : fabsf(f.angleDown);
            }

            views[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            // A pinned panel keeps the pose it appeared at. A 3D eye reports
            // the pose its image was rendered with -- which for the eye that
            // did not render this frame is a frame old, and saying so is what
            // lets the compositor reproject it correctly.
            views[eye].pose = !stereo            ? g_panelPose[eye]
                            : g_eyePoseValid[src] ? g_eyePose[src]
                                                  : g_views[eye].pose;
            // Report the symmetric fov we actually rendered, not the runtime's
            // asymmetric suggestion, so the compositor reprojects correctly.
            views[eye].fov.angleLeft  = -halfX;
            views[eye].fov.angleRight =  halfX;
            views[eye].fov.angleUp    =  halfY;
            views[eye].fov.angleDown  = -halfY;
            views[eye].subImage.swapchain = g_swapchain[eye];
            views[eye].subImage.imageArrayIndex = 0;

            // Map the runtime's true asymmetric frustum onto the symmetric
            // superset we rendered, and submit just that window of the image
            // with the true angles. Without this the symmetric fov is taken as
            // gospel and the picture sits off-centre on the panel -- which is
            // what cut the top of the view off.
            XrRect2Di rect = { { 0, 0 }, { (int32_t)g_eyeW, (int32_t)g_eyeH } };
            if (stereo && halfX > 0.0f && halfY > 0.0f) {
                const float tsx = tanf(halfX), tsy = tanf(halfY);
                const float u0 = (tanf(f.angleLeft)  + tsx) / (2.0f * tsx);
                const float u1 = (tanf(f.angleRight) + tsx) / (2.0f * tsx);
                const float v0 = (tanf(f.angleDown)  + tsy) / (2.0f * tsy);
                const float v1 = (tanf(f.angleUp)    + tsy) / (2.0f * tsy);

                int x0 = (int)(u0 * g_eyeW + 0.5f), x1 = (int)(u1 * g_eyeW + 0.5f);
                int y0 = (int)(v0 * g_eyeH + 0.5f), y1 = (int)(v1 * g_eyeH + 0.5f);
                if (x0 < 0) x0 = 0;  if (x1 > (int)g_eyeW) x1 = (int)g_eyeW;
                if (y0 < 0) y0 = 0;  if (y1 > (int)g_eyeH) y1 = (int)g_eyeH;

                if (x1 - x0 >= 16 && y1 - y0 >= 16) {
                    rect.offset.x = x0;
                    rect.offset.y = y0;
                    rect.extent.width  = x1 - x0;
                    rect.extent.height = y1 - y0;
                    // The sub-rect IS the true frustum, so report it as such.
                    views[eye].fov = f;
                }
            }
            views[eye].subImage.imageRect = rect;

            static bool fovLogged = false;
            if (!fovLogged && stereo && eye == 1) {
                fovLogged = true;
                Log("VR: runtime fov L%.2f R%.2f U%.2f D%.2f deg",
                    f.angleLeft  * 57.2957795f, f.angleRight * 57.2957795f,
                    f.angleUp    * 57.2957795f, f.angleDown  * 57.2957795f);
                Log("VR: rendered symmetric +-%.2f x +-%.2f deg, submitting rect "
                    "%d,%d %dx%d of %ux%u",
                    halfX * 57.2957795f, halfY * 57.2957795f,
                    rect.offset.x, rect.offset.y,
                    rect.extent.width, rect.extent.height, g_eyeW, g_eyeH);
            }
        }

        layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
        layer.space = g_space;
        layer.viewCount = EYES;
        layer.views = views;
        layers[0] = (const XrCompositionLayerBaseHeader*)&layer;
        layerCount = 1;
    }

    XrFrameEndInfo fei = {};
    fei.type = XR_TYPE_FRAME_END_INFO;
    fei.displayTime = g_frameState.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = layerCount;
    fei.layers = layerCount ? layers : nullptr;
    xrEndFrame(g_session, &fei);

    g_frameBegun = false;
}

void VR_Shutdown()
{
    for (int i = 0; i < EYES; ++i) {
        if (g_swapchain[i] != XR_NULL_HANDLE) xrDestroySwapchain(g_swapchain[i]);
        delete[] g_images[i];
        g_images[i] = nullptr;
    }
    if (g_space   != XR_NULL_HANDLE) xrDestroySpace(g_space);
    if (g_session != XR_NULL_HANDLE) xrDestroySession(g_session);
    if (g_instance != XR_NULL_HANDLE) xrDestroyInstance(g_instance);
    g_space = XR_NULL_HANDLE;
    g_session = XR_NULL_HANDLE;
    g_instance = XR_NULL_HANDLE;
}






```


---

## `src/dllmain.cpp`  (1896 lines)

Proxy entry, cgame syscall hook, GL interception (viewport/scissor/drawbuffer/readbuffer/wglGetProcAddress), frame orchestration, config, self-test.

```cpp
﻿// COD VR - camera control probe.
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
extern "C" PROC WINAPI i_wglGetProcAddress(LPCSTR name)
{
    typedef PROC (WINAPI *Fn)(LPCSTR);
    if (name && !strcmp(name, "wglSwapIntervalEXT"))
        return (PROC)&i_wglSwapIntervalEXT;
    return ((Fn)p_wglGetProcAddress)(name);
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

    if (fopen_s(&g_log, path, "w") != 0 || !g_log) {
        char tmp[MAX_PATH];
        if (GetTempPathA(MAX_PATH, tmp)) {
            strcat_s(tmp, "codvr_result.log");
            if (fopen_s(&g_log, tmp, "w") != 0)
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
enum { REPLAY_MAX = 2048, REPLAY_SNAP = 256, ENT_AXIS_OFF = 12, REPLAY_ARGS = 8 };

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
static int  g_vrEye;            // which eye this frame belongs to (alternates)

// The 2D pass (HUD, compass, ammo, subtitles) runs entirely after the render
// syscall returns, so everything drawn between there and the buffer swap is
// flat overlay. Both eyes otherwise receive byte-identical HUD pixels at
// byte-identical positions, which is zero parallax -- the HUD reads as
// infinitely far away while the world sits a few metres off, and holding focus
// on it is uncomfortable. Nudging the overlay outward for one eye and inward
// for the other gives it convergence, placing it at arm's length.
// Starts TRUE: a frame is 2D until proven otherwise. Menus, briefings and
// loading screens never issue a render call at all, so keying this off "after
// the render call" alone left them stretched onto the square buffer with no
// aspect correction. The render hook clears it around the 3D call instead.
static bool g_hudPhase = true;
static int  g_hudShiftPx = 12;  // codvr.cfg "hudshift"; 0 leaves it at infinity
LONG g_viewW, g_viewH;          // the engine's own render size

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
        static bool flatGeomLogged = false;
        if (!flatGeomLogged) {
            flatGeomLogged = true;
            Log("");
            Log("======= SCENE GEOMETRY (first scene) =======");
            LogSceneGeometry(iv, fv, "game's own");
            Log("===========================================");
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

        // ONE eye per frame, across the whole back buffer.
        //
        // Rendering both eyes in a single frame required replaying the scene
        // for the second one, because the engine hands its entity list to the
        // first render and empties it -- and reconstructing that replay from
        // outside the engine kept failing (and once crashed the game). This
        // alternates instead: every frame renders a complete, ordinary scene
        // that the engine builds for itself, so the weapon and soldiers are
        // simply always there. It also doubles the width each eye gets and
        // gives every eye a whole 2D/HUD pass rather than half of one.
        const int eye = g_vrEye;
        float org[3], axis[9], fovX, fovY;
        VR_GetEyeCamera(eye, gameOrigin, gameAxis, sw, sh, org, axis, &fovX, &fovY);

        fv[RD_FOVX] = fovX;
        fv[RD_FOVY] = fovY;
        fv[RD_ORG] = org[0]; fv[RD_ORG+1] = org[1]; fv[RD_ORG+2] = org[2];
        memcpy((BYTE*)p + RD_AXIS_BYTES, axis, sizeof(axis));

        // The 3D scene is the one thing that must NOT be reshaped: its aspect
        // is already handled by the field of view we wrote into the refdef.
        g_hudPhase = false;
        int rv = g_realSyscall(num, p, a2, a3, a4, a5, a6, a7);

        // Everything from here to the buffer swap is flat overlay again.
        g_hudPhase = true;

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
        jmp  dword ptr [g_realSyscall]
    divert:
        // Tail-jump: SplitRenderAndForward is cdecl and inherits this exact
        // frame, so [esp+4]=number and [esp+8]=refdef land where expected.
        jmp  SplitRenderAndForward
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
    const float offX = (sw - bw) * 0.5f, offY = (sh - bh) * 0.5f;
    const float scaleX = bw / (float)sw, scaleY = bh / (float)sh;

    *x = (int)(offX + *x * scaleX + 0.5f);
    *y = (int)(offY + *y * scaleY + 0.5f);
    *w = (int)(*w * scaleX + 0.5f);
    *h = (int)(*h * scaleY + 0.5f);
}

// Per-eye horizontal nudge for the flat 2D overlay. The shift is in the game's
// own pixels; the supersample scale is applied after it.
static int HudShiftForEye()
{
    if (!g_hudPhase || g_hudShiftPx == 0 || !VR_Active())
        return 0;
    // Only meaningful over a 3D scene. A menu frame is copied to both eyes
    // identically, so a shift there would just move the panel off-centre
    // without giving it any depth.
    if (!g_vrDrewStereo)
        return 0;
    // Left eye sees a near object right of centre, right eye left of centre.
    return (g_vrEye == 0) ? g_hudShiftPx : -g_hudShiftPx;
}

// Both of these take the engine's own screen coordinates and put them where
// they belong in the supersampled target: reshaped to the intended aspect and
// nudged for eye depth during the 2D pass, then scaled up.
static void MapRect(int* x, int* y, int* w, int* h, bool hudPass)
{
    if (hudPass) {
        HudRemap(x, y, w, h);
        *x += HudShiftForEye();
    }
    const float s = VR_Supersample();
    if (s > 1.0f) {
        *x = (int)(*x * s + 0.5f); *y = (int)(*y * s + 0.5f);
        *w = (int)(*w * s + 0.5f); *h = (int)(*h * s + 0.5f);
    }
}

extern "C" void WINAPI i_glViewport(int x, int y, int w, int h)
{
    typedef void (WINAPI *Fn)(int,int,int,int);
    MapRect(&x, &y, &w, &h, g_hudPhase);
    ((Fn)p_glViewport)(x, y, w, h);
}

extern "C" void WINAPI i_glScissor(int x, int y, int w, int h)
{
    typedef void (WINAPI *Fn)(int,int,int,int);
    // The scissor has to follow the viewport through the same remap, or the 2D
    // pass would be clipped against a rectangle that no longer matches where it
    // is being drawn.
    MapRect(&x, &y, &w, &h, g_hudPhase);
    ((Fn)p_glScissor)(x, y, w, h);
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

extern "C" HGLRC WINAPI i_wglCreateContext(HDC hdc)
{
    typedef HGLRC (WINAPI *Fn)(HDC);
    return ((Fn)p_wglCreateContext)(hdc);
}

static HDC   g_curDC;
static HGLRC g_curRC;

extern "C" BOOL WINAPI i_wglMakeCurrent(HDC hdc, HGLRC rc)
{
    typedef BOOL (WINAPI *Fn)(HDC, HGLRC);
    BOOL ok = ((Fn)p_wglMakeCurrent)(hdc, rc);
    if (ok && hdc && rc) {
        g_curDC = hdc;
        g_curRC = rc;
    }
    return ok;
}

extern "C" BOOL WINAPI i_wglDeleteContext(HGLRC rc)
{
    typedef BOOL (WINAPI *Fn)(HGLRC);
    return ((Fn)p_wglDeleteContext)(rc);
}

extern "C" BOOL WINAPI i_wglSwapBuffers(HDC hdc)
{
    InterlockedIncrement(&g_frame);
    TryHookCgame();

    if (!Probe_Enabled()) {
        // Startup retries internally until the headset wakes, so putting the
        // Quest on after launching flat still brings VR up.
        VR_Startup(g_curDC ? g_curDC : hdc, g_curRC);

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
        // Hand the next frame to the other eye. Only advance once a 3D scene
        // actually rendered, so sitting in a menu does not leave one eye
        // permanently stale.
        if (g_vrDrewStereo)
            g_vrEye ^= 1;
        g_vrDrewStereo = false;
        g_hudPhase = true;          // a frame is 2D until a scene render says otherwise
        // Backstop for the per-scene reset in the render hook: if no scene was
        // rendered at all this frame (menu, loading screen), clear whatever was
        // captured so it cannot leak into the next frame's first scene.
        InterlockedExchange(&g_replayCount, 0);
        // Comfort: keys first (recenter, snap turn), then advance the smoothed
        // vignette so the next frame's overlay reflects current motion.
        Comfort_PollInput();
        Comfort_Update();
        Comfort_NoteFrame();

        // Acquire poses for the frame the engine is about to build. Doing it
        // here means the render hook already has fresh views when it runs.
        VR_BeginFrame();

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
static void LoadConfig()
{
    char path[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                       | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&LoadConfig, &self);
    if (!GetModuleFileNameA(self, path, MAX_PATH))
        return;
    char* slash = strrchr(path, '\\');
    if (!slash)
        return;
    strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "codvr.cfg");

    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) {
        Log("config: none at %s (using defaults)", path);
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

        if (!_stricmp(key, "recenterKey")) {
            const int vk = Comfort_ParseKey(text);
            Comfort_SetRecenterKey(vk);
            Log("config: recenter key = %s%s", text, vk ? "" : " (UNRECOGNISED, disabled)");
            continue;
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
            continue;
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
    }
    fclose(f);
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

    // Against the real module: locate the syscall slot the same way we will
    // at runtime, and check it lands inside cgamex86.dll's .data.
    HMODULE cg = LoadLibraryA(
        "H:\\SteamLibrary\\steamapps\\common\\Call of Duty\\cgamex86.dll");
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
                    // Derived from the live factor, not hardcoded: the value
                    // comes from codvr.cfg and changes with the tuning.
                    const float s = VR_Supersample();
                    const int wantW = (int)(100 * s + 0.5f);
                    const int wantH = (int)(50  * s + 0.5f);
                    int vp[4] = {0,0,0,0};
                    i_glViewport(0, 0, 100, 50);
                    getInt(0x0BA2 /*GL_VIEWPORT*/, vp);
                    CHECK(vp[2] == wantW && vp[3] == wantH,
                          "GL: viewport is scaled by the supersample factor");
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

        char flag[8];
        if (GetEnvironmentVariableA("CODVR_SELFTEST", flag, sizeof(flag)) > 0)
            SelfTest();
    }
    else if (reason == DLL_PROCESS_DETACH) {
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







```


---

## `src/comfort.cpp`  (531 lines)

Vignette, recenter, snap turn, frame-rate watch.

```cpp
// Comfort and input: the features that decide whether a VR mod is pleasant to
// play for an hour or makes you take the headset off after ten minutes.
//
// Three things live here:
//
//  * A TUNNELLING VIGNETTE. Simulator sickness comes from vection -- seeing
//    motion your inner ear cannot feel. Narrowing the field of view while the
//    player is moving or turning removes the peripheral optical flow that
//    drives it, and widening it back when they stop keeps the restriction from
//    being noticeable. This is the single most effective comfort option in VR
//    and every serious mod has one.
//
//  * RECENTER. Seated tracking drifts, and people start a session facing the
//    wrong way. Without a key for this the only fix is restarting the game.
//
//  * SNAP TURN. Rotating in discrete steps rather than sweeping past the whole
//    world is far gentler for people prone to sickness.
//
// The vignette is drawn with fixed-function OpenGL, which is all this engine's
// context guarantees -- no shaders anywhere.

#include "codvr.h"
#include "vr.h"

#include <math.h>
#include <stdio.h>

// ------------------------------------------------------------ GL plumbing

typedef void (WINAPI *PFN_GenTextures)(int, unsigned*);
typedef void (WINAPI *PFN_BindTexture)(unsigned, unsigned);
typedef void (WINAPI *PFN_TexImage2D)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*);
typedef void (WINAPI *PFN_TexParameteri)(unsigned,unsigned,int);
typedef void (WINAPI *PFN_Enable)(unsigned);
typedef void (WINAPI *PFN_Disable)(unsigned);
typedef unsigned char (WINAPI *PFN_IsEnabled)(unsigned);
typedef void (WINAPI *PFN_BlendFunc)(unsigned,unsigned);
typedef void (WINAPI *PFN_MatrixMode)(unsigned);
typedef void (WINAPI *PFN_PushMatrix)(void);
typedef void (WINAPI *PFN_PopMatrix)(void);
typedef void (WINAPI *PFN_LoadIdentity)(void);
typedef void (WINAPI *PFN_Ortho)(double,double,double,double,double,double);
typedef void (WINAPI *PFN_Begin)(unsigned);
typedef void (WINAPI *PFN_End)(void);
typedef void (WINAPI *PFN_Color4f)(float,float,float,float);
typedef void (WINAPI *PFN_TexCoord2f)(float,float);
typedef void (WINAPI *PFN_Vertex2f)(float,float);
typedef void (WINAPI *PFN_Viewport)(int,int,int,int);
typedef void (WINAPI *PFN_GenFramebuffers)(int, unsigned*);
typedef void (WINAPI *PFN_BindFramebuffer)(unsigned, unsigned);
typedef void (WINAPI *PFN_FramebufferTexture2D)(unsigned,unsigned,unsigned,unsigned,int);
typedef void (WINAPI *PFN_GetIntegerv)(unsigned, int*);
typedef void (WINAPI *PFN_ShadeModel)(unsigned);

static PFN_GenTextures          gl_GenTextures;
static PFN_BindTexture          gl_BindTexture;
static PFN_TexImage2D           gl_TexImage2D;
static PFN_TexParameteri        gl_TexParameteri;
static PFN_Enable               gl_Enable;
static PFN_Disable              gl_Disable;
static PFN_IsEnabled            gl_IsEnabled;
static PFN_BlendFunc            gl_BlendFunc;
static PFN_MatrixMode           gl_MatrixMode;
static PFN_PushMatrix           gl_PushMatrix;
static PFN_PopMatrix            gl_PopMatrix;
static PFN_LoadIdentity         gl_LoadIdentity;
static PFN_Ortho                gl_Ortho;
static PFN_Begin                gl_Begin;
static PFN_End                  gl_End;
static PFN_Color4f              gl_Color4f;
static PFN_TexCoord2f           gl_TexCoord2f;
static PFN_Vertex2f             gl_Vertex2f;
static PFN_Viewport             gl_Viewport;
static PFN_GenFramebuffers      gl_GenFramebuffers;
static PFN_BindFramebuffer      gl_BindFramebuffer;
static PFN_FramebufferTexture2D gl_FramebufferTexture2D;
static PFN_GetIntegerv          gl_GetIntegerv;
static PFN_ShadeModel           gl_ShadeModel;

enum {
    GL_TEXTURE_2D_        = 0x0DE1,
    GL_BLEND_             = 0x0BE2,
    GL_SRC_ALPHA_         = 0x0302,
    GL_ONE_MINUS_SRC_ALPHA_ = 0x0303,
    GL_DEPTH_TEST_        = 0x0B71,
    GL_SCISSOR_TEST_      = 0x0C11,
    GL_CULL_FACE_         = 0x0B44,
    GL_LIGHTING_          = 0x0B50,
    GL_PROJECTION_        = 0x1701,
    GL_MODELVIEW_         = 0x1700,
    GL_QUADS_             = 0x0007,
    GL_RGBA_              = 0x1908,
    GL_RGBA8_             = 0x8058,
    GL_UNSIGNED_BYTE_     = 0x1401,
    GL_LINEAR_            = 0x2601,
    GL_CLAMP_TO_EDGE_     = 0x812F,
    GL_TEXTURE_MIN_FILTER_= 0x2801,
    GL_TEXTURE_MAG_FILTER_= 0x2800,
    GL_TEXTURE_WRAP_S_    = 0x2802,
    GL_TEXTURE_WRAP_T_    = 0x2803,
    GL_FRAMEBUFFER_       = 0x8D40,
    GL_COLOR_ATTACHMENT0_ = 0x8CE0,
    GL_VIEWPORT_          = 0x0BA2,
    GL_TEXTURE_BINDING_2D_= 0x8069,
    GL_QUAD_STRIP_        = 0x0008,
    GL_BLEND_SRC_         = 0x0BE1,
    GL_BLEND_DST_         = 0x0BE0,
    GL_SHADE_MODEL_       = 0x0B54,
    GL_SMOOTH_            = 0x1D01
};

static bool g_glReady;
static bool g_glTried;

static bool LoadGL()
{
    if (g_glTried)
        return g_glReady;
    g_glTried = true;

    #define GETP(v, n) v = (decltype(v))CODVR_GetGLProc(n)
    GETP(gl_GenTextures,          "glGenTextures");
    GETP(gl_BindTexture,          "glBindTexture");
    GETP(gl_TexImage2D,           "glTexImage2D");
    GETP(gl_TexParameteri,        "glTexParameteri");
    GETP(gl_Enable,               "glEnable");
    GETP(gl_Disable,              "glDisable");
    GETP(gl_IsEnabled,            "glIsEnabled");
    GETP(gl_BlendFunc,            "glBlendFunc");
    GETP(gl_MatrixMode,           "glMatrixMode");
    GETP(gl_PushMatrix,           "glPushMatrix");
    GETP(gl_PopMatrix,            "glPopMatrix");
    GETP(gl_LoadIdentity,         "glLoadIdentity");
    GETP(gl_Ortho,                "glOrtho");
    GETP(gl_Begin,                "glBegin");
    GETP(gl_End,                  "glEnd");
    GETP(gl_Color4f,              "glColor4f");
    GETP(gl_TexCoord2f,           "glTexCoord2f");
    GETP(gl_Vertex2f,             "glVertex2f");
    GETP(gl_Viewport,             "glViewport");
    GETP(gl_GenFramebuffers,      "glGenFramebuffers");
    GETP(gl_BindFramebuffer,      "glBindFramebuffer");
    GETP(gl_FramebufferTexture2D, "glFramebufferTexture2D");
    GETP(gl_GetIntegerv,          "glGetIntegerv");
    GETP(gl_ShadeModel,           "glShadeModel");
    #undef GETP

    g_glReady = gl_GenTextures && gl_BindTexture && gl_TexImage2D && gl_Begin
             && gl_End && gl_Vertex2f && gl_TexCoord2f && gl_Color4f
             && gl_BindFramebuffer && gl_FramebufferTexture2D && gl_Ortho;
    if (!g_glReady)
        Log("comfort: fixed-function GL unavailable; vignette disabled");
    return g_glReady;
}

// ------------------------------------------------------------- settings

static bool  g_vignetteOn      = true;
static float g_vignetteStrength = 1.0f;   // 0..1 scale on how far it closes
static float g_speedThreshold   = 40.0f;  // units/sec where it starts closing
static float g_turnThreshold    = 45.0f;  // deg/sec where it starts closing
static int   g_recenterKey      = VK_F9;
static int   g_snapTurnDeg      = 0;      // 0 disables snap turn
static int   g_snapLeftKey      = 0;
static int   g_snapRightKey     = 0;
static float g_snapMouseScale   = 4.0f;   // mouse counts per degree

void Comfort_SetVignette(bool on)          { g_vignetteOn = on; }
void Comfort_SetVignetteStrength(float s)  { if (s >= 0.0f && s <= 1.0f) g_vignetteStrength = s; }
void Comfort_SetSpeedThreshold(float u)    { if (u > 0.0f) g_speedThreshold = u; }
void Comfort_SetTurnThreshold(float d)     { if (d > 0.0f) g_turnThreshold = d; }
void Comfort_SetRecenterKey(int vk)        { g_recenterKey = vk; }
void Comfort_SetSnapTurn(int deg)          { g_snapTurnDeg = deg; }
void Comfort_SetSnapKeys(int l, int r)     { g_snapLeftKey = l; g_snapRightKey = r; }
void Comfort_SetSnapMouseScale(float s)    { if (s > 0.0f) g_snapMouseScale = s; }

// ----------------------------------------------------------- the aperture
//
// The tunnel is drawn as geometry rather than a fixed texture, because what
// should change with speed is the APERTURE RADIUS, not the opacity. Fading a
// fixed-size mask in and out just greys the periphery; shrinking the opening is
// what actually removes the peripheral motion that causes sickness, and it is
// what the research on field-of-view restrictors describes.
//
// Published restrictors close to roughly 0.6 of the screen radius at full
// motion with a soft inner edge, so that is what the fully-closed end maps to.
enum { VIG_SEGMENTS = 96 };

static unsigned g_vigFbo;

// Aperture for the current amount: 0 = wide open (entirely off screen), 1 =
// fully closed to the published tunnel size.
static void ApertureRadii(float amount, float* inner, float* outer)
{
    // Wide open sits past the corner (1.414 in normalised units) so nothing is
    // visible at all; closed matches the 0.25 / 0.65 tunnel from the literature.
    const float in0 = 1.75f, in1 = 0.25f;
    const float out0 = 2.20f, out1 = 0.65f;
    *inner = in0 + (in1 - in0) * amount;
    *outer = out0 + (out1 - out0) * amount;
}

// ------------------------------------------------------------ the amount

static float g_vigCurrent;   // smoothed 0..1

// How closed the tunnel should be right now, from the player's virtual motion.
static float TargetVignette()
{
    if (!g_vignetteOn || g_vignetteStrength <= 0.0f)
        return 0.0f;

    float speed = 0.0f, turn = 0.0f;
    VR_GetPlayerMotion(&speed, &turn);

    const float bySpeed = speed / (g_speedThreshold * 4.0f);
    const float byTurn  = turn  / (g_turnThreshold  * 4.0f);
    float t = bySpeed > byTurn ? bySpeed : byTurn;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * g_vignetteStrength;
}

float Comfort_VignetteAmount() { return g_vigCurrent; }

// Advance the smoothed amount. Opening is deliberately slower than closing:
// snapping open the instant the player stops is itself jarring.
void Comfort_Update()
{
    const float target = TargetVignette();
    const float rate = (target > g_vigCurrent) ? 0.20f : 0.06f;
    g_vigCurrent += (target - g_vigCurrent) * rate;
    if (g_vigCurrent < 0.001f) g_vigCurrent = 0.0f;
}

// ------------------------------------------------------------- rendering

// Draw the tunnel over a finished eye image. Every piece of GL state touched is
// saved and restored, because the engine owns this context and will keep
// drawing with it the moment we hand it back.
bool Comfort_DrawVignette(int eye)
{
    if (g_vigCurrent <= 0.002f)
        return true;                      // nothing to draw, not a failure
    if (!LoadGL())
        return false;

    const unsigned tex = VR_EyeTexture(eye);
    if (!tex)
        return false;
    int w = 0, h = 0;
    VR_EyeTextureSize(&w, &h);
    if (w < 2 || h < 2)
        return false;

    if (!g_vigFbo)
        gl_GenFramebuffers(1, &g_vigFbo);

    // --- save state ---
    int oldViewport[4] = {0,0,0,0};
    if (gl_GetIntegerv) gl_GetIntegerv(GL_VIEWPORT_, oldViewport);
    int oldTex = 0;
    if (gl_GetIntegerv) gl_GetIntegerv(GL_TEXTURE_BINDING_2D_, &oldTex);
    const bool hadBlend   = gl_IsEnabled && gl_IsEnabled(GL_BLEND_) != 0;
    const bool hadDepth   = gl_IsEnabled && gl_IsEnabled(GL_DEPTH_TEST_) != 0;
    const bool hadScissor = gl_IsEnabled && gl_IsEnabled(GL_SCISSOR_TEST_) != 0;
    const bool hadCull    = gl_IsEnabled && gl_IsEnabled(GL_CULL_FACE_) != 0;
    const bool hadLight   = gl_IsEnabled && gl_IsEnabled(GL_LIGHTING_) != 0;
    const bool hadTex2D   = gl_IsEnabled && gl_IsEnabled(GL_TEXTURE_2D_) != 0;
    // The blend FUNCTION matters as much as whether blending is on. The engine
    // sets its own for particles and decals, and leaving ours behind would
    // quietly corrupt the next thing it draws.
    int oldBlendSrc = GL_SRC_ALPHA_, oldBlendDst = GL_ONE_MINUS_SRC_ALPHA_;
    if (gl_GetIntegerv) {
        gl_GetIntegerv(GL_BLEND_SRC_, &oldBlendSrc);
        gl_GetIntegerv(GL_BLEND_DST_, &oldBlendDst);
    }
    // The soft edge is per-vertex alpha interpolated across the ring, so it
    // needs smooth shading. The engine sets flat shading in places, which would
    // turn the gradient into a hard black ring.
    int oldShade = GL_SMOOTH_;
    if (gl_GetIntegerv) gl_GetIntegerv(GL_SHADE_MODEL_, &oldShade);
    if (gl_ShadeModel)  gl_ShadeModel(GL_SMOOTH_);

    gl_BindFramebuffer(GL_FRAMEBUFFER_, g_vigFbo);
    gl_FramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_,
                            GL_TEXTURE_2D_, tex, 0);
    gl_Viewport(0, 0, w, h);

    if (hadScissor) gl_Disable(GL_SCISSOR_TEST_);
    if (hadDepth)   gl_Disable(GL_DEPTH_TEST_);
    if (hadCull)    gl_Disable(GL_CULL_FACE_);
    if (hadLight)   gl_Disable(GL_LIGHTING_);
    gl_Enable(GL_TEXTURE_2D_);
    gl_Enable(GL_BLEND_);
    gl_BlendFunc(GL_SRC_ALPHA_, GL_ONE_MINUS_SRC_ALPHA_);

    gl_MatrixMode(GL_PROJECTION_);
    gl_PushMatrix();
    gl_LoadIdentity();
    gl_Ortho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);   // centred, so radii are direct
    gl_MatrixMode(GL_MODELVIEW_);
    gl_PushMatrix();
    gl_LoadIdentity();

    // Draw the tunnel as two concentric rings in normalised [-1,1] space:
    // a soft edge from the clear aperture out to full black, then solid black
    // from there past the corners. No texture is involved, so nothing depends
    // on the texture environment the engine happened to leave set.
    float rIn = 0.0f, rOut = 0.0f;
    ApertureRadii(g_vigCurrent, &rIn, &rOut);
    const float rFar = 2.4f;              // safely past the corner at 1.414

    // The eye is not perfectly square, so scale the circle to stay circular.
    float ax = 1.0f, ay = 1.0f;
    if (w > h) ax = (float)w / (float)h;
    else       ay = (float)h / (float)w;

    gl_BindTexture(GL_TEXTURE_2D_, 0);
    gl_Disable(GL_TEXTURE_2D_);

    // Soft edge: transparent at the aperture, opaque at the outer radius.
    gl_Begin(GL_QUAD_STRIP_);
    for (int i = 0; i <= VIG_SEGMENTS; ++i) {
        const float a = (float)i / VIG_SEGMENTS * 6.28318531f;
        const float ca = cosf(a) / ax, sa = sinf(a) / ay;
        gl_Color4f(0.0f, 0.0f, 0.0f, 0.0f);
        gl_Vertex2f(ca * rIn, sa * rIn);
        gl_Color4f(0.0f, 0.0f, 0.0f, 1.0f);
        gl_Vertex2f(ca * rOut, sa * rOut);
    }
    gl_End();

    // Solid surround out past the corners.
    gl_Begin(GL_QUAD_STRIP_);
    for (int i = 0; i <= VIG_SEGMENTS; ++i) {
        const float a = (float)i / VIG_SEGMENTS * 6.28318531f;
        const float ca = cosf(a) / ax, sa = sinf(a) / ay;
        gl_Color4f(0.0f, 0.0f, 0.0f, 1.0f);
        gl_Vertex2f(ca * rOut, sa * rOut);
        gl_Color4f(0.0f, 0.0f, 0.0f, 1.0f);
        gl_Vertex2f(ca * rFar, sa * rFar);
    }
    gl_End();

    // --- restore ---
    gl_Color4f(1.0f, 1.0f, 1.0f, 1.0f);
    gl_MatrixMode(GL_MODELVIEW_);
    gl_PopMatrix();
    gl_MatrixMode(GL_PROJECTION_);
    gl_PopMatrix();
    gl_MatrixMode(GL_MODELVIEW_);

    gl_BindTexture(GL_TEXTURE_2D_, (unsigned)oldTex);
    if (hadTex2D)   gl_Enable(GL_TEXTURE_2D_);
    else            gl_Disable(GL_TEXTURE_2D_);
    if (gl_BlendFunc)  gl_BlendFunc((unsigned)oldBlendSrc, (unsigned)oldBlendDst);
    if (gl_ShadeModel) gl_ShadeModel((unsigned)oldShade);
    if (!hadBlend)  gl_Disable(GL_BLEND_);
    if (hadDepth)   gl_Enable(GL_DEPTH_TEST_);
    if (hadCull)    gl_Enable(GL_CULL_FACE_);
    if (hadLight)   gl_Enable(GL_LIGHTING_);
    gl_BindFramebuffer(GL_FRAMEBUFFER_, 0);
    if (hadScissor) gl_Enable(GL_SCISSOR_TEST_);
    if (gl_GetIntegerv)
        gl_Viewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);

    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("comfort: vignette active (%.0f%% closed)", g_vigCurrent * 100.0f);
    }
    return true;
}

// ------------------------------------------------------ frame rate watch
//
// Frames alternate between eyes, so each eye is refreshed at half the game's
// rate. That is fine at the hundreds of frames a 2003 engine normally manages,
// but if the game ever drops near the headset's own refresh the two eyes start
// disagreeing badly enough to be uncomfortable. Supersampling is the setting
// that costs the most, so say so plainly rather than leaving it to be guessed.
enum { FPS_WINDOW = 600 };

static DWORD g_fpsWindowStart;
static int   g_fpsFrames;
static float g_fps;
static bool  g_fpsWarned;

float Comfort_FrameRate() { return g_fps; }

void Comfort_NoteFrame()
{
    const DWORD now = GetTickCount();
    if (g_fpsWindowStart == 0) {
        g_fpsWindowStart = now;
        g_fpsFrames = 0;
        return;
    }
    ++g_fpsFrames;
    const DWORD elapsed = now - g_fpsWindowStart;
    if (elapsed < 2000)
        return;

    g_fps = g_fpsFrames * 1000.0f / (float)elapsed;
    g_fpsWindowStart = now;
    g_fpsFrames = 0;

    static int reports = 0;
    if (reports < 3) {
        ++reports;
        Log("perf: %.0f fps (%.0f per eye -- frames alternate)", g_fps, g_fps * 0.5f);
    }

    // Below roughly twice the headset's refresh, each eye falls under the
    // display rate and the alternation becomes visible.
    if (!g_fpsWarned && g_fps > 1.0f && g_fps < 150.0f) {
        g_fpsWarned = true;
        Log("");
        Log("perf: WARNING -- %.0f fps means only %.0f per eye. Frames alternate",
            g_fps, g_fps * 0.5f);
        Log("  between eyes, so this is where the two start visibly disagreeing.");
        Log("  Lower 'supersample' in codvr.cfg (2.25 -> 1.75 -> 1.5); it is by");
        Log("  far the most expensive setting.");
        Log("");
    }
}

// ----------------------------------------------------------------- input

// Edge-triggered so holding a key fires once. Only acts while the game has
// focus, or the mod would respond to typing in other applications.
static bool KeyPressedEdge(int vk, bool* prev)
{
    if (!vk) { *prev = false; return false; }
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool fired = down && !*prev;
    *prev = down;
    return fired;
}

static bool GameHasFocus()
{
    const HWND fg = GetForegroundWindow();
    if (!fg)
        return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// Snap turn drives the game's own aim, so the weapon turns with the view rather
// than the two drifting apart. The engine reads relative mouse motion, so a
// synthetic horizontal movement is the honest way to turn it.
static void InjectYaw(int degrees)
{
    const int counts = (int)(degrees * g_snapMouseScale);
    if (!counts)
        return;
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dx = counts;
    in.mi.dy = 0;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
}

void Comfort_PollInput()
{
    static bool prevRecenter = false, prevLeft = false, prevRight = false;

    if (!GameHasFocus()) {
        prevRecenter = prevLeft = prevRight = false;
        return;
    }

    if (KeyPressedEdge(g_recenterKey, &prevRecenter))
        VR_Recenter();

    if (g_snapTurnDeg > 0) {
        if (KeyPressedEdge(g_snapLeftKey,  &prevLeft))
            InjectYaw(-g_snapTurnDeg);
        if (KeyPressedEdge(g_snapRightKey, &prevRight))
            InjectYaw(+g_snapTurnDeg);
    }
}

// ------------------------------------------------------------- self test

// Maps a config name like "F9" or "HOME" to a virtual-key code, so the keys are
// settable without anyone having to look up hex codes.
int Comfort_ParseKey(const char* name)
{
    if (!name || !*name)
        return 0;
    if (!_stricmp(name, "none") || !_stricmp(name, "off"))
        return 0;
    if ((name[0] == 'F' || name[0] == 'f') && name[1] >= '1' && name[1] <= '9') {
        int n = atoi(name + 1);
        if (n >= 1 && n <= 24)
            return VK_F1 + (n - 1);
    }
    if (!_stricmp(name, "home"))     return VK_HOME;
    if (!_stricmp(name, "end"))      return VK_END;
    if (!_stricmp(name, "insert"))   return VK_INSERT;
    if (!_stricmp(name, "delete"))   return VK_DELETE;
    if (!_stricmp(name, "pageup"))   return VK_PRIOR;
    if (!_stricmp(name, "pagedown")) return VK_NEXT;
    if (!_stricmp(name, "left"))     return VK_LEFT;
    if (!_stricmp(name, "right"))    return VK_RIGHT;
    if (!_stricmp(name, "up"))       return VK_UP;
    if (!_stricmp(name, "down"))     return VK_DOWN;
    if (!_stricmp(name, "tab"))      return VK_TAB;
    if (name[1] == '\0') {
        char c = name[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return (int)c;
    }
    return 0;
}

// Testing seam: drive the vignette without a game running.
void Comfort_TestSetVignette(float v) { g_vigCurrent = v; }

// Testing seam: the aperture the current amount maps to.
void Comfort_TestAperture(float amount, float* inner, float* outer)
{
    ApertureRadii(amount, inner, outer);
}

```


---

## `src/probe.cpp`  (411 lines)

Headset-free self-scoring diagnostic (probe mode).

```cpp
// The self-scoring diagnostic, split out of dllmain.cpp.
//
// This runs INSTEAD of VR (codvr.cfg "probe 1") and needs no headset: the game
// runs flat on the monitor and writes a plain-language verdict to the log. That
// makes it the cheap way to settle questions which would otherwise cost a play
// session, and the game cannot be launched by script -- so every question that
// can be answered here is one the user does not have to test by hand.

#include "codvr.h"
#include "vr.h"

#include <math.h>
#include <float.h>

// The two syscalls that each receive the camera struct once per frame. Owned by
// dllmain because the naked assembly interposer branches on them directly.
extern int g_candA, g_candB;

static bool g_enabled;

bool Probe_Enabled()          { return g_enabled; }
void Probe_SetEnabled(bool on) { g_enabled = on; }

// ------------------------------------------------------------- experiment

enum { PH_BASE0 = 0, PH_HALF_A, PH_HALF_B, PH_DBL_A, PH_DBL_B, PH_DONE, PH_COUNT };
static const char* kPhaseName[PH_COUNT] = {
    "baseline (untouched)",
    "halve viewport via syscall 71",
    "halve viewport via syscall 243",
    "double render via syscall 71",
    "double render via syscall 243",
    "restored (untouched)"
};

enum { PHASE_MS = 5000, PHASE_MIN_FRAMES = 120 };

static volatile LONG g_phase;
static DWORD g_phaseStartTick;
static LONG  g_framesInPhase;
static bool  g_started;

static double g_ratio[PH_COUNT];
static bool   g_ratioValid[PH_COUNT];

// ------------------------------------------------ camera authority test
//
// THE decisive question: do our writes to the refdef actually drive the
// renderer? The old left/right viewport test cannot answer it -- it assumes the
// engine clears the half we stop drawing into, and if the engine never clears,
// the stale half looks unchanged whether our write worked or not. That false
// negative sent a whole session chasing the wrong layer.
//
// This test cannot be fooled that way. It toggles a drastic camera displacement
// on alternate frames and measures how much consecutive frames differ, then
// compares that against how much they differ with no toggling at all (ordinary
// scene motion). If our writes reach the renderer, toggling teleports the camera
// every other frame and the difference is enormous. If they do not, the two are
// the same. No assumption about clearing, viewports, or symmetry.
enum { CAM_SAMPLE_W = 512, CAM_SAMPLE_H = 8,
       CAM_PIX_BYTES = CAM_SAMPLE_W * CAM_SAMPLE_H * 4,
       CAM_SETTLE_FRAMES = 60, CAM_MEASURE_FRAMES = 120 };
enum { CAM_SETTLE = 0, CAM_MOTION, CAM_TOGGLE, CAM_DONE };

static int    g_camStage;
static int    g_camFrames;
static double g_camMotionSum;  static int g_camMotionN;
static double g_camToggleSum;  static int g_camToggleN;
static bool   g_camDisplaceNow;
static BYTE   g_camCur[CAM_PIX_BYTES];
static BYTE   g_camPrev[CAM_PIX_BYTES];
static bool   g_camPrevValid;

enum { GL_RGBA_ = 0x1908, GL_UNSIGNED_BYTE_ = 0x1401 };

typedef void (WINAPI *ReadPixelsFn)(int,int,int,int,unsigned,unsigned,void*);

static ReadPixelsFn ReadPixels()
{
    static ReadPixelsFn fn = nullptr;
    static bool looked = false;
    if (!looked) { looked = true; fn = (ReadPixelsFn)CODVR_GetGLProc("glReadPixels"); }
    return fn;
}

// Samples a fixed band of the frame and accumulates how much it changes between
// consecutive frames.
static void StepCameraAuthorityTest()
{
    if (g_camStage == CAM_DONE)
        return;

    ReadPixelsFn readPixels = ReadPixels();
    int w = (int)g_viewW, h = (int)g_viewH;
    if (!readPixels || w < CAM_SAMPLE_W || h < 64)
        return;                       // no scene yet; do not advance the test

    const int x0 = w/2 - CAM_SAMPLE_W/2;
    const int y0 = h/2 - CAM_SAMPLE_H/2;
    memset(g_camCur, 0, sizeof(g_camCur));
    readPixels(x0, y0, CAM_SAMPLE_W, CAM_SAMPLE_H, GL_RGBA_, GL_UNSIGNED_BYTE_, g_camCur);

    if (g_camPrevValid) {
        double sum = 0;
        for (int i = 0; i < CAM_PIX_BYTES; ++i) {
            int d = (int)g_camCur[i] - (int)g_camPrev[i];
            sum += d < 0 ? -d : d;
        }
        const double mad = sum / CAM_PIX_BYTES;
        if (g_camStage == CAM_MOTION)      { g_camMotionSum += mad; ++g_camMotionN; }
        else if (g_camStage == CAM_TOGGLE) { g_camToggleSum += mad; ++g_camToggleN; }
    }
    memcpy(g_camPrev, g_camCur, CAM_PIX_BYTES);
    g_camPrevValid = true;

    ++g_camFrames;
    const int limit = (g_camStage == CAM_SETTLE) ? CAM_SETTLE_FRAMES : CAM_MEASURE_FRAMES;
    if (g_camFrames < limit) {
        // Set here, at the end of a frame, so it applies to the frame the
        // engine builds next.
        g_camDisplaceNow = (g_camStage == CAM_TOGGLE) && ((g_camFrames & 1) != 0);
        return;
    }

    g_camFrames = 0;
    g_camPrevValid = false;
    ++g_camStage;

    if (g_camStage == CAM_DONE) {
        g_camDisplaceNow = false;
        const double motion = g_camMotionN ? g_camMotionSum / g_camMotionN : 0.0;
        const double toggle = g_camToggleN ? g_camToggleSum / g_camToggleN : 0.0;
        Log("");
        Log("========= CAMERA AUTHORITY TEST =========");
        Log("  ordinary frame-to-frame change : %.2f  (%d samples)", motion, g_camMotionN);
        Log("  with camera toggled every other: %.2f  (%d samples)", toggle, g_camToggleN);
        if (g_camMotionN < 10 || g_camToggleN < 10) {
            Log("  INCONCLUSIVE - too few frames with a scene on screen.");
            Log("  Load a mission and play continuously for ~20 seconds.");
        } else if (motion < 0.5 && toggle < 0.5) {
            Log("  INCONCLUSIVE - the sampled band never changed at all.");
            Log("  The back buffer may not be where the scene is being drawn.");
        } else if (toggle > motion * 3.0) {
            Log("  RESULT: our refdef writes DO drive the renderer.");
            Log("  Teleporting the camera changed the image %.1fx more than",
                motion > 0.01 ? toggle/motion : 0.0);
            Log("  ordinary motion. The camera hook is sound; the remaining");
            Log("  faults are in presentation (blit/FOV/pose), not authority.");
        } else {
            Log("  RESULT: our refdef writes DO **NOT** drive the renderer.");
            Log("  Teleporting the camera every other frame changed the image");
            Log("  no more than ordinary motion did. The engine is reading its");
            Log("  camera from somewhere other than the struct we are editing.");
            Log("  The whole per-eye camera approach needs to change: find the");
            Log("  real camera source before doing any more VR work.");
        }
        Log("=========================================");
    }
}

// ---------------------------------------------------- framebuffer scoring

enum { PIX_BYTES = 4096 * 16 * 4 };     // wide enough for a 4K-wide viewport
static BYTE g_pixels[PIX_BYTES];

// Detail level, not a hash. A hash only says two regions differ, which is
// uninformative for an asymmetric scene. What separates "something rendered
// here" from "nothing did" is texture: a drawn scene has high luminance
// variance; a region nothing wrote to collapses to the clear colour.
struct HalfStats { double mean, stddev; long nonBlack, total; };

static HalfStats MeasureHalf(const BYTE* px, int w, int rows, int x0, int x1)
{
    HalfStats s = {0, 0, 0, 0};
    double sum = 0, sumSq = 0;
    for (int y = 0; y < rows; ++y) {
        const BYTE* row = px + (size_t)y * w * 4;
        for (int x = x0; x < x1; ++x) {
            const BYTE* q = row + (size_t)x * 4;
            double luma = 0.299*q[0] + 0.587*q[1] + 0.114*q[2];
            sum += luma; sumSq += luma * luma;
            if (q[0] | q[1] | q[2]) ++s.nonBlack;
            ++s.total;
        }
    }
    if (s.total) {
        s.mean = sum / s.total;
        double var = sumSq / s.total - s.mean * s.mean;
        s.stddev = var > 0 ? sqrt(var) : 0.0;
    }
    return s;
}

static void MeasurePhase(int phase)
{
    ReadPixelsFn readPixels = ReadPixels();
    if (!readPixels) { Log("  phase %d: glReadPixels unavailable", phase); return; }

    int w = (int)g_viewW, h = (int)g_viewH;
    if (w < 64 || h < 64) { Log("  phase %d: no viewport yet", phase); return; }

    // Adapt the row count to the width so any resolution is measurable rather
    // than skipped -- a skipped phase wastes the whole session.
    int rows = PIX_BYTES / (w * 4);
    if (rows > 16) rows = 16;
    if (rows < 1)  { Log("  phase %d: viewport %d too wide to sample", phase, w); return; }

    memset(g_pixels, 0, (size_t)w * rows * 4);
    readPixels(0, h/2 - rows/2, w, rows, GL_RGBA_, GL_UNSIGNED_BYTE_, g_pixels);

    const int half = w / 2;
    HalfStats L = MeasureHalf(g_pixels, w, rows, 0, half);
    HalfStats R = MeasureHalf(g_pixels, w, rows, half, w);
    double ratio = (L.stddev > 1.0) ? (R.stddev / L.stddev) : -1.0;

    Log("  phase %d [%s]  (%d rows of %d px)", phase, kPhaseName[phase], rows, w);
    Log("     left  mean %6.1f stddev %6.2f nonblack %ld/%ld",
        L.mean, L.stddev, L.nonBlack, L.total);
    Log("     right mean %6.1f stddev %6.2f nonblack %ld/%ld",
        R.mean, R.stddev, R.nonBlack, R.total);
    Log("     right/left detail ratio = %.3f", ratio);

    if (ratio >= 0 && phase >= 0 && phase < PH_COUNT) {
        g_ratio[phase] = ratio;
        g_ratioValid[phase] = true;
    } else {
        Log("     (inconclusive: left half had no detail either)");
    }
}

static void Conclude()
{
    Log("");
    Log("=================== CONCLUSIONS ===================");

    if (!g_ratioValid[PH_BASE0]) {
        Log("  Baseline never scored -- not enough gameplay was rendered.");
        Log("  Load a mission and play ~30 seconds without pausing.");
        Log("===================================================");
        return;
    }
    const double base = g_ratio[PH_BASE0];
    Log("  baseline right/left detail ratio = %.3f", base);

    struct { int half, dbl, id; } cand[2] = {
        { PH_HALF_A, PH_DBL_A, g_candA },
        { PH_HALF_B, PH_DBL_B, g_candB }
    };

    int renderCall = -1;
    bool doubleWorks = false;

    for (int i = 0; i < 2; ++i) {
        Log("");
        Log("  --- syscall %d ---", cand[i].id);
        if (!g_ratioValid[cand[i].half]) {
            Log("    viewport phase not scored");
            continue;
        }
        double rh = g_ratio[cand[i].half];
        bool consumed = rh < 0.25 * base;
        Log("    halve viewport : ratio %.3f -> %s", rh,
            consumed ? "RIGHT HALF WENT FLAT: this refdef IS consumed by the renderer"
                     : "no change: this refdef is NOT what the renderer reads");
        if (!consumed)
            continue;
        renderCall = cand[i].id;

        if (!g_ratioValid[cand[i].dbl]) {
            Log("    double render  : not scored");
            continue;
        }
        double rd = g_ratio[cand[i].dbl];
        bool refilled = rd > 0.5 * base;
        Log("    double render  : ratio %.3f -> %s", rd,
            refilled ? "RIGHT HALF REFILLED: two scene renders per frame ACCEPTED"
                     : "stayed flat: the second render drew nothing");
        if (refilled)
            doubleWorks = true;
    }

    Log("");
    if (renderCall < 0) {
        Log("  RESULT: neither candidate's refdef drives the renderer.");
        Log("  The camera is read from somewhere else; the cgame syscall hook");
        Log("  is not sufficient and the architecture needs revisiting.");
    } else if (!doubleWorks) {
        Log("  RESULT: syscall %d IS the render call and our writes reach it,", renderCall);
        Log("  but the engine renders only the first scene per frame. Stereo");
        Log("  cannot be done by calling it twice; it will need either a");
        Log("  per-eye alternating frame or a lower-level hook.");
    } else {
        Log("  RESULT: syscall %d is R_RenderScene, our writes drive the camera,", renderCall);
        Log("  and the engine accepts two scene renders per frame with");
        Log("  independent viewports and cameras.");
        Log("  This is precisely the mechanism stereo VR requires. CONFIRMED.");
    }
    Log("===================================================");
}

void Probe_TestSetPhase(int phase)
{
    InterlockedExchange(&g_phase, phase);
    // The camera authority test runs ahead of the phases in a real session;
    // the self test targets the phase machine directly, so mark it finished.
    g_camStage = CAM_DONE;
    g_camDisplaceNow = false;
    g_started = (phase != PH_BASE0);
}

void Probe_ConcludeIfRunning()
{
    if (g_phase > PH_BASE0 && g_phase < PH_DONE) {
        Log("");
        Log("(process exited mid-experiment at phase %ld -- partial results)", g_phase);
        Conclude();
    }
}

// ------------------------------------------------------------- hook points

bool Probe_RenderHook(int num, void* p,
                      int a2, int a3, int a4, int a5, int a6, int a7, int* ret)
{
    if (!g_enabled)
        return false;

    int*   iv = (int*)p;
    float* fv = (float*)p;

    if (!g_started) {
        g_started = true;
        g_phaseStartTick = GetTickCount();
    }

    // Camera authority test: teleport the camera far above the map on the
    // frames the test asks for. If our writes drive the renderer this is
    // impossible to miss; if they do not, nothing changes.
    if (g_camDisplaceNow && num == g_renderSyscall) {
        const float oz = fv[RD_ORG+2];
        fv[RD_ORG+2] = oz + 4096.0f;
        *ret = g_realSyscall(num, p, a2, a3, a4, a5, a6, a7);
        fv[RD_ORG+2] = oz;
        return true;
    }

    const LONG phase = g_phase;
    const bool isA = (num == g_candA), isB = (num == g_candB);
    const bool halve   = (phase == PH_HALF_A && isA) || (phase == PH_HALF_B && isB);
    const bool doubled = (phase == PH_DBL_A  && isA) || (phase == PH_DBL_B  && isB);
    if (!halve && !doubled)
        return false;                     // let the caller forward it normally

    const int   sx = iv[RD_X], sy = iv[RD_Y], sw = iv[RD_W], sh = iv[RD_H];
    const float ox = fv[RD_ORG], oy = fv[RD_ORG+1], oz = fv[RD_ORG+2];
    const int   half = sw / 2;

    // Left half only. If this write reaches the renderer, nothing draws into
    // the right half and it collapses to the clear colour.
    iv[RD_X] = sx; iv[RD_W] = half;
    int r = g_realSyscall(num, p, a2, a3, a4, a5, a6, a7);

    if (doubled) {
        // Second scene render, same frame, own viewport and own camera.
        iv[RD_X] = sx + half; iv[RD_W] = half;
        fv[RD_ORG+2] = oz + 500.0f;
        g_realSyscall(num, p, a2, a3, a4, a5, a6, a7);
    }

    iv[RD_X] = sx; iv[RD_Y] = sy; iv[RD_W] = sw; iv[RD_H] = sh;
    fv[RD_ORG] = ox; fv[RD_ORG+1] = oy; fv[RD_ORG+2] = oz;
    *ret = r;
    return true;
}

bool Probe_FrameHook()
{
    if (!g_enabled)
        return false;

    // The camera authority test runs first and gates everything else: if our
    // writes do not reach the renderer, the later phases are noise.
    if (g_camStage != CAM_DONE) {
        StepCameraAuthorityTest();
        return true;                      // caller presents immediately
    }

    if (!g_started)
        return false;

    ++g_framesInPhase;

    // Advance one phase at a time, and only once the phase has both run long
    // enough AND actually rendered enough frames. Gating on frames as well as
    // time keeps a pause or alt-tab from silently skipping phases while nothing
    // is being drawn.
    LONG phase = g_phase;
    if (phase < PH_DONE
        && (GetTickCount() - g_phaseStartTick) >= PHASE_MS
        && g_framesInPhase >= PHASE_MIN_FRAMES)
    {
        MeasurePhase((int)phase);         // scores the frame just drawn
        InterlockedExchange(&g_phase, phase + 1);
        g_phaseStartTick = GetTickCount();
        g_framesInPhase = 0;
        if (phase + 1 == PH_DONE)
            Conclude();
    }
    return false;
}

```
