#include "vr.h"
#include "codvr.h"
#include "vr_math.h"
#include "vr_input.h"
#include "vr_weapon.h"
#include "vr_binds.h"
#include "vr_native_hands.h"
#include "vr_menu.h"
#include "vr_snapshot.h"
#include "vr_glsurf.h"
#include "vr_handpose.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <float.h>

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
// for that eye (scene AND the 2D pass drawn over it). The two eyes of one XR
// frame are captured on consecutive game frames (the pair machine below);
// both are submitted together when the second capture lands.
enum { GL_SCISSOR_TEST_ = 0x0C11, GL_TEXTURE_MIN_FILTER_ = 0x2801,
       GL_TEXTURE_MAG_FILTER_ = 0x2800, GL_CLAMP_TO_EDGE_ = 0x812F,
       GL_TEXTURE_WRAP_S_ = 0x2802, GL_TEXTURE_WRAP_T_ = 0x2803,
       GL_UNSIGNED_BYTE_ = 0x1401, GL_RGBA_ = 0x1908 };
static unsigned g_eyeTex[EYES];
static int      g_capSizeW[EYES], g_capSizeH[EYES];   // per eye: runtimes may differ per view
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

// Menu/panel safe area. The 2026-08-19 hardware run: menu content clipped
// slightly on all four sides. Cause: the old fitting (70% of texture height,
// up to 95% of width) sized the panel against the TEXTURE, but the texture is
// submitted with the symmetric-superset fov (54/55 deg) while the lens only
// shows the smaller asymmetric box (40 deg nasal, 44 deg up) -- so panel
// edges landed at angles only one eye (or neither) can see. The fit is now
// ANGULAR: content stays inside the binocularly-visible half-angles times
// g_menuScale. g_menuDistM > 0 additionally shifts each eye nasally so the
// panel converges at that distance instead of infinity (0 = old behavior).
//
// Default derivation (2026-08-20 run: 0.90 STILL clipped on all four edges).
// At scale s the content half-angles are atan(s*tan(vis)): 0.90 leaves only
// ~3 deg of true margin against the RUNTIME-REPORTED box (37.1 vs 40 nasal,
// 41.0 vs 44 up) -- but the reported fov is the RENDER frustum, which
// overscans what the lens physically shows, and the pinned panel pose adds a
// further few degrees whenever the head is not exactly at the pin. Both
// margins together clearly exceed 3 deg on this hardware. 0.80 puts content
// at 33.9 x 37.7 deg -- a real ~6 deg margin per edge against the reported
// box -- with the actual content/visible angles logged per fit so the next
// run can verify instead of guessing again.
static float g_menuScale = 0.80f;
static float g_menuDistM = 0.0f;
// The VR UI safe area (brief 2026-09-04, P3): a further uniform inset on
// every 2D surface, menus and HUD alike. See vr.h.
static float g_uiScale = VR_UI_SCALE_DEFAULT;
static float g_uiSafeX = 1.0f;
static float g_uiSafeY = 1.0f;
static float g_menuFitLoggedX = -1.0f, g_menuFitLoggedY = -1.0f;
static float g_menuContentHalfX, g_menuContentHalfY;   // degrees, last fit
static float g_menuVisHalfX, g_menuVisHalfY;
static bool  g_menuSafeValid;

// Seated-height comfort: metres added to the head's vertical translation.
static float g_heightAdjustM = 0.0f;

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

// CoD's own mapping standard is 1 world unit = 1 inch (official CoD Radiant
// gameplay standards: 1 unit = 0.0254 m, player 72 units = 6 ft tall, eye
// level ~64 units), so 39.37 units per metre is the engine's true scale --
// not a Quake-family guess. (Quake 3 ports use ~32 because Q3's player is
// only 56 units for ~1.75 m; that reasoning does not transfer to CoD.)
static float g_unitsPerMetre = 39.37f;

// Comfort knob over physical head translation only. 1.0 = physically
// correct. Deliberately NOT applied to the inter-eye offset: scaling IPD
// would change stereo depth, which is a different (and worse) lever.
static float g_positionalGain = 1.0f;

// Degrees added to each side of the symmetric CULLING fov beyond the exact
// superset of the eye's frustum. The engine may recompute fov_y from the
// viewport aspect; the margin keeps such quirks from culling geometry the
// real (asymmetric) projection can see. Overdraw is the only cost.
static float g_cullMarginDeg = 5.0f;

// --------------------------------------------------------------- xr state

static XrInstance   g_instance   = XR_NULL_HANDLE;
static XrSystemId   g_systemId   = XR_NULL_SYSTEM_ID;
static XrSession    g_session    = XR_NULL_HANDLE;
static XrSpace      g_space      = XR_NULL_HANDLE;
static XrSessionState g_sessionState = XR_SESSION_STATE_UNKNOWN;

static XrSwapchain  g_swapchain[EYES] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
static XrSwapchainImageOpenGLKHR* g_images[EYES];
static uint32_t     g_imageCount[EYES];
static uint32_t     g_eyeSizeW[EYES], g_eyeSizeH[EYES];   // per view, from the runtime
static float        g_submitHalfX[EYES], g_submitHalfY[EYES];  // radians, what we rendered

static XrFrameState g_frameState = {};
static XrView       g_views[EYES];
static bool         g_viewsValid;
static bool         g_everLocated;   // have we ever had a good pose?
static bool         g_frameBegun;
static bool         g_sessionRunning;
static bool         g_sessionComplete;  // session AND space AND swapchains all exist
static bool         g_startupFailed;    // permanent: no loader beside the exe
static bool         g_userExited;       // runtime said EXITING: do not auto-restart
static DWORD        g_lastRetryTick;
static char         g_status[256] = "not started";

// The blend mode submitted with every frame. Enumerated from the runtime at
// startup (first entry is the runtime's preference); OPAQUE is only the
// fallback when enumeration itself fails.
static XrEnvironmentBlendMode g_blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

// Consecutive hard XR failures in the frame/submit path. A handful is a
// transient; a steady stream means the session is broken in a way no retry of
// the same call will fix, so the session is torn down and the normal startup
// retry path rebuilds it.
static int g_xrConsecutiveFailures;
enum { XR_FAILURE_TEARDOWN_THRESHOLD = 300 };   // ~3 s at headset cadence

static HDC   g_hdc;
static HGLRC g_glrc;

// Neutral head reference, in Quake units, latched ATOMICALLY from one
// successful two-view locate in VR_BeginFrameEx -- never from whichever eye
// happens to render next, which used to let a recenter press produce one
// frame with completely uncancelled yaw in a single eye.
//
// The LOCAL reference space puts its origin wherever the headset was when the
// session began -- typically 1.2-1.7 m off the floor. The game's vieworg is
// ALREADY the player's eye position (~60 units up), so adding the head's
// absolute position on top lifted the camera another ~50-67 units, into or
// through the ceiling: the view renders black and only the 2D HUD survives.
// Subtracting this reference makes the latch pose the neutral point, so the
// game's own eye height is respected and only real leaning/crouching/stepping
// moves the camera.
//
// The position reference is the MIDPOINT of the two eyes, so at neutral the
// eyes sit at -IPD/2 and +IPD/2 and the binocular centre lands exactly on the
// game's camera. Referencing eye 0 instead (the old scheme) preserved the
// separation but shifted the whole head half an IPD toward eye 1.
// Stored in METRES (Quake axes): scale and gain apply at use time, so a
// worldscale change never mixes units inside a latched reference.
static float g_headRefM[3];
static bool  g_headRefValid;
static float g_headYawRef;        // yaw cancelled out of the head orientation
static bool  g_headYawRefValid;
static bool  g_relatchPending = true;   // consumed by the next good locate

// Last per-eye position that passed the sanity gate, in raw XR metres. When a
// pose sample goes absurd (a real run reported y = -1592 m) the eye falls back
// to its own last sane sample, which preserves the inter-eye separation --
// zeroing the delta instead collapsed BOTH eyes onto one point and flattened
// the world one eye at a time.
static XrVector3f g_lastSanePos[EYES];
static int  g_pairFarStreak;        // consecutive consistent-but-far pairs
static bool g_originAnomalyLogged;
static bool       g_lastSaneValid[EYES];

// Where the head is pointing in the game's world, in Quake basis form. The
// input layer reads this to steer the game's own aim toward the view.
static float g_headWorldAxis[9];
static bool  g_headWorldValid;

// The head's OWN yaw in room terms (neutral cancelled), independent of any
// game-aim composition. The steering-leak detector compares rendered-camera
// yaw deltas against body-yaw + THIS, so legitimate head turns are never
// mistaken for weapon-steering leaking into the camera.
static float g_headLocalYawDeg;
static bool  g_headLocalYawValid;

// ---- render/submit pose consistency (the Phase-7 probe) ----
// One relocate at the pair's own displayTime, compared per eye against the
// records the layer is about to submit. The submitted pose IS the locate the
// cameras rendered from, so any nonzero mismatch is the runtime revising its
// prediction between locate and submit -- exactly the quantity a "world
// slides when I translate" report needs measured, not guessed at.
static bool  g_consistencyArmed;         // test seam: force on next stereo submit
static bool  g_consistencyDone;          // the automatic one-shot fired
static int   g_consistencyValid;         // results below are meaningful
static float g_consistencyPosM[EYES];    // translation mismatch, metres
static float g_consistencyRotDeg[EYES];  // rotation mismatch, degrees
static double g_pairLocateTime;          // QPC seconds at the pair's locate

// How far the head is turned away from the game's aim, in degrees. Positive
// yaw is to the left, positive pitch is up. Used both for head-aim steering and
// for the comfort vignette.
static float g_headYawOffsetDeg;
static float g_headPitchOffsetDeg;

// Player motion, measured from the game's own camera between frames. Vection --
// the sick feeling from seeing movement you cannot feel -- tracks translation
// speed and turn rate, so the comfort vignette keys off both.
//
// Timing is QueryPerformanceCounter: GetTickCount's 15.6 ms granularity meant
// most frames measured dt = 0 (sample dropped) and the rest measured a whole
// tick, so the estimates lurched. Sub-millisecond intervals are accumulated
// rather than dropped, so no movement is ever lost to a fast frame.
static float  g_prevOrigin[3];
static bool   g_prevOriginValid;
static float  g_prevAimYaw;
static bool   g_prevAimYawValid;
static double g_prevMotionTime;
static double g_motionDtAccum;
static float  g_pendingDist;       // distance accumulated while dt was too small
static float  g_pendingDyawDeg;    // yaw accumulated the same way
static float  g_playerSpeed;       // engine units per second
static float  g_turnRate;          // degrees per second

// One smoothing step with an explicit dt: frame-rate independent, matching
// the old 0.25-per-frame feel at 72 Hz (tau ~= 48 ms).
static float MotionSmooth(float current, float instantaneous, float dt)
{
    const float tau = 0.048f;
    const float k = 1.0f - expf(-dt / tau);
    return current + (instantaneous - current) * k;
}

// Translation filter: teleports (level load, respawn) read as absurd
// single-interval distances and would slam the vignette shut for nothing.
static float FilterSpeed(float current, float distUnits, float dt)
{
    if (dt <= 0.0f) return current;
    const float inst = (distUnits < 400.0f) ? distUnits / dt : 0.0f;
    return MotionSmooth(current, inst, dt);
}

// Turn filter: a single-interval yaw step beyond anything a human sweeps with
// a mouse in ~1/90 s is a DISCRETE turn -- snap turn's injected step, or a
// respawn -- and the whole point of a discrete turn is that no continuous
// rotation was shown. Feeding it in as 2000+ deg/s slammed the comfort tunnel
// shut immediately AFTER the snap, when there was nothing to protect against.
static float FilterTurn(float current, float dyawDeg, float dt)
{
    if (dt <= 0.0f) return current;
    const float inst = (fabsf(dyawDeg) < 20.0f) ? fabsf(dyawDeg) / dt : 0.0f;
    return MotionSmooth(current, inst, dt);
}

float VR_TestFilterSpeed(float current, float distUnits, float dtSeconds)
{
    return FilterSpeed(current, distUnits, dtSeconds);
}
float VR_TestFilterTurn(float current, float dyawDegrees, float dtSeconds)
{
    return FilterTurn(current, dyawDegrees, dtSeconds);
}

// Pose the 2D panel is pinned to. A flat screen (menu, briefing, loading) has
// no world position of its own, so it was submitted at the live head pose --
// which nails it to your face and makes it swim as you look around. Latching
// the pose once, when the panel first appears, leaves it hanging in the world
// so you can look around it instead of dragging it with you.
static XrPosef g_panelPose[EYES];
static bool    g_panelPinned;
// View-state flags of the most recent successful locate: the panel pin waits
// for ORIENTATION_TRACKED so a pre-tracking attitude can never be latched.
static XrViewStateFlags g_lastViewStateFlags;

// Everything describing HOW an eye's texture was rendered, captured in the
// same moment as the texture itself. The pose is what reprojection visibly
// depends on; the fov and the symmetric half-angles decide the
// sub-rectangle; and the SERIAL pins the capture to the located XR frame it
// belongs to. Both records of a submitted stereo pair must carry the current
// frame's serial -- that is the guard that keeps a "one fresh / one stale"
// mixed-generation pair from ever being reintroduced through an edge case.
struct EyeRenderRecord {
    bool     valid;
    XrPosef  pose;          // head pose the camera used
    XrFovf   runtimeFov;    // the runtime's asymmetric frustum that frame
    float    halfX, halfY;  // symmetric superset actually rendered (radians);
                            // 0 = not a 3D scene (flat panel fitted to the eye)
    unsigned serial;        // g_xrFrameSerial of the locate this belongs to
};
static EyeRenderRecord g_eyeRecord[EYES];

// ------------------------------------------------------- the pair machine
//
// One OpenXR frame spans TWO 3D game frames. The old loop ran a full
// wait/begin/locate/end cycle per GAME frame, which put xrWaitFrame -- the
// call that deliberately throttles the app to display cadence -- between
// every eye render: 90 game frames/s became 45 fresh frames per eye. The
// pair machine holds ONE begun XR frame open across both eye renders:
//
//   PAIR_IDLE       no XR frame begun. The next VR_BeginFrameEx runs the real
//                   wait/begin/locate and opens a pair.
//   PAIR_FIRST_EYE  frame begun and views located; nothing captured yet. The
//                   next 3D game frame must be eye 0; a flat frame instead is
//                   the normal menu path and uses this same open frame.
//   PAIR_SECOND_EYE eye 0 captured fresh for this frame. The next 3D game
//                   frame must be eye 1, after which BOTH eyes are copied out
//                   and the frame is ended with one stereo layer. Between
//                   these two renders there is NO xrWaitFrame, NO
//                   xrBeginFrame and NO xrLocateViews.
//
// Every abort path -- shouldRender=false, session loss, capture or swapchain
// failure, a flat frame interrupting a pair, an unexpected eye -- ends the
// open frame with zero layers (or lets the panel take it over, for flat) and
// resets to PAIR_IDLE/PAIR_FIRST_EYE, so rendering always resumes at eye 0
// with a freshly located view pair. Illegal transitions log themselves.
enum PairPhase {
    PAIR_IDLE       = 0,
    PAIR_FIRST_EYE  = 1,
    PAIR_SECOND_EYE = 2
};
static PairPhase g_pairPhase = PAIR_IDLE;

// ---------------------------------------------- CAMERA SOURCE OF TRUTH
//
// ONE base camera pose per XR pair. Both eyes of a stereo pair compose onto
// THIS latched pose; nothing sampled between the two game frames can reach
// the camera.
//
// Why this exists (hardware run 2026-08-23): the F1 scheduler spans one XR
// frame across TWO game frames -- eye 0 on frame N, eye 1 on frame N+1. The
// old path re-read the game's refdef origin AND the body basis on each of
// those frames, so every change to the game camera between them was rendered
// as stereo disparity: a position change became a fake IPD (measured: 30.730
// units against a true 2.456), an orientation change became independent
// per-eye rotation. The two eyes were a SAMPLING of a moving camera rather
// than one pose plus two eye offsets.
//
// The invariant this restores, and which the stereo gate re-checks before
// every submit:
//
//   eyeOrigin[e] = latchedBase + R(latchedBasis) * (headLean +- IPD/2)
//
// Both eyes share `latchedBase` and `latchedBasis` BY CONSTRUCTION, so the
// only term that can differ between them is the runtime's own eye offset.
// A controller event, a steering injection, a body-yaw rebase or a collision
// -hook write occurring between the eyes is now structurally incapable of
// changing eye separation: it lands on the NEXT pair's latch, moving both
// eyes together, which is ordinary camera motion rather than divergence.
static float g_baseLatchOrigin[3];
static float g_baseLatchAxis[9];
static bool  g_baseLatchValid;
static unsigned g_baseLatchCount;      // pairs that latched a fresh base
static unsigned g_baseLatchReuseCount; // eye 1s that reused the latch

// Inter-eye distance the composition actually used this pair, in metres,
// AFTER the per-eye continuity gate. The stereo fail-safe scores the rendered
// separation against this rather than against the raw located poses.
static float g_acceptedIpdM;

// Generation of the located view pair. Bumped by every successful locate and
// by session teardown, and stamped into each capture's record, so a capture
// from one generation can never be submitted inside another -- not even
// across a session rebuild that happens to leave a record looking valid.
static unsigned g_xrFrameSerial;

// Wall-clock moment the pair's first capture completed, for the eye-pair
// separation diagnostic (the time the game needed to produce the second eye).
static double g_pairFirstCapTime;

// Testing seam: force the next n captures to fail, so the abort paths for
// "eye capture failed" can be exercised offline (a real GL capture in the
// harness otherwise always succeeds).
static int g_testFailCaptures;

// State for the per-eye asymmetric projection override (the machinery and
// its rationale live further down, with VR_GetEyeCamera). Declared here
// because the pair machine's reset paths must disarm it.
static float         g_eyeTan[EYES][4];     // tanL, tanR, tanU, tanD per eye
static volatile LONG g_projArmedEye = -1;   // eye whose scene render is in flight
static unsigned      g_projReplaced[EYES];  // lifetime replacement counts
static bool          g_projLoggedEye[EYES];
static float         g_lastCullHalfX, g_lastCullHalfY;   // radians, for the log
static float         g_lastEyeOriginU[EYES][3];          // world units, for the log
static float         g_lastEyeAxisRows[EYES][9];         // view basis, for the log
// GL state at each eye's FIRST projection replacement -- the world pass's
// own viewport/scissor. Per eye: the 2026-08-19 run proved a shared slot
// records whatever pass loaded a matrix last, not what the world drew with.
static int           g_projGLViewport[EYES][4];
static int           g_projGLScissor[EYES][4];
static int           g_projGLScissorOn[EYES];
static int           g_projGLTargetW[EYES], g_projGLTargetH[EYES];
static bool          g_projGLStateValid[EYES];

// ------------------------------------------------------------ perf metrics
//
// With paired rendering, xrWaitFrame paces the game loop to TWO game frames
// per display interval. These counters separate the three cadences that
// matter -- game frames driven, XR frames begun, XR frames submitted with
// layers -- and measure the eye-pair separation: the wall-clock time between
// the two captures of one XR frame. If the engine really runs unthrottled
// between waits, that separation is a few milliseconds; a separation near a
// whole display interval means the engine itself cannot keep the pace.
static float    g_xrTargetHz;
static unsigned g_statSkipped, g_statDiscarded, g_statLocateFail, g_statXrErrors;
static unsigned g_statFreshTotal[EYES];
static unsigned g_statAbortedPairs;
// Stereo pairs refused at pre-submit by the native-object verdict
// (VRNH_PairPreSubmit): half-native, or PairWorld disagreement.
static unsigned g_statNativePairRejects;
static double   g_perfWindowStart;
static unsigned g_winXrFrames, g_winGameFrames, g_winSubmitted, g_winFresh[EYES];
static double   g_winSepSum, g_winSepMax;
static unsigned g_winSepCount;
static float    g_lastPresentHz, g_lastGameHz, g_lastSubmitHz, g_lastFreshHz[EYES];
static float    g_lastSepMeanMs, g_lastSepMaxMs;
static int      g_lastPairVerdict;      // VRPairVerdict
static bool     g_perfValid;
static double   g_perfNextLog;
static int      g_perfLogCount;

// Test override for the clock: freezing time makes cadence and separation
// arithmetic exactly checkable instead of a race against the wall clock.
static double g_testNow = -1.0;

double CODVR_NowSeconds()
{
    if (g_testNow >= 0.0)
        return g_testNow;
    static LARGE_INTEGER freq = { 0 };
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

void CODVR_TestSetNow(double seconds)
{
    g_testNow = seconds;
}

// ------------------------------------------- collision-hook camera invariant
//
// See CODVR_CollisionHookCameraMutationCount in codvr.h and
// RETIRED_INVALID_37_38_WEAPON_PATH in dllmain.cpp. Nothing in the shipped
// build increments this; it must read ZERO for the life of the process, and
// the stereo fail-safe refuses to submit a pair if it ever does not.
//
// It lives here rather than in dllmain.cpp so that both test binaries link
// against the same counter the fail-safe reads.
static volatile LONG g_collisionHookCameraMutationCount;

unsigned CODVR_CollisionHookCameraMutationCount()
{
    return (unsigned)InterlockedCompareExchange(
        &g_collisionHookCameraMutationCount, 0, 0);
}

void CODVR_NoteCollisionHookCameraMutation()
{
    InterlockedIncrement(&g_collisionHookCameraMutationCount);
}

void CODVR_TestResetCollisionHookCameraMutation()
{
    InterlockedExchange(&g_collisionHookCameraMutationCount, 0);
}

void VR_GetPerfStats(VRPerfStats* out)
{
    if (!out) return;
    out->xrTargetHz      = g_xrTargetHz;
    out->presentHz       = g_lastPresentHz;
    out->gameHz          = g_lastGameHz;
    out->submitHz        = g_lastSubmitHz;
    out->freshHz[0]      = g_lastFreshHz[0];
    out->freshHz[1]      = g_lastFreshHz[1];
    out->pairSepMeanMs   = g_lastSepMeanMs;
    out->pairSepMaxMs    = g_lastSepMaxMs;
    out->pairVerdict     = g_lastPairVerdict;
    out->abortedPairs    = g_statAbortedPairs;
    out->skippedFrames   = g_statSkipped;
    out->discardedFrames = g_statDiscarded;
    out->locateFailures  = g_statLocateFail;
    out->xrErrors        = g_statXrErrors;
    out->valid           = g_perfValid ? 1 : 0;
}

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

// Defined further down (frame loop / coordinate sections); the accessors
// below need them early.
static bool SaneEyePair(const XrVector3f& a, const XrVector3f& b);
static bool EyeStepOk(int e, const XrVector3f& p);
static bool FiniteVec(const XrVector3f& p);
static inline void XrToQuake(const XrVector3f& v, float out[3]);

void VR_SetWorldScale(float u) { if (u > 1.0f && u < 1000.0f) g_unitsPerMetre = u; }
float VR_WorldScale()      { return g_unitsPerMetre; }
float VR_PositionalGain()  { return g_positionalGain; }

void VR_SetHeightAdjust(float metres)
{
    if (metres >= -2.0f && metres <= 2.0f)
        g_heightAdjustM = metres;
}

void VR_SetMenuScale(float s)
{
    if (s >= 0.3f && s <= 1.2f)
        g_menuScale = s;
}

void VR_SetUiSafeArea(float scale, float safeX, float safeY)
{
    if (scale >= 0.5f && scale <= 1.0f) g_uiScale = scale;
    if (safeX >= 0.5f && safeX <= 1.0f) g_uiSafeX = safeX;
    if (safeY >= 0.5f && safeY <= 1.0f) g_uiSafeY = safeY;
}
void VR_GetUiSafeArea(float* scale, float* safeX, float* safeY)
{
    if (scale) *scale = g_uiScale;
    if (safeX) *safeX = g_uiSafeX;
    if (safeY) *safeY = g_uiSafeY;
}
float VR_UiInsetX() { return g_uiScale * g_uiSafeX; }
float VR_UiInsetY() { return g_uiScale * g_uiSafeY; }

void VR_SetMenuDistance(float m)
{
    if (m >= 0.0f && m <= 20.0f)
        g_menuDistM = m;
}

int VR_GetMenuSafeArea(float* cx, float* cy, float* vx, float* vy)
{
    if (!g_menuSafeValid)
        return 0;
    if (cx) *cx = g_menuContentHalfX;
    if (cy) *cy = g_menuContentHalfY;
    if (vx) *vx = g_menuVisHalfX;
    if (vy) *vy = g_menuVisHalfY;
    return 1;
}

int VR_GetNeutralRefs(float* yawRad, float headMidM[3])
{
    int mask = 0;
    if (g_headYawRefValid) {
        if (yawRad) *yawRad = g_headYawRef;
        mask |= 1;
    } else if (yawRad) {
        *yawRad = 0.0f;
    }
    if (g_headRefValid) {
        if (headMidM) memcpy(headMidM, g_headRefM, sizeof(g_headRefM));
        mask |= 2;
    } else if (headMidM) {
        headMidM[0] = headMidM[1] = headMidM[2] = 0.0f;
    }
    return mask;
}

int VR_GetHeadRoomMid(float outM[3])
{
    if (!g_viewsValid && !g_everLocated)
        return 0;
    // The history is refreshed from the current locate by UpdateLastSane, so
    // "last sane" IS the current position whenever the current one passed.
    float p0[3], p1[3];
    const XrVector3f* a = g_lastSaneValid[0] ? &g_lastSanePos[0] : nullptr;
    const XrVector3f* b = g_lastSaneValid[1] ? &g_lastSanePos[1] : nullptr;
    if (!a || !b)
        return 0;
    XrToQuake(*a, p0);
    XrToQuake(*b, p1);
    if (outM)
        for (int i = 0; i < 3; ++i)
            outM[i] = (p0[i] + p1[i]) * 0.5f;
    return 1;
}

void VR_GetWorldPassCoverage(int eye, int* known, int* vpOk, int* scOk)
{
    if (known) *known = 0;
    if (vpOk)  *vpOk = 0;
    if (scOk)  *scOk = 0;
    if (eye < 0 || eye >= EYES || !g_projGLStateValid[eye] || g_projGLTargetW[eye] < 2)
        return;
    if (known) *known = 1;
    if (vpOk)
        *vpOk = VRM_ViewportCovers(g_projGLViewport[eye][0], g_projGLViewport[eye][1],
                                   g_projGLViewport[eye][2], g_projGLViewport[eye][3],
                                   g_projGLTargetW[eye], g_projGLTargetH[eye]);
    if (scOk)
        *scOk = VRM_ScissorCovers(g_projGLScissorOn[eye],
                                  g_projGLScissor[eye][0], g_projGLScissor[eye][1],
                                  g_projGLScissor[eye][2], g_projGLScissor[eye][3],
                                  g_projGLTargetW[eye], g_projGLTargetH[eye]);
}

// Re-zero the headset: whatever the player is facing and wherever they are
// sitting becomes "straight ahead, centred". Essential in seated VR -- tracking
// drifts, and people start a session facing the wrong way.
//
// Only REQUESTS the relatch. The old references stay live until the next
// successful two-view locate consumes the request in VR_BeginFrameEx, so no
// frame is ever rendered against half-invalidated references. (Invalidating
// immediately let whichever eye rendered next -- half the time eye 1, which
// never latched -- draw one frame with completely uncancelled yaw: a violent
// single-eye world rotation at the exact moment the player asked for
// comfort.)
void VR_Recenter()
{
    g_relatchPending = true;
    Log("VR: recenter requested -- next good pose becomes neutral");
}

// How far the head is currently turned away from the game's aim.
// One line of the physical-aim chain: the rendered barrel (room), the
// rendered view forward (world), the yaw/pitch the barrel asks for relative
// to the body, the game's own aim state, and the resulting fire vector.
static void LogAimLine(const VRNHDebug* d, const char* tag)
{
    VRWeaponDebug wd;
    VRWeapon_GetDebug(&wd);
    float fire[9];
    VRM_AnglesToRows(wd.gameYawDeg, wd.gamePitchDeg, 0.0f, fire);
    Log("%s: pair=%u gunFwdRoom=(%+.3f %+.3f %+.3f) viewFwdWorld=(%+.3f %+.3f %+.3f) "
        "body %.1f  desired yaw %+.1f pitch %+.1f (offset vs body %+.1f)  "
        "gameAim yaw %.1f pitch %+.1f  fireVec=(%+.3f %+.3f %+.3f)  "
        "steer err %+.2f/%+.2f  decoupled %d barrel %d scripted %d",
        tag, g_xrFrameSerial,
        d->gun.barrelRoom[0], d->gun.barrelRoom[1], d->gun.barrelRoom[2],
        g_headWorldAxis[0], g_headWorldAxis[1], g_headWorldAxis[2],
        wd.bodyYawDeg, wd.targetYawDeg, wd.targetPitchDeg,
        VRM_WrapDeg(wd.targetYawDeg - wd.bodyYawDeg),
        wd.gameYawDeg, wd.gamePitchDeg,
        fire[0], fire[1], fire[2],
        VRM_WrapDeg(wd.targetYawDeg - wd.gameYawDeg), wd.targetPitchDeg - wd.gamePitchDeg,
        wd.decoupled, wd.barrelOverride, wd.scripted);
}

void VR_GetHeadAimOffset(float* yawDeg, float* pitchDeg)
{
    if (yawDeg)   *yawDeg   = g_headYawOffsetDeg;
    if (pitchDeg) *pitchDeg = g_headPitchOffsetDeg;
}

int VR_GetRenderedViewYawDeg(float* yawDeg)
{
    if (!g_headWorldValid || !yawDeg)
        return 0;
    *yawDeg = atan2f(g_headWorldAxis[1], g_headWorldAxis[0]) * 57.2957795f;
    return 1;
}

int VR_GetHeadLocalYawDeg(float* yawDeg)
{
    if (!g_headLocalYawValid || !yawDeg)
        return 0;
    *yawDeg = g_headLocalYawDeg;
    return 1;
}

int VR_TestGetPanelPose(int eye, float quat[4], float posM[3], int* pinned)
{
    if (eye < 0 || eye >= EYES)
        return 0;
    if (quat) {
        quat[0] = g_panelPose[eye].orientation.x;
        quat[1] = g_panelPose[eye].orientation.y;
        quat[2] = g_panelPose[eye].orientation.z;
        quat[3] = g_panelPose[eye].orientation.w;
    }
    if (posM) {
        posM[0] = g_panelPose[eye].position.x;
        posM[1] = g_panelPose[eye].position.y;
        posM[2] = g_panelPose[eye].position.z;
    }
    if (pinned) *pinned = g_panelPinned ? 1 : 0;
    return 1;
}

void VR_TestArmConsistencyCheck()
{
    g_consistencyArmed = true;
    g_consistencyValid = 0;
}

int VR_TestGetRenderSubmitConsistency(float posM[2], float rotDeg[2])
{
    if (!g_consistencyValid)
        return 0;
    if (posM)   { posM[0] = g_consistencyPosM[0];     posM[1] = g_consistencyPosM[1]; }
    if (rotDeg) { rotDeg[0] = g_consistencyRotDeg[0]; rotDeg[1] = g_consistencyRotDeg[1]; }
    return 1;
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

// Old single-size query, kept for existing callers: answers for eye 0.
void VR_EyeTextureSize(int* w, int* h)
{
    if (w) *w = g_capSizeW[0];
    if (h) *h = g_capSizeH[0];
}

void VR_EyeTextureSizeFor(int eye, int* w, int* h)
{
    const int e = (eye >= 0 && eye < EYES) ? eye : 0;
    if (w) *w = g_capSizeW[e];
    if (h) *h = g_capSizeH[e];
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
    if (w) *w = g_capSizeW[0];
    if (h) *h = g_capSizeH[0];
    return g_capSizeW[0] > 0 && g_capSizeH[0] > 0;
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

// ------------------------------------------------- roll-stable composition
//
// ComposeAxis applies the head rotation INSIDE the game's frame. That is fine
// until the game frame is pitched: yawing the head then rotates about a
// tilted axis and the horizon rolls -- asin(sin(headYaw) x sin(gamePitch)),
// about 14.5 degrees at 30/30, an uncommanded roll the inner ear never felt.
//
// The stable model decomposes the game's aim into yaw / pitch / roll and
// rebuilds the view as (column order)
//
//     view = YAW(game) x HEAD(room) x PITCH(game) x ROLL(game)
//
// so the head's own yaw (a rotation about gravity in room space) always
// composes with the game's yaw about gravity, and the game's pitch is applied
// about the resulting LATERAL axis. Every combination of mouse pitch and head
// yaw now keeps the horizon level; with the head centred the result is
// EXACTLY the game's aim (the decomposition round-trips); genuine physical
// head roll still passes straight through.
//
// The one behaviour given up: with the head turned far to the side, the
// game's pitch tilts the view about the VIEW's lateral axis rather than the
// weapon's, so during a big glance the gun's pitch plane and the view's
// disagree slightly. That is cosmetic during free-look; the rolling horizon
// was not.

// Quake row bases (rows = forward/left/up in the parent frame).
static void RowsYaw(float yawRad, float rows[9])
{
    const float c = cosf(yawRad), s = sinf(yawRad);
    rows[0]=c;  rows[1]=s;  rows[2]=0;
    rows[3]=-s; rows[4]=c;  rows[5]=0;
    rows[6]=0;  rows[7]=0;  rows[8]=1;
}
static void RowsPitch(float pitchUpRad, float rows[9])
{
    const float c = cosf(pitchUpRad), s = sinf(pitchUpRad);
    rows[0]=c;  rows[1]=0;  rows[2]=s;
    rows[3]=0;  rows[4]=1;  rows[5]=0;
    rows[6]=-s; rows[7]=0;  rows[8]=c;
}
static void RowsRoll(float rollRad, float rows[9])
{
    const float c = cosf(rollRad), s = sinf(rollRad);
    rows[0]=1;  rows[1]=0;  rows[2]=0;
    rows[3]=0;  rows[4]=c;  rows[5]=s;
    rows[6]=0;  rows[7]=-s; rows[8]=c;
}

// Split an orthonormal Quake basis into yaw/pitch/roll such that
// yaw*pitch*roll rebuilds it exactly (standard AngleVectors factorisation).
static void ExtractYawPitchRoll(const float axis[9], float* yaw, float* pitch, float* roll)
{
    float fz = axis[2];
    if (fz >  1.0f) fz =  1.0f;
    if (fz < -1.0f) fz = -1.0f;
    *pitch = asinf(fz);

    // Looking straight up or down the forward vector carries no yaw; recover
    // it from the left vector instead. (CoD clamps pitch well short of this;
    // the branch exists so the math cannot blow up on a degenerate input.)
    if (fabsf(fz) > 0.9999f) {
        *yaw  = atan2f(-axis[3], axis[4]);
        *roll = 0.0f;
        return;
    }
    *yaw = atan2f(axis[1], axis[0]);

    const float cy = cosf(*yaw),  sy = sinf(*yaw);
    const float cp = cosf(*pitch), sp = sinf(*pitch);
    const float l0[3] = { -sy, cy, 0.0f };            // no-roll left at this yaw/pitch
    const float u0[3] = { -sp * cy, -sp * sy, cp };   // no-roll up
    const float* l = &axis[3];
    *roll = atan2f(l[0]*u0[0] + l[1]*u0[1] + l[2]*u0[2],
                   l[0]*l0[0] + l[1]*l0[1] + l[2]*l0[2]);
}

static void ComposeViewOrientation(const float headLocal[9], const float gameAxis[9],
                                   float out[9])
{
    float yaw = 0, pitch = 0, roll = 0;
    ExtractYawPitchRoll(gameAxis, &yaw, &pitch, &roll);

    float rYaw[9], rPitch[9], rRoll[9], t1[9], t2[9];
    RowsYaw(yaw, rYaw);
    RowsPitch(pitch, rPitch);
    RowsRoll(roll, rRoll);

    // ComposeAxis(A, B) applies A within B's frame (columns: B x A), so the
    // column product YAW x HEAD x PITCH x ROLL builds inner-first:
    ComposeAxis(rRoll, rPitch, t1);      // PITCH x ROLL
    ComposeAxis(t1, headLocal, t2);      // HEAD x PITCH x ROLL
    ComposeAxis(t2, rYaw, out);          // YAW x HEAD x PITCH x ROLL
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

void VR_TestComposeView(const float headLocal[9], const float gameAxis[9], float out[9])
{
    ComposeViewOrientation(headLocal, gameAxis, out);
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

    // Each view keeps its OWN recommended size. Quest happens to recommend the
    // same for both, but that is the runtime's choice to make, not ours.
    for (int i = 0; i < EYES; ++i) {
        g_eyeSizeW[i] = vv[i].recommendedImageRectWidth;
        g_eyeSizeH[i] = vv[i].recommendedImageRectHeight;
    }
    const int64_t format = PickSwapchainFormat();
    Log("VR: per-eye %ux%u / %ux%u, swapchain format 0x%llX",
        g_eyeSizeW[0], g_eyeSizeH[0], g_eyeSizeW[1], g_eyeSizeH[1], (long long)format);

    for (int i = 0; i < EYES; ++i) {
        XrSwapchainCreateInfo sci = {};
        sci.type        = XR_TYPE_SWAPCHAIN_CREATE_INFO;
        sci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT
                        | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sci.format      = format;
        sci.sampleCount = 1;
        sci.width       = g_eyeSizeW[i];
        sci.height      = g_eyeSizeH[i];
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

// ------------------------------------------------------- teardown / rebuild
//
// Startup is transactional: either the session, its reference space and both
// swapchains all exist, or none of them do. Any failure after xrCreateSession
// lands here, as do session loss and persistent frame-loop failures, so the
// ordinary retry path can rebuild from a clean slate. The old scheme kept a
// half-built session alive forever: "g_session != NULL, return true" made
// every later startup call vouch for a session with no swapchains.

static void DestroySwapchainsOnly()
{
    for (int i = 0; i < EYES; ++i) {
        if (g_swapchain[i] != XR_NULL_HANDLE) {
            xrDestroySwapchain(g_swapchain[i]);
            g_swapchain[i] = XR_NULL_HANDLE;
        }
        delete[] g_images[i];
        g_images[i] = nullptr;
        g_imageCount[i] = 0;
    }
}

// Defined with the frame loop below; teardown needs them first. Teardown
// closes any open XR frame WITHOUT failure escalation (noteFailures=false),
// because the failure counter is what calls teardown -- escalating from
// inside it would recurse.
static void EndOpenFrameEmpty(bool noteFailures);
static void ResetPairState();

static void TeardownSession(const char* why)
{
    if (g_session == XR_NULL_HANDLE && g_space == XR_NULL_HANDLE)
        return;
    Log("VR: tearing down session (%s)", why);

    // No synthetic key or mouse button may outlive the session that was
    // driving it -- a held MOUSE1 with no more updates coming would fire
    // forever at the OS level.
    VRBinds_ReleaseAllInjectedInputs("session teardown");

    // A pair may be mid-flight. End the begun frame first so every
    // successful xrBeginFrame still gets exactly one xrEndFrame, then reset
    // the pair machine; the serial bump below makes any record that somehow
    // survives unmatchable against a future frame -- a fresh eye is never
    // carried across session recreation.
    if (g_frameBegun && g_sessionRunning)
        EndOpenFrameEmpty(false);
    g_frameBegun = false;
    ResetPairState();
    ++g_xrFrameSerial;

    // Action spaces are session children; destroy them before the session so
    // nothing dangles. The action set itself survives with the instance.
    VRInput_OnSessionTeardown();

    DestroySwapchainsOnly();
    if (g_space != XR_NULL_HANDLE)   { xrDestroySpace(g_space);     g_space = XR_NULL_HANDLE; }
    if (g_session != XR_NULL_HANDLE) { xrDestroySession(g_session); g_session = XR_NULL_HANDLE; }

    g_sessionComplete = false;
    g_sessionRunning  = false;
    g_sessionState    = XR_SESSION_STATE_UNKNOWN;
    g_frameBegun      = false;
    g_viewsValid      = false;
    g_everLocated     = false;
    g_panelPinned     = false;
    g_relatchPending  = true;     // a new session means a new LOCAL space
    g_headRefValid    = false;
    g_headYawRefValid = false;
    g_lastSaneValid[0] = g_lastSaneValid[1] = false;
    g_eyeRecord[0].valid = g_eyeRecord[1].valid = false;
    g_eyeTexValid[0] = g_eyeTexValid[1] = false;
    g_xrConsecutiveFailures = 0;
    g_lastRetryTick = GetTickCount();   // rebuild goes through the retry throttle
    sprintf_s(g_status, "session down (%s)", why);
}

static void TeardownInstance(const char* why)
{
    TeardownSession(why);
    VRInput_OnInstanceTeardown();     // actions/set are instance children
    if (g_instance != XR_NULL_HANDLE) {
        xrDestroyInstance(g_instance);
        g_instance = XR_NULL_HANDLE;
    }
    g_systemId = XR_NULL_SYSTEM_ID;
}

// A hard failure in the per-frame XR path. One is a transient; a steady
// stream means no retry of the same call will ever succeed, so rebuild.
static void NoteXrFailure(const char* what, XrResult r)
{
    ++g_statXrErrors;
    ++g_xrConsecutiveFailures;
    static int logged = 0;
    if (logged < 5) {
        ++logged;
        Log("VR: %s failed -> %s (%u total xr errors)", what, ResStr(r), g_statXrErrors);
    }
    if (g_xrConsecutiveFailures >= XR_FAILURE_TEARDOWN_THRESHOLD)
        TeardownSession("persistent XR failures");
}

static void NoteXrHealthy()
{
    g_xrConsecutiveFailures = 0;
}

// ---------------------------------------------------------------- startup

bool VR_Startup(HDC hdc, HGLRC glrc)
{
    if (g_startupFailed || g_userExited)
        return false;
    if (g_session != XR_NULL_HANDLE) {
        if (g_sessionComplete)
            return true;
        // Should be impossible with transactional startup; never vouch for a
        // half-built session.
        TeardownSession("incomplete session found at startup");
        return false;
    }
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
        if (XR_SUCCEEDED(xrGetInstanceProperties(g_instance, &ip))) {
            Log("VR: runtime %s", ip.runtimeName);
            VRInput_SetRuntimeName(ip.runtimeName);
        }

        // Controller actions are instance children: build them once, here.
        VRInput_OnInstanceCreated((unsigned long long)g_instance);
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

    // Ask the runtime which blend modes it supports, in ITS preference order,
    // and submit the first. Hardcoding OPAQUE happened to be right for every
    // HMD but was formally a guess; an un-enumerated mode makes xrEndFrame
    // fail outright.
    {
        XrEnvironmentBlendMode modes[8];
        uint32_t n = 0;
        if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(g_instance, g_systemId,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 8, &n, modes)) && n > 0) {
            g_blendMode = modes[0];
        } else {
            g_blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            Log("VR: blend mode enumeration unavailable; assuming OPAQUE");
        }
        static bool blendLogged = false;
        if (!blendLogged) {
            blendLogged = true;
            Log("VR: environment blend mode %d", (int)g_blendMode);
        }
    }

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

    // From here on the session exists: every failure must tear it back down,
    // or a later VR_Startup would vouch for a session with missing pieces.
    XrReferenceSpaceCreateInfo rsci = {};
    rsci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;   // seated-origin
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    XrResult rs = xrCreateReferenceSpace(g_session, &rsci, &g_space);
    if (XR_FAILED(rs)) {
        Log("VR: xrCreateReferenceSpace failed -> %s", ResStr(rs));
        TeardownSession("reference space creation failed");
        return false;
    }
    // Printed so a hardware log PROVES no game-world value ever enters an
    // XrPosef: any absurd coordinate the runtime reports afterwards (the
    // 2026-08-20 run's y = -197 m) is the runtime's own origin placement.
    Log("VR: LOCAL reference space created (poseInReferenceSpace = identity)");

    if (!CreateSwapchains()) {
        TeardownSession("swapchain creation failed");
        return false;
    }

    for (int i = 0; i < EYES; ++i) { g_views[i] = {}; g_views[i].type = XR_TYPE_VIEW; }

    g_sessionComplete = true;
    sprintf_s(g_status, "session created, waiting for READY");
    Log("VR: session created");

    // Suggest bindings (first session only), create the per-session action
    // spaces, and attach the action set. A failure inside leaves controller
    // input dead but never blocks rendering.
    VRInput_OnSessionCreated((unsigned long long)g_instance, (unsigned long long)g_session);
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
    if (g_instance == XR_NULL_HANDLE)
        return;

    XrEventDataBuffer ev = {};
    for (;;) {
        ev = {};
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        if (xrPollEvent(g_instance, &ev) != XR_SUCCESS)
            break;

        if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            // The runtime itself is going away. Drop everything; the retry
            // path recreates the instance if a runtime comes back.
            Log("VR: instance loss pending -- dropping all XR objects");
            TeardownInstance("instance loss pending");
            return;                       // the instance is gone; stop polling
        }

        if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
            // Controllers woke, slept, or the runtime rebound them.
            VRInput_OnInteractionProfileChanged((unsigned long long)g_session);
            continue;
        }

        if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            const XrEventDataReferenceSpaceChangePending* r =
                (const XrEventDataReferenceSpaceChangePending*)&ev;
            if (r->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                // The runtime moved OUR space under us (the user recentred
                // from the runtime's own UI). Every latched neutral is now
                // expressed in a coordinate frame that no longer exists.
                Log("VR: LOCAL space recentred by the runtime -- relatching neutral");
                g_relatchPending = true;
                g_lastSaneValid[0] = g_lastSaneValid[1] = false;
                g_pairFarStreak = 0;
                g_panelPinned = false;    // a pinned menu is now mis-oriented
            }
            continue;
        }

        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged* s =
                (const XrEventDataSessionStateChanged*)&ev;
            g_sessionState = s->state;
            Log("VR: session state -> %d", (int)g_sessionState);

            if (g_sessionState == XR_SESSION_STATE_READY && !g_sessionRunning) {
                XrSessionBeginInfo bi = {};
                bi.type = XR_TYPE_SESSION_BEGIN_INFO;
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(g_session, &bi))) {
                    g_sessionRunning = true;
                    sprintf_s(g_status, "running");
                    Log("VR: session begun");
                }
            } else if (g_sessionState == XR_SESSION_STATE_STOPPING) {
                // The session survives STOPPING (Virtual Desktop reconnects
                // deliver READY again on the SAME session), so only end it --
                // but close any open XR frame first (a pair may be mid-
                // flight) so wait/begin/end stays balanced, and reset the
                // pair so resumption starts cleanly at eye 0.
                EndOpenFrameEmpty(false);
                ResetPairState();
                xrEndSession(g_session);
                g_sessionRunning = false;
                VRBinds_ReleaseAllInjectedInputs("session stopping");
                sprintf_s(g_status, "stopped");
            } else if (g_sessionState == XR_SESSION_STATE_LOSS_PENDING) {
                // The session is unrecoverable. Destroy it so the ordinary
                // startup retry can build a fresh one -- keeping the handle
                // alive is what used to block reconnection until game restart.
                TeardownSession("session lost");
                if (g_session == XR_NULL_HANDLE)
                    return;               // nothing left to poll against safely
            } else if (g_sessionState == XR_SESSION_STATE_EXITING) {
                // The user asked to leave VR from the runtime. Honour it: do
                // not auto-restart; the game keeps running flat.
                g_userExited = true;
                TeardownSession("runtime requested exit");
                sprintf_s(g_status, "user exited VR");
                return;
            }
        }
    }
}

// End the currently begun XR frame with zero layers. This is what the spec
// asks for when there is nothing to show: keep the wait/begin/end cadence,
// omit the layers. noteFailures=false is for teardown and session-event
// paths, where a failing end must not feed the counter that triggers
// teardown.
static void EndOpenFrameEmpty(bool noteFailures)
{
    if (!g_frameBegun)
        return;
    XrFrameEndInfo fei = {};
    fei.type = XR_TYPE_FRAME_END_INFO;
    fei.displayTime = g_frameState.predictedDisplayTime;
    fei.environmentBlendMode = g_blendMode;
    XrResult r = xrEndFrame(g_session, &fei);
    if (XR_FAILED(r) && noteFailures) NoteXrFailure("xrEndFrame(empty)", r);
    g_frameBegun = false;
}

static void EndFrameEmpty() { EndOpenFrameEmpty(true); }

// Return the pair machine to its boundary state. A half-complete pair (eye 0
// captured, eye 1 never submitted) is counted -- a systematic abort loop
// must show up in the perf report, not vanish silently. Records are
// invalidated so nothing can borrow the dead pair's captures; the serial
// guard would catch that anyway, but a corpse nobody can trip over is
// better than a guarded one.
static void ResetPairState()
{
    if (g_pairPhase == PAIR_SECOND_EYE)
        ++g_statAbortedPairs;
    g_pairPhase = PAIR_IDLE;
    g_pairFirstCapTime = 0.0;
    g_eyeRecord[0].valid = false;
    g_eyeRecord[1].valid = false;
    // The pair is over: the next eye 0 must latch a FRESH base camera. Leaving
    // a stale latch armed would compose the new pair's eye 1 onto the dead
    // pair's base -- the exact class of cross-pair mismatch this latch exists
    // to make impossible.
    g_baseLatchValid = false;
    // No 3D scene render is in flight past a pair reset, so the projection
    // override must not stay armed into whatever renders next.
    InterlockedExchange(&g_projArmedEye, -1);
}

// Abandon the open XR frame: end it with zero layers and reset to the
// boundary, so the next frame starts a fresh pair at eye 0 with a fresh
// locate. Every mid-pair failure funnels through here; that is what keeps
// the old "one fresh / one stale" mode from coming back through an edge
// case.
static void AbortPair(const char* why)
{
    static int moans = 0;
    if (moans < 8) {
        ++moans;
        Log("VR: pair aborted (%s) -- ending XR frame with zero layers", why);
    }
    ResetPairState();
    EndFrameEmpty();
}

// Absolute position in LOCAL space is ARBITRARY: the runtime owns the origin,
// and the 2026-08-20 VDXR run parked it 197 m above the player's head. Every
// real pose failed the old |p| < 10 m absolute gate, which silently killed the
// neutral midpoint, the eye separation, the grip conversion, the steering and
// the weapon transform for the whole session. Magnitude alone can therefore
// never disqualify a sample; only non-finite garbage can. (fabsf(NaN) < x is
// false, so NaN and inf both fail this test.)
static bool FiniteVec(const XrVector3f& p)
{
    return fabsf(p.x) < 100000.0f && fabsf(p.y) < 100000.0f && fabsf(p.z) < 100000.0f;
}

// Internal-consistency gate for a stereo pair, valid at ANY origin offset:
// real tracking reports two eyes a human IPD apart. Uninitialised zeros
// (separation 0), identity poses, and one-eye teleports never look like this.
// This is how position history BOOTSTRAPS; per-frame acceptance afterwards is
// continuity against that history (EyeStepOk).
static bool SaneEyePair(const XrVector3f& a, const XrVector3f& b)
{
    if (!FiniteVec(a) || !FiniteVec(b))
        return false;
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    const float sep2 = dx * dx + dy * dy + dz * dz;
    return sep2 >= 0.030f * 0.030f && sep2 <= 0.120f * 0.120f;
}

// Per-frame, per-eye acceptance: finite and continuous with this eye's last
// accepted sample (a head cannot move 1.5 m between two locates; a tracking
// glitch or an origin jump can). "Sane" is defined by internal consistency
// and continuity, never by absolute magnitude.
static bool EyeStepOk(int e, const XrVector3f& p)
{
    if (!FiniteVec(p) || !g_lastSaneValid[e])
        return false;
    const float dx = p.x - g_lastSanePos[e].x;
    const float dy = p.y - g_lastSanePos[e].y;
    const float dz = p.z - g_lastSanePos[e].z;
    return dx * dx + dy * dy + dz * dz <= 1.5f * 1.5f;
}

// Track the last per-eye position that passed the gates, and complete a
// deferred position latch once a consistent pair has ever been seen.
static void UpdateLastSane(bool positionValid)
{
    if (positionValid) {
        const XrVector3f& p0 = g_views[0].pose.position;
        const XrVector3f& p1 = g_views[1].pose.position;
        const bool pairOk = SaneEyePair(p0, p1);
        const bool seeded = g_lastSaneValid[0] && g_lastSaneValid[1];
        if (!seeded) {
            // Bootstrap: only a geometrically consistent pair may seed the
            // history, but it may do so ANYWHERE -- the origin offset is the
            // runtime's business, ours is deltas.
            if (pairOk) {
                g_lastSanePos[0] = p0; g_lastSanePos[1] = p1;
                g_lastSaneValid[0] = g_lastSaneValid[1] = true;
                if (!g_originAnomalyLogged
                    && (fabsf(p0.x) > 10.0f || fabsf(p0.y) > 10.0f || fabsf(p0.z) > 10.0f)) {
                    g_originAnomalyLogged = true;
                    Log("VR: runtime LOCAL origin sits %.1f %.1f %.1f m from the head "
                        "(runtime quirk; our reference space was created with an "
                        "identity pose) -- harmless: every camera term is a delta "
                        "from the latched neutral", p0.x, p0.y, p0.z);
                }
            }
        } else {
            const bool step0 = EyeStepOk(0, p0);
            const bool step1 = EyeStepOk(1, p1);
            if (step0) g_lastSanePos[0] = p0;
            if (step1) g_lastSanePos[1] = p1;
            // An unannounced origin jump (no reference-space event): a
            // consistent pair, sustained well away from history, is the
            // runtime moving the world under us. Adopt it and relatch the
            // neutral rather than freezing on stale samples forever.
            if (pairOk && !step0 && !step1) {
                if (++g_pairFarStreak >= 30) {
                    g_pairFarStreak = 0;
                    g_lastSanePos[0] = p0; g_lastSanePos[1] = p1;
                    g_relatchPending = true;
                    Log("VR: tracked pose moved to a new origin with no recenter "
                        "event -- adopting it and relatching neutral");
                }
            } else {
                g_pairFarStreak = 0;
            }
        }
    }
    if (!g_headRefValid && g_headYawRefValid
        && g_lastSaneValid[0] && g_lastSaneValid[1]) {
        // The yaw latched earlier but positions were unusable at the time;
        // finish the job with the first consistent pair.
        float p0[3], p1[3];
        XrToQuake(g_lastSanePos[0], p0);
        XrToQuake(g_lastSanePos[1], p1);
        for (int i = 0; i < 3; ++i)
            g_headRefM[i] = (p0[i] + p1[i]) * 0.5f;
        g_headRefValid = true;
        Log("VR: head neutral midpoint latched late at %.3f %.3f %.3f m",
            g_headRefM[0], g_headRefM[1], g_headRefM[2]);
    }
}

// Consume a pending recenter using BOTH views from the locate that just
// succeeded. Latching here -- not in whichever eye renders next -- is what
// makes recenter atomic: no frame is ever composed against a half-latched
// reference, on either eye, in menus, or right after startup.
static void ConsumeRelatch()
{
    if (!g_relatchPending)
        return;

    // Yaw: the averaged forward of the two eyes (they are rigidly attached;
    // averaging just smooths the tiny numerical disagreement).
    float ax0[9], ax1[9];
    QuatToQuakeAxis(g_views[0].pose.orientation, ax0);
    QuatToQuakeAxis(g_views[1].pose.orientation, ax1);
    const float fx = ax0[0] + ax1[0], fy = ax0[1] + ax1[1];
    if (fabsf(fx) < 1e-6f && fabsf(fy) < 1e-6f)
        return;                       // degenerate (looking straight up); retry
    const float oldYawRef = g_headYawRef;
    const bool  oldYawRefValid = g_headYawRefValid;
    g_headYawRef = atan2f(fy, fx);
    g_headYawRefValid = true;

    // The room's "forward" just rotated by this much. The weapon layer turns
    // the virtual body WITH it, so a recenter changes what "straight ahead"
    // means without anything on screen moving.
    if (oldYawRefValid) {
        float d = g_headYawRef - oldYawRef;
        while (d >  3.14159265f) d -= 6.28318531f;
        while (d < -3.14159265f) d += 6.28318531f;
        VRWeapon_OnNeutralRelatched(d);
    }

    // Position: midpoint of the two eye positions, so neutral puts the eyes
    // at -IPD/2 and +IPD/2 around the game's camera. The current pair is
    // preferred when it is internally consistent (any origin offset); the
    // gated history covers a relatch that lands on a glitched frame.
    bool havePos = true;
    XrVector3f p[EYES];
    if (SaneEyePair(g_views[0].pose.position, g_views[1].pose.position)) {
        p[0] = g_views[0].pose.position;
        p[1] = g_views[1].pose.position;
    } else if (g_lastSaneValid[0] && g_lastSaneValid[1]) {
        p[0] = g_lastSanePos[0];
        p[1] = g_lastSanePos[1];
    } else {
        havePos = false;
    }
    if (havePos) {
        float q0[3], q1[3];
        XrToQuake(p[0], q0);
        XrToQuake(p[1], q1);
        for (int i = 0; i < 3; ++i)
            g_headRefM[i] = (q0[i] + q1[i]) * 0.5f;
        g_headRefValid = true;
    } else {
        g_headRefValid = false;       // UpdateLastSane completes it later
    }

    g_relatchPending = false;
    g_panelPinned = false;       // any pinned panel predates this reference
    Log("VR: neutral latched -- yaw %.1f deg, midpoint %s",
        g_headYawRef * 57.2957795f, havePos ? "set" : "deferred (no consistent pair yet)");
}

// Per-XR-frame perf bookkeeping: cadence measurement and the periodic
// summary. Runs at every REAL xrBeginFrame (pair boundaries only) -- never
// between the two eyes of a pair. Game frames are counted separately in
// VR_BeginFrameEx, which also runs for the no-XR-call frame in the middle of
// a pair.
static void PerfFrameTick()
{
    if (g_frameState.predictedDisplayPeriod > 0) {
        const float hz = 1.0e9f / (float)g_frameState.predictedDisplayPeriod;
        if (fabsf(hz - g_xrTargetHz) > 0.5f) {
            g_xrTargetHz = hz;
            Log("VR: XR target cadence %.1f Hz (period %.2f ms) -- one XR frame "
                "spans two game frames, so a healthy engine drives ~%.0f game "
                "frames/s and both eyes stay fresh every display interval",
                hz, (double)g_frameState.predictedDisplayPeriod / 1.0e6, hz * 2.0f);
        }
    }

    const double now = CODVR_NowSeconds();
    if (g_perfWindowStart == 0.0) {
        g_perfWindowStart = now;
        g_perfNextLog = now + 5.0;
        return;
    }
    ++g_winXrFrames;

    const double elapsed = now - g_perfWindowStart;
    if (elapsed < 5.0)
        return;

    g_lastPresentHz = (float)(g_winXrFrames / elapsed);
    g_lastGameHz    = (float)(g_winGameFrames / elapsed);
    g_lastSubmitHz  = (float)(g_winSubmitted / elapsed);
    for (int e = 0; e < EYES; ++e)
        g_lastFreshHz[e] = (float)(g_winFresh[e] / elapsed);
    g_lastSepMeanMs = g_winSepCount
                    ? (float)(g_winSepSum / (double)g_winSepCount * 1000.0) : 0.0f;
    g_lastSepMaxMs  = (float)(g_winSepMax * 1000.0);

    // Pair throughput: can the engine feed two fresh eyes per display
    // interval? Judged against 2x the XR target, never a hard-coded fps
    // figure -- 150 fps is plenty at 72 Hz and not enough at 90. Diagnostic
    // only; nothing acts on it automatically.
    if (g_xrTargetHz > 1.0f && g_winSepCount > 0) {
        if      (g_lastGameHz >= 1.9f * g_xrTargetHz) g_lastPairVerdict = VR_PAIR_PASS;
        else if (g_lastGameHz >= 1.6f * g_xrTargetHz) g_lastPairVerdict = VR_PAIR_MARGINAL;
        else                                          g_lastPairVerdict = VR_PAIR_FAIL;
    } else {
        g_lastPairVerdict = VR_PAIR_UNKNOWN;
    }

    g_perfValid = true;
    g_perfWindowStart = now;
    g_winXrFrames = g_winGameFrames = g_winSubmitted = 0;
    g_winFresh[0] = g_winFresh[1] = 0;
    g_winSepSum = g_winSepMax = 0.0;
    g_winSepCount = 0;

    // A few early reports, then one every 30 seconds -- enough for one future
    // hardware run to answer the pacing question without combing a spammy log.
    if (g_perfLogCount < 3 || now >= g_perfNextLog) {
        ++g_perfLogCount;
        g_perfNextLog = now + 30.0;
        static const char* kVerdict[4] = { "N/A", "PASS", "MARGINAL", "FAIL" };
        Log("VR perf:");
        Log("  XR target: %.0f Hz", g_xrTargetHz);
        Log("  game frames: %.1f Hz", g_lastGameHz);
        Log("  XR submitted: %.1f Hz (begun %.1f/s)", g_lastSubmitHz, g_lastPresentHz);
        Log("  fresh left: %.1f Hz", g_lastFreshHz[0]);
        Log("  fresh right: %.1f Hz", g_lastFreshHz[1]);
        Log("  mean eye-pair separation: %.2f ms", g_lastSepMeanMs);
        Log("  max eye-pair separation: %.2f ms", g_lastSepMaxMs);
        Log("  incomplete/aborted pairs: %u", g_statAbortedPairs);
        Log("  XR skips/errors: skip %u, discard %u, locateFail %u, xrErr %u",
            g_statSkipped, g_statDiscarded, g_statLocateFail, g_statXrErrors);
        Log("  PAIR THROUGHPUT: %s", kVerdict[g_lastPairVerdict & 3]);
    }
}

VRFrameDisposition VR_BeginFrameEx()
{
    if (g_session == XR_NULL_HANDLE)
        return VR_FRAME_INACTIVE;

    PumpEvents();
    if (g_session == XR_NULL_HANDLE || !g_sessionRunning)
        return VR_FRAME_INACTIVE;    // events may have torn the session down
                                     // (any mid-pair frame was closed there)

    ++g_winGameFrames;               // one call per game frame while running

    // Between the two eyes of a pair the XR frame stays OPEN: no xrWaitFrame,
    // no xrBeginFrame, no xrLocateViews, and g_frameState/g_views are
    // untouched, so the second eye renders against exactly the located view
    // pair and predicted display time the first eye used. Removing the wait
    // from this spot is the entire fix: it is the call that throttled every
    // eye to half the display rate.
    if (g_frameBegun && g_pairPhase == PAIR_SECOND_EYE)
        return VR_FRAME_RENDER;

    // A frame begun at a boundary but never submitted (the caller bailed):
    // end it empty now so wait/begin/end stays exactly paired.
    if (g_frameBegun) {
        static int moans = 0;
        if (moans < 5) {
            ++moans;
            Log("VR: begun XR frame was never submitted -- ending it empty");
        }
        EndFrameEmpty();
        ResetPairState();
    }

    g_frameState = {};
    g_frameState.type = XR_TYPE_FRAME_STATE;
    XrFrameWaitInfo fwi = {};
    fwi.type = XR_TYPE_FRAME_WAIT_INFO;
    XrResult r = xrWaitFrame(g_session, &fwi, &g_frameState);
    if (XR_FAILED(r)) {
        NoteXrFailure("xrWaitFrame", r);
        return VR_FRAME_INACTIVE;
    }

    XrFrameBeginInfo fbi = {};
    fbi.type = XR_TYPE_FRAME_BEGIN_INFO;
    r = xrBeginFrame(g_session, &fbi);
    if (XR_FAILED(r)) {
        NoteXrFailure("xrBeginFrame", r);
        return VR_FRAME_INACTIVE;
    }
    if (r == XR_FRAME_DISCARDED)
        ++g_statDiscarded;
    g_frameBegun = true;
    PerfFrameTick();

    g_viewsValid = false;
    if (!g_frameState.shouldRender) {
        // The runtime does not want a picture (headset off, dashboard up).
        // Spec guidance: keep the frame loop alive, skip the rendering work,
        // submit no layers. End the frame HERE so the ordinary submit path
        // cannot capture, blit, or push stale-pose layers for it -- and
        // reset the pair, so rendering resumes from "need eye 0".
        ++g_statSkipped;
        EndFrameEmpty();
        ResetPairState();
        return VR_FRAME_SKIP_RENDER;
    }

    XrViewLocateInfo vli = {};
    vli.type = XR_TYPE_VIEW_LOCATE_INFO;
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = g_frameState.predictedDisplayTime;
    vli.space = g_space;

    XrViewState vs = {};
    vs.type = XR_TYPE_VIEW_STATE;
    uint32_t got = 0;
    r = xrLocateViews(g_session, &vli, &vs, EYES, &got, g_views);
    const bool orientOk = XR_SUCCEEDED(r) && got == EYES
        && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
    if (!orientOk) {
        // Without orientation there is nothing sensible to render or submit.
        // (Position-only loss is NOT fatal: the per-eye last-sane fallback in
        // VR_GetEyeCamera keeps the separation alive, so a tracking blip dims
        // to rotation-only instead of flattening the world.)
        ++g_statLocateFail;
        if (XR_FAILED(r))
            NoteXrFailure("xrLocateViews", r);
        EndFrameEmpty();
        ResetPairState();
        return VR_FRAME_SKIP_RENDER;
    }

    g_lastViewStateFlags = vs.viewStateFlags;
    UpdateLastSane((vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0);
    ConsumeRelatch();

    g_viewsValid = true;
    g_everLocated = true;
    ++g_xrFrameSerial;               // a new located generation opens
    g_pairPhase = PAIR_FIRST_EYE;
    g_pairLocateTime = CODVR_NowSeconds();

    // Sync controller actions ONCE per located XR frame, at the same
    // predicted display time the views were located for. Both eyes of the
    // pair render against this single input generation -- controller poses
    // can never be stereo-inconsistent, and no extra latency is added (this
    // is the newest data the runtime has for this display time).
    VRInput_Sync((unsigned long long)g_session, (unsigned long long)g_space,
                 (long long)g_frameState.predictedDisplayTime);

    // One "xrinput:" line per pair for the first few valid pairs: the raw
    // controller state EXACTLY as the production sync produced it, so the
    // first seconds of any hardware run prove (or indict) the XR input layer
    // without anyone having to reproduce with probes.
    {
        static unsigned xrinputLines = 0;
        if (xrinputLines < 5) {
            ++xrinputLines;
            const VRInputState* is = VRInput_State();
            const VRHandState& R = is->hand[VRI_RIGHT];
            const VRHandState& L = is->hand[VRI_LEFT];
            Log("xrinput: pair=%u focused=%d sync=%s "
                "Ractive=%d RposeValid=%d Rpos=(%+.3f %+.3f %+.3f) "
                "Rquat=(%+.3f %+.3f %+.3f %+.3f) Rstick=(%+.2f,%+.2f) "
                "Rtrig=%.2f Rsq=%.2f "
                "Lactive=%d LposeValid=%d Lpos=(%+.3f %+.3f %+.3f) "
                "Lstick=(%+.2f,%+.2f) Lsq=%.2f",
                g_xrFrameSerial, is->focused, is->valid ? "ok" : "never",
                R.active, R.gripValid,
                R.gripPos[0], R.gripPos[1], R.gripPos[2],
                R.gripQuat[0], R.gripQuat[1], R.gripQuat[2], R.gripQuat[3],
                R.stickX, R.stickY, R.trigger, R.squeeze,
                L.active, L.gripValid,
                L.gripPos[0], L.gripPos[1], L.gripPos[2],
                L.stickX, L.stickY, L.squeeze);
        }
    }

    // Native hands: open this pair's object/GLSurf latches and, if enabled,
    // compute the controller-driven hand render targets. Runs exactly here --
    // once per REAL pair boundary, never on the eye-1 reuse path above --
    // which is what makes "controller sampled once per pair" a property of
    // where this call sits rather than something a caller has to remember.
    // Head pose: THIS pair's own located views, not the recentred neutral
    // reference (a viewmodel is baked relative to wherever the camera really
    // is this frame). Orientation is guaranteed valid here (checked above);
    // eye 0's orientation stands for both (a stereo rig does not rotate the
    // eyes independently), and the position is the eye midpoint.
    {
        float headAxis[9];
        QuatToQuakeAxis(g_views[0].pose.orientation, headAxis);
        float eyes[2][3];
        XrToQuake(g_views[0].pose.position, eyes[0]);
        XrToQuake(g_views[1].pose.position, eyes[1]);
        // BOTH eyes' located positions: absolute native targets are expressed
        // in the midpoint frame and each eye's own offset is subtracted at
        // the draw, so the two eyes render ONE physical object with true
        // disparity rather than one eye-relative pose painted twice.
        VRNH_OnPairBeginEyes(g_xrFrameSerial, VRInput_State(), eyes, headAxis, 1,
                             CODVR_NowSeconds());

        // palmtry: the first few hand-solve attempts, one line each, with
        // their SOURCING (buffer, generations, offsets, definedness) beside
        // the verdict -- the line that decides whether the historical
        // refusals were undefined bytes or geometry. Bounded by the layer's
        // own ring (GLS_PALMTRY_MAX), so this never becomes per-draw spam.
        {
            GLSurfPalmTry pt;
            while (GLSurf_PopPalmTry(&pt)) {
                Log("palmtry: pair=%u eye=%d owner=%s rig=%d vbo=%u gen=%u/%u "
                    "vertexOffset=0x%X indexOffset=0x%X span=%d validRange=%s "
                    "unknownBytes=%u solve=%s reason=%s site=%d rms=%.4fm shape=%.4f order=%s",
                    pt.serial, pt.eye,
                    pt.cls == GLSC_HAND_R ? "R" : pt.cls == GLSC_HAND_L ? "L" : "?",
                    pt.rig, pt.vbo, pt.gen, pt.allocGen,
                    pt.vertexOffset, pt.indexOffset, pt.span,
                    pt.validRange ? "YES" : "NO", pt.unknownBytes,
                    pt.ok ? "PASS" : "REFUSE", VRHandPose_ReasonName(pt.reason),
                    pt.site, pt.rmsM, pt.shape, pt.swapped ? "swapped" : "file");
            }
        }

        // One "nativehands:" line per pair for the first few pairs, with the
        // ENUMERATED refusal reason per hand and for the gun -- the log names
        // the first broken stage instead of a bare engaged=0.
        {
            static unsigned nhLines = 0;
            if (nhLines < 5) {
                ++nhLines;
                VRNHDebug d;
                VRNH_GetDebug(&d);
                Log("nativehands: pair=%u R{eng=%d %s} L{eng=%d %s} "
                    "gun{on=%d %s verts=%d} 2h=%d reload=%d",
                    g_xrFrameSerial,
                    d.hand[VRI_RIGHT].engaged,
                    VRNH_HandReasonName(d.hand[VRI_RIGHT].reason),
                    d.hand[VRI_LEFT].engaged,
                    VRNH_HandReasonName(d.hand[VRI_LEFT].reason),
                    d.gun.targetInstalled,
                    VRNH_GunReasonName(d.gun.reason),
                    d.gun.vertexCount, d.gun.twoHandState, d.gun.reloadState);
            }
        }

        // Early one-shot production snapshot: within the first few valid XR
        // pairs after native-hands starts (~2s), well before the ~600-pair
        // heartbeat below could fire, so a short headset session still gets
        // one line saying what engaged, solved, or refused.
        static int nhEarlyBeatDone = 0;
        if (!nhEarlyBeatDone && g_xrFrameSerial >= 120) {
            nhEarlyBeatDone = 1;
            VRNHDebug d;
            VRNH_GetDebug(&d);
            GLSurfHandSolve hr, hl;
            GLSurf_GetHandSolve(GLSC_HAND_R, &hr);
            GLSurf_GetHandSolve(GLSC_HAND_L, &hl);
            GLSurfGunSolve gs;
            GLSurf_GetGunSolve(&gs);
            Log("nativehands: early pair %u  R{eng %d %s trk %d solve %u/%u rig %d}  "
                "L{eng %d %s trk %d solve %u/%u rig %d}  gun{on %d %s verts %d "
                "solve %u/%u gripErr %.3fu}  2h %d t=%.2f  reload st %d taps %u",
                g_xrFrameSerial,
                d.hand[VRI_RIGHT].engaged,
                VRNH_HandReasonName(d.hand[VRI_RIGHT].reason),
                d.hand[VRI_RIGHT].trackState,
                hr.solves, hr.refusals, hr.lastRig,
                d.hand[VRI_LEFT].engaged,
                VRNH_HandReasonName(d.hand[VRI_LEFT].reason),
                d.hand[VRI_LEFT].trackState,
                hl.solves, hl.refusals, hl.lastRig,
                d.gun.targetInstalled, VRNH_GunReasonName(d.gun.reason),
                d.gun.vertexCount,
                gs.solves, gs.refusals, gs.rearGripErrU,
                d.gun.twoHandState, d.gun.supportT,
                d.gun.reloadState, d.gun.reloadTaps);
        }

        // Production diagnostics: one line every ~600 pairs, so a headset
        // run's log says what engaged, what solved and what refused without
        // any probe arming.
        static unsigned nhBeat = 0;
        if (g_xrFrameSerial >= nhBeat + 600) {
            nhBeat = g_xrFrameSerial;
            VRNHDebug d;
            VRNH_GetDebug(&d);
            GLSurfHandSolve hr, hl;
            GLSurf_GetHandSolve(GLSC_HAND_R, &hr);
            GLSurf_GetHandSolve(GLSC_HAND_L, &hl);
            GLSurfGunSolve gs;
            GLSurf_GetGunSolve(&gs);
            Log("nativehands: pair %u  R{eng %d %s trk %d solve %u/%u rig %d}  "
                "L{eng %d %s trk %d solve %u/%u rig %d}  gun{on %d %s verts %d "
                "solve %u/%u gripErr %.3fu}  2h %d t=%.2f  reload st %d taps %u",
                g_xrFrameSerial,
                d.hand[VRI_RIGHT].engaged,
                VRNH_HandReasonName(d.hand[VRI_RIGHT].reason),
                d.hand[VRI_RIGHT].trackState,
                hr.solves, hr.refusals, hr.lastRig,
                d.hand[VRI_LEFT].engaged,
                VRNH_HandReasonName(d.hand[VRI_LEFT].reason),
                d.hand[VRI_LEFT].trackState,
                hl.solves, hl.refusals, hl.lastRig,
                d.gun.targetInstalled, VRNH_GunReasonName(d.gun.reason),
                d.gun.vertexCount,
                gs.solves, gs.refusals, gs.rearGripErrU,
                d.gun.twoHandState, d.gun.supportT,
                d.gun.reloadState, d.gun.reloadTaps);
            // Which draw shapes are refusing, and how near the nearest
            // landmark table came: the split that turns an opaque "5522
            // refusals" (the 2026-08-28 hardware run) into a named batch.
            if (hr.refusals || hl.refusals) {
                GLSurfRefuseBucket rb[GLS_REFUSE_BUCKETS];
                unsigned ovf = 0;
                const int nrb = GLSurf_GetRefuseHisto(GLSC_HAND_R, rb, &ovf);
                char buf[240] = { 0 };
                int off = 0;
                for (int i = 0; i < nrb && off < 200; ++i)
                    off += sprintf_s(buf + off, sizeof(buf) - off,
                                     " (idx=%d span=%d x%u)",
                                     rb[i].count, rb[i].span, rb[i].hits);
                // shape is the rigid-invariant signature: if lastOk and
                // lastRef agree, the refusing draws are the SAME geometry read
                // with a wrong correspondence; if they differ, the bytes are a
                // different point set. That one comparison partitions the
                // remaining hypotheses without another run.
                Log("nativehands: palm refusals R idx/base/solve/bytesUnknown "
                    "%u/%u/%u/%u  L %u/%u/%u/%u  "
                    "lastRef{site %d %s idx=%d span=%d bestRms %.4fm shape %.4f "
                    "vbo=%u off=%u gen=%u/%u unknownBytes=%u}  "
                    "lastOk{shape %.4f vbo=%u off=%u gen=%u}%s%s",
                    hr.refusedIdx, hr.refusedBase, hr.refusedSolve,
                    hr.refusedUnknownBytes,
                    hl.refusedIdx, hl.refusedBase, hl.refusedSolve,
                    hl.refusedUnknownBytes,
                    hr.lastRefSite, VRHandPose_ReasonName(hr.lastRefReason),
                    hr.lastRefCount, hr.lastRefSpan,
                    hr.lastRefBestRmsM, hr.lastRefShape,
                    hr.lastRefBuffer, hr.lastRefOffset,
                    hr.lastRefGen, hr.lastRefAllocGen, hr.lastRefUnknownBytes,
                    hr.lastOkShape, hr.lastOkBuffer, hr.lastOkOffset, hr.lastOkGen,
                    buf, ovf ? " +ovf" : "");
            }
            // The pair verdict, low rate: how many stereo pairs were judged,
            // refused, and why -- the counters that say whether both eyes
            // reconstruct the same objects in real F1 order.
            {
                VRNHPairStats ps;
                VRNH_GetPairStats(&ps);
                Log("nativehands: pairs judged %u accepted %u rejected %u "
                    "(halfNative %u disagree %u) cooldown %u  bothApplied "
                    "R %u L %u gun %u  lastErr R %.3fmm/%.3fdeg L %.3fmm/%.3fdeg",
                    ps.checked, ps.accepted, ps.rejected,
                    ps.rejectHalfNative, ps.rejectDisagree, ps.cooldownPairs,
                    ps.objBothApplied[VRNH_OBJ_R], ps.objBothApplied[VRNH_OBJ_L],
                    ps.objBothApplied[VRNH_OBJ_GUN],
                    d.hand[VRI_RIGHT].pairPosErrU * 25.4f, d.hand[VRI_RIGHT].pairRotErrDeg,
                    d.hand[VRI_LEFT].pairPosErrU * 25.4f, d.hand[VRI_LEFT].pairRotErrDeg);
            }
            // The other two blind spots the 2026-08-28 log had: whether the
            // merged batch is actually being SPLIT (a rejected split fails
            // closed silently), and whether the gun's authored baseline is
            // alive (its "solve 0/0" is healthy while no target is installed,
            // which read as broken).
            // "Did a delta actually REACH GL" -- the question engagement and
            // solve counts cannot answer, and the one that separates "never
            // engaged" from "engaged but never rendered".
            Log("nativehands: applied R %u  L %u  gun %u transformed / %u "
                "vanilla   split %u verified / %u rejected   "
                "gun baseline{haveBody %d serial %u scene %u} scene %u",
                hr.deltasApplied, hl.deltasApplied,
                gs.batchesTransformed, gs.batchesVanilla,
                GLSurf_SplitVerifiedCount(), GLSurf_SplitRejectedCount(),
                gs.haveBody, gs.bodySerial, gs.bodyScene,
                GLSurf_SceneFrameCount());
            // WEAPON OWNERSHIP per eye-scene (brief 2026-09-04, sections
            // 3-4): every gun batch's disposition, the pre-key policy's
            // measured deviation, and the per-weapon table.
            Log("nativehands: gun scenes{model %d fresh %u partKeyed %u partRefused %u "
                "stale %u suppressed %u "
                "vanilla %u conflicts %u staleScenes %u staleErrMax %.3fu/%.3fdeg "
                "keySurf %d/%dv keyMisses %u} prekeyPolicy %d",
                gs.lastVertexCount, gs.batchesTransformed, gs.batchesPartKeyed,
                gs.batchesPartRefused, gs.batchesStale,
                gs.batchesSuppressed, gs.batchesVanilla, gs.ownerConflicts,
                gs.staleScenes, gs.staleErrMaxU, gs.staleErrMaxDeg,
                gs.keySurface, gs.keyBatchVerts, gs.keyMisses, GLSurf_GunPreKeyPolicy());
            {
                // TWO-HAND (brief 2026-09-04, section 14): low-rate telemetry.
                float gr, rl, sl, bl;
                VRNH_GetTwoHandTuning(&gr, &rl, &sl, &bl);
                Log("twohand: state %d grab %.1fcm t %.2f resid %.1fcm ang %.2fdeg "
                    "attach %u detach %u last %s  tuning{grab %.0fcm release %.0fcm slide +/-%.0fcm blend %.2fs}",
                    d.gun.twoHandState, d.gun.grabDistanceU * 2.54f, d.gun.supportT,
                    d.gun.supportResidualU * 2.54f, d.gun.angularResidualDeg,
                    d.gun.attachCount, d.gun.detachCount,
                    VRNH_TwoHandDetachName(d.gun.lastDetachReason),
                    gr * 100.0f, rl * 100.0f, sl * 100.0f, bl);
            }
            {
                // SLEEVES (brief 2026-09-04, P1): how each sleeve half was placed.
                GLSurfSleeveStats ss;
                GLSurf_GetSleeveStats(&ss);
                Log("nativehands: sleeves{forearm %u palm-fallback %u hiddenNearHead %u "
                    "hiddenByPolicy %u solveRefusals %u lastRms %.4fm "
                    "lastElbowEye (%.1f %.1f %.1f)u} anatomical %d visible %d",
                    ss.followedForearm, ss.followedPalm, ss.hiddenNearHead,
                    ss.hiddenByPolicy, ss.solveRefusals, ss.lastRmsM, ss.lastElbowEye[0],
                    ss.lastElbowEye[1], ss.lastElbowEye[2], GLSurf_SleeveAnatomical(),
                    GLSurf_SleeveVisible());
            }
            for (int wi = 0; wi < GLSurf_WeaponStatsCount(); ++wi) {
                GLSurfWeaponStats ws;
                if (!GLSurf_GetWeaponStats(wi, &ws)) continue;
                Log("nativehands: weapon key %d: scenes %u draws %u fresh %u partKeyed %u "
                    "stale %u suppressed %u vanilla %u conflicts %u",
                    ws.modelKey, ws.scenes, ws.draws, ws.native, ws.partKeyed, ws.stale,
                    ws.suppressed, ws.vanilla, ws.conflicts);
            }
            for (int e = 0; e < 2; ++e) {
                GLSurfGunScene sc;
                if (!GLSurf_GetGunScene(e, &sc) || !sc.weaponDrawSeen) continue;
                char buf[256]; int n = 0;
                for (int k = 0; k < sc.nDisp && n < 220; ++k) {
                    const char* d = sc.disp[k] == GLS_GDISP_NATIVE ? "F"
                                  : sc.disp[k] == GLS_GDISP_PARTKEY ? "P"
                                  : sc.disp[k] == GLS_GDISP_STALE ? "S"
                                  : sc.disp[k] == GLS_GDISP_SUPPRESSED ? "X" : "V";
                    n += snprintf(buf + n, sizeof(buf) - n, "%s%d:%s",
                                  k ? " " : "", sc.dispVerts[k], d);
                }
                buf[n] = 0;
                Log("nativehands: gun eye%d scene %u model %d [%s] conflict %d "
                    "staleErr %.3fu/%.3fdeg", e, sc.scene, sc.modelKey, buf,
                    sc.weaponOwnerConflict, sc.staleErrU, sc.staleErrDeg);
            }
            // ONE VISUAL OWNER (brief 2026-09-03, section 3): the identity
            // and ownership counters. orderSwapped is the engine packing the
            // hand surfaces the other way round -- the 2026-09-03 root cause,
            // now a solved identity instead of a refusal.
            {
                GLSurfOwnerStats os;
                GLSurf_GetOwnerStats(&os);
                VRNHPairStats ps;
                VRNH_GetPairStats(&ps);
                Log("ownership: primaryNativeDraw %u primaryVanillaFallbackDraw %u "
                    "duplicateOwnerPrevented %u | pairNative %u pairFailClosed %u "
                    "mixedEyeRejected %u vanillaFallback aborted %u accepted %u | "
                    "order normal %u swapped %u unknown %u | single content %u "
                    "ordinal %u | sleeve proximity %u ordinal %u | swappedSolves R %u L %u",
                    os.primaryNativeDraw, os.primaryVanillaFallbackDraw,
                    os.duplicateOwnerPrevented,
                    ps.pairNative, ps.pairFailClosed, ps.mixedEyeRejected,
                    ps.vanillaFallbackAborted, ps.vanillaFallbackAccepted,
                    os.orderNormal, os.orderSwapped, os.orderUnknown,
                    os.singleByContent, os.singleByOrdinal,
                    os.sleeveByProximity, os.sleeveByOrdinal,
                    hr.swappedSolves, hl.swappedSolves);
            }
            LogAimLine(&d, "aim(beat)");
        }
        // aim: the first few pairs after the physical barrel starts driving
        // the game's aim (brief section 18): gun forward, view forward, the
        // desired yaw/pitch offset, the game's aim state, the fire vector.
        {
            static unsigned aimLines = 0;
            VRNHDebug d;
            VRNH_GetDebug(&d);
            if (d.gun.barrelValid && VRWeapon_BarrelOverrideActive() && aimLines < 10) {
                ++aimLines;
                LogAimLine(&d, "aim");
            }
        }
    }
    return VR_FRAME_RENDER;
}

bool VR_BeginFrame()
{
    return VR_BeginFrameEx() == VR_FRAME_RENDER;
}

int VR_ExpectedEye()
{
    return g_pairPhase == PAIR_SECOND_EYE ? 1 : 0;
}

// -------------------------------------- per-eye asymmetric projection
//
// The optical architecture (mirroring q3vr / RTCWQuest / hello_xr):
//
//   runtime XrView[eye].fov  ->  REAL asymmetric GL projection for that eye
//                            ->  render the eye through it
//                            ->  submit the FULL image with that same fov.
//
// refdef's symmetric fov cannot express an asymmetric lens, so it is used
// for CULLING ONLY (a symmetric superset). The engine still computes and
// loads its own symmetric perspective matrix from it -- and the moment it
// does, the opengl32 wrapper hands the matrix here and it is replaced with
// the armed eye's true frustum, keeping the engine's own depth row so
// near/far conventions are untouched.
//
// TIMING: the render syscall only QUEUES the scene; the engine executes its
// GL command buffer AFTER the syscall returns and BEFORE wglSwapBuffers. The
// armed window therefore spans hook -> submit (VR_GetEyeCamera arms it,
// VR_SubmitFrame / any pair reset disarms it), never just the syscall.
// (State lives with the pair machine above, whose resets disarm it.)

static void ArmProjection(int eye)
{
    InterlockedExchange(&g_projArmedEye, (LONG)eye);
}

static void DisarmProjection()
{
    InterlockedExchange(&g_projArmedEye, -1);
}

int VR_ProjectionArmedEye()
{
    const LONG e = g_projArmedEye;
    return (e >= 0 && e < EYES) ? (int)e : -1;
}

bool CODVR_MaybeReplaceProjection(float m[16])
{
    const LONG eye = g_projArmedEye;
    if (eye < 0 || eye >= EYES)
        return false;
    if (!VRM_IsPerspective(m))
        return false;

    const float tL = g_eyeTan[eye][0], tR = g_eyeTan[eye][1];
    const float tU = g_eyeTan[eye][2], tD = g_eyeTan[eye][3];
    if (!(tR > tL) || !(tU > tD))
        return false;               // no valid armed frustum

    float repl[16];
    VRM_ReplaceLateralOptics(m, tL, tR, tU, tD, repl);

    if (!g_projLoggedEye[eye]) {
        g_projLoggedEye[eye] = true;
        float n = 0.0f, f = 0.0f;
        VRM_ExtractNearFar(m, &n, &f);
        Log("VR: eye %d 3D projection intercepted (engine near %.2f, far %s%.1f)",
            (int)eye, n, f <= 0.0f ? "infinite " : "", f);
        Log("VR:   engine m0/m5/m8/m9 = %.4f %.4f %.4f %.4f, depth m10/m14 = %.4f %.2f",
            m[0], m[5], m[8], m[9], m[10], m[14]);
        Log("VR:   ours   m0/m5/m8/m9 = %.4f %.4f %.4f %.4f (tan L%.4f R%.4f U%.4f D%.4f), depth kept",
            repl[0], repl[5], repl[8], repl[9], tL, tR, tU, tD);
    }

    memcpy(m, repl, 16 * sizeof(float));
    ++g_projReplaced[eye];
    return true;
}

void CODVR_NoteProjectionGLState(int eye, const int viewport[4],
                                 const int scissor[4], int scissorEnabled)
{
    if (eye < 0 || eye >= EYES)
        return;
    memcpy(g_projGLViewport[eye], viewport, sizeof(g_projGLViewport[eye]));
    memcpy(g_projGLScissor[eye], scissor, sizeof(g_projGLScissor[eye]));
    g_projGLScissorOn[eye] = scissorEnabled;
    // Coverage must be judged against the target this frame actually
    // rendered into: the first mission frame draws before the supersampled
    // FBO exists, and comparing its window-sized viewport against the FBO
    // would report a phantom coverage failure.
    const bool fbo = g_sceneBound && g_sceneFbo && !g_sceneFailed;
    g_projGLTargetW[eye] = fbo ? g_sceneW : 0;
    g_projGLTargetH[eye] = fbo ? g_sceneH : 0;
    g_projGLStateValid[eye] = true;
}

void VR_GetHudPrincipalShift(int eye, int viewW, int viewH, int* dxPx, int* dyPx)
{
    if (dxPx) *dxPx = 0;
    if (dyPx) *dyPx = 0;
    if (eye < 0 || eye >= EYES || viewW < 2 || viewH < 2 || !dxPx || !dyPx)
        return;
    const float tL = g_eyeTan[eye][0], tR = g_eyeTan[eye][1];
    const float tU = g_eyeTan[eye][2], tD = g_eyeTan[eye][3];
    if (!(tR > tL) || !(tU > tD))
        return;                     // no 3D eye driven yet
    VRM_PrincipalPointShiftPx(tL, tR, tU, tD, viewW, viewH, dxPx, dyPx);
}

void VR_SetPositionalGain(float gain)
{
    if (gain >= 0.0f && gain <= 2.0f)
        g_positionalGain = gain;
}

void VR_SetCullMargin(float deg)
{
    if (deg >= 0.0f && deg <= 20.0f)
        g_cullMarginDeg = deg;
}

void VR_TestArmProjection(int eye, float tanL, float tanR, float tanU, float tanD)
{
    if (eye < 0 || eye >= EYES)
        return;
    g_eyeTan[eye][0] = tanL;
    g_eyeTan[eye][1] = tanR;
    g_eyeTan[eye][2] = tanU;
    g_eyeTan[eye][3] = tanD;
    ArmProjection(eye);
}

void VR_TestDisarmProjection()
{
    DisarmProjection();
}

int VR_TestProjectionDecision(float m[16])
{
    return CODVR_MaybeReplaceProjection(m) ? 1 : 0;
}

unsigned VR_TestProjectionReplacedCount(int eye)
{
    return (eye >= 0 && eye < EYES) ? g_projReplaced[eye] : 0;
}

// The yaw of the base camera THIS pair was latched with, plus the latch
// serial. The steering-leak watchdog needs both: with one base latched per XR
// pair, the rendered camera changes only at pair boundaries, so a per-game-
// frame comparison against a LIVE body yaw would see the body advance while
// the camera legitimately held still and report a leak that is not there.
int VR_GetPairBaseYawDeg(float* yawDeg, unsigned* latchSerial)
{
    if (!g_baseLatchValid)
        return 0;
    if (yawDeg)
        *yawDeg = atan2f(g_baseLatchAxis[1], g_baseLatchAxis[0]) * 57.2957795f;
    if (latchSerial)
        *latchSerial = g_baseLatchCount;
    return 1;
}

void VR_TestGetBaseLatchCounts(unsigned* latches, unsigned* reuses)
{
    if (latches) *latches = g_baseLatchCount;
    if (reuses)  *reuses  = g_baseLatchReuseCount;
}

void VR_TestGetEyeTangents(int eye, float out[4])
{
    if (eye < 0 || eye >= EYES) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }
    memcpy(out, g_eyeTan[eye], 4 * sizeof(float));
}

void VR_GetEyeCamera(int eye,
                     const float gameOrigin_[3], const float gameAxis_[9],
                     int viewW, int viewH,
                     float outOrigin[3], float outAxis[9],
                     float* fovXdeg, float* fovYdeg)
{
    // ---- CAMERA SOURCE OF TRUTH: one base pose per XR pair ----
    //
    // Eye 0 latches whatever the game currently has; eye 1 IGNORES its
    // caller-supplied sample and reuses the latch. From here down, the
    // function cannot see the live game camera at all -- `gameOrigin` and
    // `gameAxis` are the latched pair base, and the arguments are shadowed
    // so no later edit can accidentally reach past the latch.
    //
    // Eye 1 arriving with no latch means the pair machine started mid-pair
    // (a reset between the eyes). Latching there is still correct -- it makes
    // this eye self-consistent -- and the pair's own abort accounting plus
    // the stereo gate below decide whether the result may be submitted.
    if (eye == 0 || !g_baseLatchValid) {
        memcpy(g_baseLatchOrigin, gameOrigin_, sizeof(g_baseLatchOrigin));
        memcpy(g_baseLatchAxis,   gameAxis_,   sizeof(g_baseLatchAxis));
        g_baseLatchValid = true;
        ++g_baseLatchCount;
    } else {
        ++g_baseLatchReuseCount;
    }
    const float* const gameOrigin = g_baseLatchOrigin;
    const float* const gameAxis   = g_baseLatchAxis;

    // ---- orientation: mouse aims, the head free-looks on top of it ----
    float headAxis[9];
    QuatToQuakeAxis(g_views[eye].pose.orientation, headAxis);

    // Cancel the neutral yaw latched atomically in VR_BeginFrameEx (from both
    // views of one locate; re-latched by VR_Recenter and by runtime recenter
    // events). Only yaw is cancelled -- cancelling pitch or roll would bake a
    // tilted starting posture into the horizon permanently.
    //
    // If no neutral exists yet (the first frames before any good locate), use
    // the identity head instead of the RAW head basis: the raw fallback could
    // compose the player's entire physical facing offset -- up to 180 degrees
    // -- into a single eye for a frame, a violent one-eye world rotation.
    float headLocal[9];
    if (g_headYawRefValid) {
        const float rc = cosf(-g_headYawRef), rs = sinf(-g_headYawRef);
        for (int row = 0; row < 3; ++row)
            RotateZ(rc, rs, &headAxis[row * 3], &headLocal[row * 3]);
    } else {
        memset(headLocal, 0, sizeof(headLocal));
        headLocal[0] = headLocal[4] = headLocal[8] = 1.0f;
    }

    // The game's own view basis carries the player's aim -- BOTH yaw and
    // pitch from the mouse. The roll-stable composition keeps the mouse in
    // charge of aim (head centred = exactly along the gun) while head yaw
    // always rotates about gravity, so aiming down and glancing sideways can
    // no longer tilt the horizon.
    ComposeViewOrientation(headLocal, gameAxis, outAxis);

    // Remember where the head is pointing in world terms, and by how much it
    // differs from the game's aim. The input layer steers the game toward
    // this; the comfort vignette watches how fast it changes. Stored on
    // EVERY eye render (both eyes share one orientation composition, but the
    // BODY basis can advance between the two game frames of a pair -- a
    // sample left stale on eye-1 frames made the leak watchdog compare an
    // old camera against a new body and cry wolf during stick turns).
    const float rad2deg = 57.2957795f;
    memcpy(g_headWorldAxis, outAxis, sizeof(g_headWorldAxis));
    g_headWorldValid = true;

    // The head's own yaw (neutral-cancelled), before any composition.
    g_headLocalYawDeg = atan2f(headLocal[1], headLocal[0]) * rad2deg;
    g_headLocalYawValid = true;

    // Yaw difference between the composed view and the game's own aim.
    const float viewYaw = atan2f(outAxis[1], outAxis[0]);
    const float aimYaw  = atan2f(gameAxis[1], gameAxis[0]);
    {
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
    }

    if (eye == 0) {
        // Translation speed and turn rate, for the comfort vignette. Measured
        // from the game's camera rather than the head, because it is the
        // *virtual* motion the player cannot feel that causes sickness --
        // physically turning your own head never does.
        //
        // High-resolution clock, and intervals too small to divide by are
        // ACCUMULATED rather than dropped -- GetTickCount's 15.6 ms tick used
        // to throw away most frames' movement entirely. The filters handle
        // teleport and snap-turn steps (see FilterSpeed / FilterTurn).
        const double now = CODVR_NowSeconds();
        if (g_prevMotionTime != 0.0) {
            const double dt = now - g_prevMotionTime;
            if (dt > 0.0 && dt < 0.5) {
                if (g_prevOriginValid) {
                    const float mx = gameOrigin[0] - g_prevOrigin[0];
                    const float my = gameOrigin[1] - g_prevOrigin[1];
                    const float mz = gameOrigin[2] - g_prevOrigin[2];
                    g_pendingDist += sqrtf(mx*mx + my*my + mz*mz);
                }
                if (g_prevAimYawValid) {
                    float dyaw = aimYaw - g_prevAimYaw;
                    while (dyaw >  3.14159265f) dyaw -= 6.28318531f;
                    while (dyaw < -3.14159265f) dyaw += 6.28318531f;
                    g_pendingDyawDeg += dyaw * rad2deg;
                }
                g_motionDtAccum += dt;
                if (g_motionDtAccum >= 0.004) {
                    g_playerSpeed = FilterSpeed(g_playerSpeed, g_pendingDist,
                                                (float)g_motionDtAccum);
                    g_turnRate    = FilterTurn(g_turnRate, g_pendingDyawDeg,
                                               (float)g_motionDtAccum);
                    g_motionDtAccum = 0.0;
                    g_pendingDist = 0.0f;
                    g_pendingDyawDeg = 0.0f;
                }
            } else {
                // A long stall (level load): discard the interval entirely.
                g_motionDtAccum = 0.0;
                g_pendingDist = 0.0f;
                g_pendingDyawDeg = 0.0f;
            }
        }
        g_prevMotionTime = now;
        g_prevOrigin[0] = gameOrigin[0];
        g_prevOrigin[1] = gameOrigin[1];
        g_prevOrigin[2] = gameOrigin[2];
        g_prevOriginValid = true;
        g_prevAimYaw = aimYaw;
        g_prevAimYawValid = true;
    }

    // ---- position: body camera + head-centre delta + per-eye offset ----
    //
    // Three explicitly separated contributions (never conflated):
    //   1. the game's own camera (gameOrigin) -- virtual locomotion;
    //   2. the physical HEAD-CENTRE translation: the midpoint of both located
    //      eye positions minus the latched neutral midpoint (leaning,
    //      stepping, crouching), scaled by unitsPerMetre * positionalGain and
    //      clamped to a believable range;
    //   3. this eye's OFFSET from the head centre: exactly +-IPD/2, scaled by
    //      unitsPerMetre only -- gain never touches it, so stereo depth is
    //      independent of the comfort knob, and the midpoint of the two
    //      rendered eyes always lands on the translated head centre (IPD is
    //      applied once, as an offset from the centre, never twice).
    //
    // Each eye is continuity-gated BEFORE the midpoint (a glitch once
    // teleported one eye a mile): a discontinuous sample falls back to THIS
    // EYE's last accepted sample, so one absurd eye still cannot collapse
    // the separation or drag the midpoint away. Bootstrap is the PAIR
    // consistency gate in UpdateLastSane -- absolute magnitude never
    // disqualifies (the runtime may park the LOCAL origin 197 m away).
    float pEyeM[EYES][3];
    bool  haveEye[EYES] = { false, false };
    for (int e = 0; e < EYES; ++e) {
        const XrVector3f& rawE = g_views[e].pose.position;
        const XrVector3f* use = nullptr;
        if (EyeStepOk(e, rawE)) {
            use = &rawE;
        } else if (g_lastSaneValid[e]) {
            use = &g_lastSanePos[e];
            static bool moanedGlitch = false;
            if (!moanedGlitch) {
                moanedGlitch = true;
                Log("VR: discontinuous eye %d position %.3f %.3f %.3f m -- using "
                    "its last accepted sample so the separation survives",
                    e, rawE.x, rawE.y, rawE.z);
            }
        } else {
            static bool moaned = false;
            if (!moaned) {
                moaned = true;
                Log("VR: eye %d position %.3f %.3f %.3f m with no consistent "
                    "pair seen yet -- rotation only", e, rawE.x, rawE.y, rawE.z);
            }
        }
        if (use) {
            XrToQuake(*use, pEyeM[e]);
            haveEye[e] = true;
        }
    }

    // Lean and step have to land in world space. The deltas are in room
    // space, so rotate them by the body's facing (the game's aim yaw) minus
    // the neutral yaw cancelled out of the orientation -- otherwise leaning
    // left would move the camera in the wrong direction by however far the
    // player was turned when the session started.
    const float bodyYaw = atan2f(gameAxis[1], gameAxis[0])
                        - (g_headYawRefValid ? g_headYawRef : 0.0f);
    const float byc = cosf(bodyYaw), bys = sinf(bodyYaw);

    // The separation the composition is ACTUALLY working from, after the
    // per-eye continuity gate above. The stereo fail-safe scores the rendered
    // pair against THIS, not against the raw runtime poses: the continuity
    // gate exists precisely to substitute a last-sane sample for an absurd
    // one, and a safety check that measured the raw poses would condemn the
    // very correction that saved the image. Zero when no consistent pair
    // exists yet -- that is the deliberate rotation-only fallback, where
    // coincident eyes are expected rather than corrupt.
    if (haveEye[0] && haveEye[1]) {
        const float ex = pEyeM[1][0] - pEyeM[0][0];
        const float ey = pEyeM[1][1] - pEyeM[0][1];
        const float ez = pEyeM[1][2] - pEyeM[0][2];
        g_acceptedIpdM = sqrtf(ex*ex + ey*ey + ez*ez);
    } else {
        g_acceptedIpdM = 0.0f;
    }

    float rotated[3] = { 0, 0, 0 };
    if (haveEye[0] && haveEye[1]) {
        float centreM[3], localU[3];
        for (int i = 0; i < 3; ++i)
            centreM[i] = (pEyeM[0][i] + pEyeM[1][i]) * 0.5f;

        for (int i = 0; i < 3; ++i) {
            // Head-centre translation, clamped so a tracking glitch can
            // never shove the camera through the level. Zero until the
            // neutral midpoint latches -- but the +-IPD/2 eye offset below
            // needs NO reference (it is relative to this frame's own
            // centre), so stereo separation must never wait for, or die
            // with, the neutral latch. The 2026-08-20 run rendered the
            // whole session with coincident eyes because the offset was
            // chained to the midpoint.
            float dM = 0.0f;
            if (g_headRefValid) {
                dM = centreM[i] - g_headRefM[i];
                if (dM >  2.0f) dM =  2.0f;
                if (dM < -2.0f) dM = -2.0f;
            }
            const float eyeOffM = pEyeM[eye][i] - centreM[i];   // +-IPD/2
            localU[i] = (dM * g_positionalGain + eyeOffM) * g_unitsPerMetre;
        }
        // Seated-height comfort offset: outside the gain so the knob is
        // metres regardless of gain. 0 by default = exactly the old path.
        localU[2] += g_heightAdjustM * g_unitsPerMetre;
        RotateZ(byc, bys, localU, rotated);
    }
    outOrigin[0] = gameOrigin[0] + rotated[0];
    outOrigin[1] = gameOrigin[1] + rotated[1];
    outOrigin[2] = gameOrigin[2] + rotated[2];
    memcpy(g_lastEyeOriginU[eye], outOrigin, sizeof(g_lastEyeOriginU[eye]));
    memcpy(g_lastEyeAxisRows[eye], outAxis, sizeof(g_lastEyeAxisRows[eye]));

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

    // refdef carries only a symmetric fov, while the eye's real frustum is
    // asymmetric. The refdef fov is therefore CULLING ONLY: the smallest
    // symmetric frustum containing this eye's runtime frustum, plus a safety
    // margin (the engine may recompute fov_y from the viewport aspect; the
    // margin keeps such quirks from culling geometry the real view can see;
    // overdraw is the only cost). The actual OPTICS are the runtime's exact
    // asymmetric frustum, installed over the engine's own perspective matrix
    // by CODVR_MaybeReplaceProjection -- armed here, for the scene render
    // this camera was built for.
    (void)viewW; (void)viewH;
    const XrFovf& f = g_views[eye].fov;
    g_eyeTan[eye][0] = tanf(f.angleLeft);
    g_eyeTan[eye][1] = tanf(f.angleRight);
    g_eyeTan[eye][2] = tanf(f.angleUp);
    g_eyeTan[eye][3] = tanf(f.angleDown);

    const float deg2rad = 0.01745329252f;
    float halfX = fabsf(f.angleLeft) > fabsf(f.angleRight)
                ? fabsf(f.angleLeft) : fabsf(f.angleRight);
    float halfY = fabsf(f.angleUp) > fabsf(f.angleDown)
                ? fabsf(f.angleUp) : fabsf(f.angleDown);
    halfX += g_cullMarginDeg * deg2rad;
    halfY += g_cullMarginDeg * deg2rad;
    const float halfCap = 75.0f * deg2rad;   // keep tan() sane if a runtime
    if (halfX > halfCap) halfX = halfCap;    // ever reports an extreme fov
    if (halfY > halfCap) halfY = halfCap;

    g_submitHalfX[eye] = halfX;
    g_submitHalfY[eye] = halfY;
    g_lastCullHalfX = halfX;
    g_lastCullHalfY = halfY;

    ArmProjection(eye);

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

// Per-axis scale from the engine's own screen coordinates into the scene
// target. Was one scalar while the target was square; the target now matches
// the eye aspect, so the two axes differ and every rect remap has to use both
// or the 2D pass lands in the wrong place.
void VR_SupersampleXY(float* sx, float* sy)
{
    float x = 1.0f, y = 1.0f;
    if (g_sceneBound && g_sceneBaseW > 1 && g_sceneBaseH > 1
        && g_sceneW > 1 && g_sceneH > 1) {
        x = (float)g_sceneW / (float)g_sceneBaseW;
        y = (float)g_sceneH / (float)g_sceneBaseH;
    }
    if (sx) *sx = x;
    if (sy) *sy = y;
}

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

    // Shape the target to the RUNTIME EYE aspect, at the same pixel budget.
    //
    // The engine's refdef is square (1440x1440) while the eye image is not
    // (3072x3264), so a square target had to be resampled by a different
    // factor per axis on the way out -- 3240->3072 across, 3240->3264 down.
    // That is NOT an angular distortion: every stage of this pipeline maps a
    // FULL rectangle onto a FULL rectangle, so normalised coordinates pass
    // through untouched and the submitted fov still lands exactly where the
    // projection put it. (S_AngularMapping proves that, and proves it stays
    // true for a square target.) What it does cost is sharpness -- one axis
    // is minified while the other is magnified, and the minified one throws
    // away detail the budget already paid for.
    //
    // Matching the eye aspect makes the outgoing blit a single uniform scale,
    // so the pixels are isotropic and nothing is resampled unevenly. Pixel
    // count is held to within a fraction of a percent of the square target,
    // so the frame cost does not move.
    if (g_capSizeW[0] > 1 && g_capSizeH[0] > 1) {
        const double area = (double)w * (double)h;
        const double r    = (double)g_capSizeW[0] / (double)g_capSizeH[0];
        int nh = (int)(sqrt(area / r) + 0.5);
        int nw = (int)(r * nh + 0.5);
        nw = (nw + 3) & ~3;                 // keep both axes multiple-of-4
        nh = (nh + 3) & ~3;
        if (nw > 15 && nh > 15 && nw <= 16384 && nh <= 16384) {
            w = nw; h = nh;
        }
    }

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
    Log("VR: scene target %dx%d (%.3fx / %.3fx of %dx%d; %.2f Mpx)",
        w, h, (float)w / (float)baseW, (float)h / (float)baseH,
        baseW, baseH, (float)w * (float)h / 1.0e6f);
    if (g_capSizeW[0] > 1 && g_capSizeH[0] > 1)
        Log("VR: scene aspect %.4f vs eye aspect %.4f -- outgoing blit scale "
            "%.4f x %.4f (equal = isotropic, no anisotropic stretch)",
            (float)w / (float)h,
            (float)g_capSizeW[0] / (float)g_capSizeH[0],
            (float)g_capSizeW[0] / (float)w, (float)g_capSizeH[0] / (float)h);
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
// into this eye's persistent texture. Called once per game frame, for
// whichever eye that game frame rendered (or twice, for a flat screen that
// goes to both eyes).
bool VR_CaptureEye(int eye, int baseW, int baseH, bool letterbox)
{
    if (g_testFailCaptures > 0) {
        --g_testFailCaptures;
        return false;
    }
    if (eye < 0 || eye >= EYES || baseW < 2 || baseH < 2)
        return false;
    if (!LoadGLFunctions())
        return false;

    // Hold each eye at the headset's own per-eye resolution, so the detail the
    // supersampled target produced survives all the way to the compositor
    // instead of being thrown away at the game's resolution on the way past.
    // Before a session exists that size is unknown, so fall back to the game's
    // own -- otherwise capture fails silently and the eye stays black.
    const int w = (g_eyeSizeW[eye] >= 2) ? (int)g_eyeSizeW[eye] : baseW;
    const int h = (g_eyeSizeH[eye] >= 2) ? (int)g_eyeSizeH[eye] : baseH;
    if (w < 2 || h < 2)
        return false;

    if (!g_eyeTex[eye] || w != g_capSizeW[eye] || h != g_capSizeH[eye]) {
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
        g_capSizeW[eye] = w; g_capSizeH[eye] = h;
        // Only THIS eye's content became invalid. Invalidating both (the old
        // behaviour) meant that creating eye 1's texture on its first frame
        // threw away eye 0's perfectly good capture, and the submit fell back
        // to showing the fresh eye's image -- with the fresh eye's pose -- in
        // both eyes for a frame.
        g_eyeTexValid[eye] = false;
        g_eyeRecord[eye].valid = false;
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

        // Destination: ANGULAR fit. The panel is submitted with the
        // symmetric-superset fov (max(|L|,|R|) x max(U,|D|)) but the lens
        // only shows the smaller asymmetric box (nasal 40 deg vs temporal 54
        // on Quest 3) -- the old pixel-based fit (70% height, up to 95%
        // width) pushed panel edges past the visible angles, which is the
        // hardware run's "menu clipped slightly on all four sides". Content
        // now stays inside the BINOCULARLY visible half-angles times the
        // menu safe-area scale, aspect preserved. No located fov (offline GL
        // tests, pre-session frames) keeps the old sizing.
        float ph = h * 0.70f;
        float pw = ph * srcAspect;
        if (pw > w * 0.95f) { pw = w * 0.95f; ph = pw / srcAspect; }
        const XrFovf& evf = g_views[eye].fov;
        const float tl = fabsf(tanf(evf.angleLeft)), tr = fabsf(tanf(evf.angleRight));
        const float tu = fabsf(tanf(evf.angleUp)),   td = fabsf(tanf(evf.angleDown));
        const float subX = tl > tr ? tl : tr, subY = tu > td ? tu : td;
        const float visX = tl < tr ? tl : tr, visY = tu < td ? tu : td;
        if (g_everLocated && subX > 0.05f && subY > 0.05f
            && visX > 0.05f && visY > 0.05f) {
            // ...times the VR UI safe area (P3, 2026-09-04): the panel is
            // fitted inside the per-axis safe bounds, aspect preserved,
            // centred (the centring happens below with the offsets).
            const float maxW = w * (g_menuScale * VR_UiInsetX() * visX / subX);
            const float maxH = h * (g_menuScale * VR_UiInsetY() * visY / subY);
            pw = maxW;
            ph = pw / srcAspect;
            if (ph > maxH) { ph = maxH; pw = ph * srcAspect; }
            g_menuContentHalfX = atanf((pw / (float)w) * subX) * 57.2957795f;
            g_menuContentHalfY = atanf((ph / (float)h) * subY) * 57.2957795f;
            g_menuVisHalfX = atanf(visX) * 57.2957795f;
            g_menuVisHalfY = atanf(visY) * 57.2957795f;
            g_menuSafeValid = true;
            if (fabsf(g_menuContentHalfX - g_menuFitLoggedX) > 0.25f
                || fabsf(g_menuContentHalfY - g_menuFitLoggedY) > 0.25f) {
                g_menuFitLoggedX = g_menuContentHalfX;
                g_menuFitLoggedY = g_menuContentHalfY;
                Log("VR: menu/loading panel fit: content +/-%.1f x +/-%.1f deg inside "
                    "visible +/-%.1f x +/-%.1f deg (menu scale %.2f, ui safe area %.2f x %.2f x %.2f)",
                    g_menuContentHalfX, g_menuContentHalfY, g_menuVisHalfX, g_menuVisHalfY,
                    g_menuScale, g_uiScale, g_uiSafeX, g_uiSafeY);
            }
        }

        // Optional convergence: shift each eye nasally as if the panel hung
        // g_menuDistM away. 0 (default) keeps the panel at infinity -- the
        // behavior the hardware run called "pretty good".
        int dxShift = 0;
        if (g_menuDistM > 0.0f && g_everLocated && subX > 0.05f) {
            float ipdM = 0.063f;
            const float ddx = g_views[1].pose.position.x - g_views[0].pose.position.x;
            const float ddy = g_views[1].pose.position.y - g_views[0].pose.position.y;
            const float ddz = g_views[1].pose.position.z - g_views[0].pose.position.z;
            const float d = sqrtf(ddx*ddx + ddy*ddy + ddz*ddz);
            if (d > 0.02f && d < 0.09f) ipdM = d;
            const float shiftTan = (ipdM * 0.5f) / g_menuDistM;
            dxShift = (int)(shiftTan * (float)w / (2.0f * subX) + 0.5f);
            if (eye == 1) dxShift = -dxShift;         // nasal for each eye
        }

        dx0 = (int)((w - pw) * 0.5f) + dxShift;
        dy0 = (int)((h - ph) * 0.5f);
        dx1 = dx0 + (int)pw;
        dy1 = dy0 + (int)ph;
        if (dx0 < 0) { dx1 -= dx0; dx0 = 0; }
        if (dx1 > w) { dx0 -= dx1 - w; dx1 = w; }
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

// ------------------------------------------------------ per-eye colour pass
// The custom opengl32 proxy owns rendering, so a desktop ReShade hook does
// not reliably reach OpenXR. Process the completed eye in a separate target
// instead: its UVs, dimensions, pose and projection remain exactly unchanged.
// Depth effects are intentionally absent: the engine depth buffer has weapon
// depth hacks and is not submitted as a stereo depth layer.
static VRPostprocessConfig g_postCfg = { 1, 0.35f, 0.0f, 0.035f, 1.015f, 0.02f };
static bool g_postFailNext;
static unsigned g_postProgram, g_postFbo, g_postTex[EYES];
static int g_postW[EYES], g_postH[EYES];
static int g_postSourceLoc, g_postTexelLoc, g_postAmountsLoc, g_postShadowLoc;
static bool g_postTried, g_postReady;

static float PostClamp(float x, float lo, float hi)
{
    if (!_finite(x)) return lo;
    return x < lo ? lo : x > hi ? hi : x;
}
void VR_SetPostprocessEnabled(int on) { g_postCfg.enabled = on ? 1 : 0; }
void VR_SetPostprocessSharpen(float x) { g_postCfg.sharpen = PostClamp(x, 0.0f, 0.75f); }
void VR_SetPostprocessBloom(float x) { g_postCfg.bloom = PostClamp(x, 0.0f, 0.20f); }
void VR_SetPostprocessContrast(float x) { g_postCfg.contrast = PostClamp(x, 0.0f, 0.10f); }
void VR_SetPostprocessSaturation(float x) { g_postCfg.saturation = PostClamp(x, 0.80f, 1.20f); }
void VR_SetPostprocessShadowLift(float x) { g_postCfg.shadowLift = PostClamp(x, 0.0f, 0.05f); }
void VR_GetPostprocessConfig(VRPostprocessConfig* out) { if (out) *out = g_postCfg; }
void VR_TestFailNextPostprocess() { g_postFailNext = true; }

struct PostGL {
    unsigned (WINAPI *createShader)(unsigned);
    void (WINAPI *shaderSource)(unsigned,int,const char* const*,const int*);
    void (WINAPI *compileShader)(unsigned);
    void (WINAPI *getShaderiv)(unsigned,unsigned,int*);
    void (WINAPI *getShaderLog)(unsigned,int,int*,char*);
    void (WINAPI *deleteShader)(unsigned);
    unsigned (WINAPI *createProgram)();
    void (WINAPI *attachShader)(unsigned,unsigned);
    void (WINAPI *linkProgram)(unsigned);
    void (WINAPI *getProgramiv)(unsigned,unsigned,int*);
    void (WINAPI *getProgramLog)(unsigned,int,int*,char*);
    void (WINAPI *deleteProgram)(unsigned);
    void (WINAPI *useProgram)(unsigned);
    int (WINAPI *getUniformLocation)(unsigned,const char*);
    void (WINAPI *uniform1i)(int,int);
    void (WINAPI *uniform1f)(int,float);
    void (WINAPI *uniform2f)(int,float,float);
    void (WINAPI *uniform4f)(int,float,float,float,float);
    void (WINAPI *getIntegerv)(unsigned,int*);
    void (WINAPI *getFloatv)(unsigned,float*);
    const unsigned char* (WINAPI *getString)(unsigned);
    void (WINAPI *activeTexture)(unsigned);
    void (WINAPI *multiTexCoord4f)(unsigned,float,float,float,float);
    void (WINAPI *colorMask)(unsigned char,unsigned char,unsigned char,unsigned char);
    void (WINAPI *viewport)(int,int,int,int);
    void (WINAPI *polygonMode)(unsigned,unsigned);
    void (WINAPI *begin)(unsigned);
    void (WINAPI *end)();
    void (WINAPI *texCoord2f)(float,float);
    void (WINAPI *vertex2f)(float,float);
} g_postGL;

static const char* g_postVertexSource =
    "#version 120\n"
    "varying vec2 uv;\n"
    "void main(){ gl_Position=gl_Vertex; uv=gl_MultiTexCoord0.xy; }\n";
static const char* g_postFragmentSource =
    "#version 120\n"
    "uniform sampler2D sourceEye; uniform vec2 texel; uniform vec4 amounts; uniform float shadowLift;\n"
    "varying vec2 uv;\n"
    "vec3 sampleEye(vec2 p){return texture2D(sourceEye,clamp(p,texel*0.5,vec2(1.0)-texel*0.5)).rgb;}\n"
    "float lum(vec3 c){return dot(c,vec3(0.2126,0.7152,0.0722));}\n"
    "vec3 bright(vec3 c){return c*max(lum(c)-0.65,0.0);}\n"
    "void main(){\n"
    " vec4 original=texture2D(sourceEye,uv); vec3 c=original.rgb;\n"
    " vec3 n=sampleEye(uv+vec2(0.0,texel.y)),s=sampleEye(uv-vec2(0.0,texel.y));\n"
    " vec3 e=sampleEye(uv+vec2(texel.x,0.0)),w=sampleEye(uv-vec2(texel.x,0.0));\n"
    " vec3 lo=min(c,min(min(n,s),min(e,w))),hi=max(c,max(max(n,s),max(e,w)));\n"
    // A local min/max limiter prevents bright outlines and dark ringing.
    " c=clamp(c+(c-(n+s+e+w)*0.25)*amounts.x,lo,hi);\n"
    " if(amounts.y>0.0){\n"
    "  vec2 d=texel*3.0; vec3 b=bright(original.rgb)*0.20;\n"
    "  b+=(bright(sampleEye(uv+vec2(d.x,0.0)))+bright(sampleEye(uv-vec2(d.x,0.0)))\n"
    "    +bright(sampleEye(uv+vec2(0.0,d.y)))+bright(sampleEye(uv-vec2(0.0,d.y))))*0.125;\n"
    "  b+=(bright(sampleEye(uv+d))+bright(sampleEye(uv-d))\n"
    "    +bright(sampleEye(uv+vec2(d.x,-d.y)))+bright(sampleEye(uv+vec2(-d.x,d.y))))*0.075;\n"
    "  c+=b*amounts.y;\n"
    " }\n"
    // Keep very dark pixels unchanged; contrast gradually enters midtones.
    " float l=lum(c); float visible=smoothstep(0.08,0.25,l);\n"
    " c=mix(vec3(l),c,amounts.w);\n"
    " c+=(c-vec3(0.5))*amounts.z*4.0*c*(vec3(1.0)-c)*visible;\n"
    // Lift shadow detail gently without turning black into a grey veil.
    " c+=shadowLift*4.0*l*(1.0-l)*(1.0-smoothstep(0.15,0.50,l));\n"
    // Restrained highlight shoulder preserves most of the original exposure.
    " vec3 h=max(c-vec3(0.80),vec3(0.0)); c-=h*h*0.35;\n"
    " gl_FragColor=vec4(clamp(c,0.0,1.0),original.a);\n"
    "}\n";

static unsigned PostCompileShader(unsigned kind, const char* source)
{
    unsigned shader = g_postGL.createShader(kind);
    if (!shader) return 0;
    g_postGL.shaderSource(shader, 1, &source, nullptr);
    g_postGL.compileShader(shader);
    int ok = 0;
    g_postGL.getShaderiv(shader, 0x8B81 /*COMPILE_STATUS*/, &ok);
    if (!ok) {
        char info[1024] = {};
        g_postGL.getShaderLog(shader, sizeof(info)-1, nullptr, info);
        Log("VR: colour shader unavailable: %s; original eyes retained", info);
        g_postGL.deleteShader(shader);
        return 0;
    }
    return shader;
}

static bool PostLoadGL()
{
    if (g_postTried) return g_postReady;
    g_postTried = true;
    if (!LoadGLFunctions() || !gl_CheckFramebufferStatus || !gl_TexParameteri) return false;
    bool found = true;
    #define POST_GL(field, name) do { \
        void* p = CODVR_GetGLProc(name); \
        if (!p || p == (void*)1 || p == (void*)2 || p == (void*)3 || p == (void*)-1) found = false; \
        else g_postGL.field = (decltype(g_postGL.field))p; \
    } while (0)
    POST_GL(createShader, "glCreateShader"); POST_GL(shaderSource, "glShaderSource");
    POST_GL(compileShader, "glCompileShader"); POST_GL(getShaderiv, "glGetShaderiv");
    POST_GL(getShaderLog, "glGetShaderInfoLog"); POST_GL(deleteShader, "glDeleteShader");
    POST_GL(createProgram, "glCreateProgram"); POST_GL(attachShader, "glAttachShader");
    POST_GL(linkProgram, "glLinkProgram"); POST_GL(getProgramiv, "glGetProgramiv");
    POST_GL(getProgramLog, "glGetProgramInfoLog"); POST_GL(deleteProgram, "glDeleteProgram");
    POST_GL(useProgram, "glUseProgram"); POST_GL(getUniformLocation, "glGetUniformLocation");
    POST_GL(uniform1i, "glUniform1i"); POST_GL(uniform1f, "glUniform1f"); POST_GL(uniform2f, "glUniform2f");
    POST_GL(uniform4f, "glUniform4f"); POST_GL(getIntegerv, "glGetIntegerv");
    POST_GL(getFloatv, "glGetFloatv"); POST_GL(getString, "glGetString");
    POST_GL(activeTexture, "glActiveTexture"); POST_GL(multiTexCoord4f, "glMultiTexCoord4f");
    POST_GL(colorMask, "glColorMask"); POST_GL(viewport, "glViewport");
    POST_GL(polygonMode, "glPolygonMode"); POST_GL(begin, "glBegin");
    POST_GL(end, "glEnd"); POST_GL(texCoord2f, "glTexCoord2f");
    POST_GL(vertex2f, "glVertex2f");
    #undef POST_GL
    if (!found || !gl_IsEnabled || !gl_Enable || !gl_Disable) {
        Log("VR: colour postprocess unavailable (GLSL entry points missing); original eyes retained");
        return false;
    }
    unsigned vert = PostCompileShader(0x8B31, g_postVertexSource);
    unsigned frag = PostCompileShader(0x8B30, g_postFragmentSource);
    if (!vert || !frag) {
        if (vert) g_postGL.deleteShader(vert);
        if (frag) g_postGL.deleteShader(frag);
        return false;
    }
    g_postProgram = g_postGL.createProgram();
    if (g_postProgram) {
        g_postGL.attachShader(g_postProgram, vert);
        g_postGL.attachShader(g_postProgram, frag);
        g_postGL.linkProgram(g_postProgram);
    }
    g_postGL.deleteShader(vert); g_postGL.deleteShader(frag);
    int linked = 0;
    if (g_postProgram) g_postGL.getProgramiv(g_postProgram, 0x8B82, &linked);
    if (!linked) {
        char info[1024] = {};
        if (g_postProgram) {
            g_postGL.getProgramLog(g_postProgram, sizeof(info)-1, nullptr, info);
            g_postGL.deleteProgram(g_postProgram); g_postProgram = 0;
        }
        Log("VR: colour program unavailable: %s; original eyes retained", info);
        return false;
    }
    g_postSourceLoc = g_postGL.getUniformLocation(g_postProgram, "sourceEye");
    g_postTexelLoc = g_postGL.getUniformLocation(g_postProgram, "texel");
    g_postAmountsLoc = g_postGL.getUniformLocation(g_postProgram, "amounts");
    g_postShadowLoc = g_postGL.getUniformLocation(g_postProgram, "shadowLift");
    g_postReady = g_postSourceLoc >= 0 && g_postTexelLoc >= 0 && g_postAmountsLoc >= 0 && g_postShadowLoc >= 0;
    Log("VR: stereo colour postprocess %s (per-eye clarity / highlight bloom / restrained colour)",
        g_postReady ? "ready" : "unavailable");
    return g_postReady;
}

// Only this pass's touched state is restored. The shader uses clip-space
// vertices, so engine projection/modelview stacks and client arrays are never
// modified. Texture unit 0 is restored independently of the active unit.
struct PostState {
    int viewport[4], program, active, texture0, drawFbo, readFbo, colorMask[4], polygon[2];
    float coord0[4];
    unsigned caps[24]; unsigned char enabled[24]; int capCount;
    PostState() : capCount(0)
    {
        g_postGL.getIntegerv(0x0BA2, viewport);
        g_postGL.getIntegerv(0x8B8D, &program);
        g_postGL.getIntegerv(0x84E0, &active);
        g_postGL.getIntegerv(0x8CA6, &drawFbo); g_postGL.getIntegerv(0x8CAA, &readFbo);
        g_postGL.getIntegerv(0x0C23, colorMask); g_postGL.getIntegerv(0x0B40, polygon);
        g_postGL.activeTexture(0x84C0);
        g_postGL.getIntegerv(0x8069, &texture0);
        g_postGL.getFloatv(0x0B03 /*CURRENT_TEXTURE_COORDS*/, coord0);
        const unsigned fixed[] = {0x0B71,0x0BE2,0x0C11,0x0B44,0x0B90,0x0BC0,0x0B60,
                                  0x0B50,0x0BF2,0x0BD0,0x0B41,0x0B42};
        for (int i=0; i<(int)(sizeof(fixed)/sizeof(fixed[0])); ++i) SaveDisable(fixed[i]);
        int clips=0; g_postGL.getIntegerv(0x0D32, &clips);
        for (int i=0; i<clips && i<8; ++i) SaveDisable(0x3000+i);
        int major=0, minor=0;
        const char* ver=(const char*)g_postGL.getString(0x1F02);
        if (ver) sscanf_s(ver, "%d.%d", &major, &minor);
        const char* ext=(const char*)g_postGL.getString(0x1F03);
        if (major>=3 || (ext && strstr(ext,"GL_EXT_framebuffer_sRGB"))) SaveDisable(0x8DB9);
        if (major>=3 || (ext && strstr(ext,"GL_EXT_transform_feedback"))) SaveDisable(0x8C89);
        g_postGL.colorMask(1,1,1,1);
        g_postGL.polygonMode(0x0408 /*FRONT_AND_BACK*/, 0x1B02 /*FILL*/);
    }
    void SaveDisable(unsigned cap) {
        caps[capCount]=cap; enabled[capCount]=gl_IsEnabled(cap); ++capCount; gl_Disable(cap);
    }
    ~PostState()
    {
        g_postGL.useProgram((unsigned)program);
        gl_BindFramebuffer(GL_DRAW_FRAMEBUFFER_, (unsigned)drawFbo);
        gl_BindFramebuffer(GL_READ_FRAMEBUFFER_, (unsigned)readFbo);
        g_postGL.viewport(viewport[0],viewport[1],viewport[2],viewport[3]);
        g_postGL.colorMask((unsigned char)colorMask[0],(unsigned char)colorMask[1],
                           (unsigned char)colorMask[2],(unsigned char)colorMask[3]);
        g_postGL.polygonMode(0x0404 /*FRONT*/, (unsigned)polygon[0]);
        g_postGL.polygonMode(0x0405 /*BACK*/, (unsigned)polygon[1]);
        for (int i=0; i<capCount; ++i) { if (enabled[i]) gl_Enable(caps[i]); else gl_Disable(caps[i]); }
        g_postGL.activeTexture(0x84C0);
        gl_BindTexture(GL_TEXTURE_2D_, (unsigned)texture0);
        g_postGL.multiTexCoord4f(0x84C0,coord0[0],coord0[1],coord0[2],coord0[3]);
        g_postGL.activeTexture((unsigned)active);
    }
};

bool VR_PostprocessEye(int eye)
{
    if (!g_postCfg.enabled) return true;
    if (eye<0 || eye>=EYES || !g_eyeTex[eye] || g_capSizeW[eye]<2 || g_capSizeH[eye]<2) return false;
    if (!PostLoadGL()) return false;
    PostState saved;
    const int w=g_capSizeW[eye], h=g_capSizeH[eye];
    if (!g_postFbo) gl_GenFramebuffers(1,&g_postFbo);
    if (!g_postTex[eye]) gl_GenTextures(1,&g_postTex[eye]);
    if (!g_postFbo || !g_postTex[eye]) return false;
    if (g_postW[eye]!=w || g_postH[eye]!=h) {
        gl_BindTexture(GL_TEXTURE_2D_,g_postTex[eye]);
        gl_TexImage2D(GL_TEXTURE_2D_,0,GL_RGBA8_,w,h,0,GL_RGBA_,GL_UNSIGNED_BYTE_,nullptr);
        gl_TexParameteri(GL_TEXTURE_2D_,GL_TEXTURE_MIN_FILTER_,GL_LINEAR_);
        gl_TexParameteri(GL_TEXTURE_2D_,GL_TEXTURE_MAG_FILTER_,GL_LINEAR_);
        gl_TexParameteri(GL_TEXTURE_2D_,GL_TEXTURE_WRAP_S_,GL_CLAMP_TO_EDGE_);
        gl_TexParameteri(GL_TEXTURE_2D_,GL_TEXTURE_WRAP_T_,GL_CLAMP_TO_EDGE_);
        g_postW[eye]=w; g_postH[eye]=h;
    }
    gl_BindFramebuffer(0x8D40,g_postFbo);
    gl_FramebufferTexture2D(0x8D40,GL_COLOR_ATTACHMENT0_,GL_TEXTURE_2D_,g_postTex[eye],0);
    if (gl_CheckFramebufferStatus(0x8D40)!=GL_FRAMEBUFFER_COMPLETE_) return false;
    if (g_postFailNext) { g_postFailNext=false; return false; }
    g_postGL.viewport(0,0,w,h);
    gl_BindTexture(GL_TEXTURE_2D_,g_eyeTex[eye]);
    g_postGL.useProgram(g_postProgram);
    g_postGL.uniform1i(g_postSourceLoc,0);
    g_postGL.uniform2f(g_postTexelLoc,1.0f/w,1.0f/h);
    g_postGL.uniform4f(g_postAmountsLoc,g_postCfg.sharpen,g_postCfg.bloom,g_postCfg.contrast,g_postCfg.saturation);
    g_postGL.uniform1f(g_postShadowLoc,g_postCfg.shadowLift);
    g_postGL.begin(0x0007 /*QUADS*/);
    g_postGL.texCoord2f(0,0); g_postGL.vertex2f(-1,-1);
    g_postGL.texCoord2f(1,0); g_postGL.vertex2f( 1,-1);
    g_postGL.texCoord2f(1,1); g_postGL.vertex2f( 1, 1);
    g_postGL.texCoord2f(0,1); g_postGL.vertex2f(-1, 1);
    g_postGL.end();
    if (gl_GetError) {
        const unsigned error=gl_GetError();
        if (error) {
            static int reports;
            if (reports++<3) Log("VR: colour pass GL error 0x%X; original eye retained",error);
            return false;
        }
    }
    if (!g_capFbo) gl_GenFramebuffers(1,&g_capFbo);
    gl_BindFramebuffer(GL_DRAW_FRAMEBUFFER_,g_capFbo);
    gl_FramebufferTexture2D(GL_DRAW_FRAMEBUFFER_,GL_COLOR_ATTACHMENT0_,GL_TEXTURE_2D_,g_eyeTex[eye],0);
    if (gl_CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER_)!=GL_FRAMEBUFFER_COMPLETE_) return false;
    gl_BindFramebuffer(GL_READ_FRAMEBUFFER_,g_postFbo);
    gl_BlitFramebuffer(0,0,w,h,0,0,w,h,GL_COLOR_BUFFER_BIT_,GL_LINEAR_);
    return true;
}

// dstEye selects the swapchain written to; srcEye selects the captured
// texture read from. The pair machine only ever submits when both eyes hold
// fresh captures, so the two are always the same eye now -- even the first
// XR frame of a session waits for both captures rather than borrowing one
// eye's image for the other (which is what used to keep early frames from
// being half black, at the cost of a wrong-eye picture).
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
    XrResult r = xrAcquireSwapchainImage(g_swapchain[eye], &ai, &index);
    if (XR_FAILED(r)) {
        NoteXrFailure("xrAcquireSwapchainImage", r);
        // CALL_ORDER_INVALID here means an image is stuck acquired -- the
        // chain wedged after a failed wait and will NEVER recover on its own,
        // so escalate as hard as the wait failure that caused it.
        if (r == XR_ERROR_CALL_ORDER_INVALID)
            g_xrConsecutiveFailures += XR_FAILURE_TEARDOWN_THRESHOLD / 10;
        return false;
    }

    XrSwapchainImageWaitInfo wi = {};
    wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
    wi.timeout = XR_INFINITE_DURATION;
    r = xrWaitSwapchainImage(g_swapchain[eye], &wi);
    if (XR_FAILED(r)) {
        // Deliberately NO xrReleaseSwapchainImage here: release requires a
        // successfully waited image. The chain now has an acquired-but-never-
        // released image and is effectively wedged, so escalate hard -- the
        // failure counter tears the session down and rebuilds it long before
        // the old behaviour (silently retry forever) would have shown anything
        // but a frozen eye.
        NoteXrFailure("xrWaitSwapchainImage", r);
        g_xrConsecutiveFailures += XR_FAILURE_TEARDOWN_THRESHOLD / 10;
        return false;
    }

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
            g_capSizeW[srcEye], g_capSizeH[srcEye], g_eyeSizeW[eye], g_eyeSizeH[eye]);
    }

    gl_BlitFramebuffer(0, 0, g_capSizeW[srcEye], g_capSizeH[srcEye],
                       0, 0, (int)g_eyeSizeW[eye], (int)g_eyeSizeH[eye],
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

    // A successfully waited image must always be released, and a failing
    // release is a real problem (the compositor never gets the frame), so it
    // is checked like everything else.
    XrSwapchainImageReleaseInfo ri = {};
    ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
    r = xrReleaseSwapchainImage(g_swapchain[eye], &ri);
    if (XR_FAILED(r)) {
        NoteXrFailure("xrReleaseSwapchainImage", r);
        return false;
    }
    return true;
}

// One-shot instrumentation: everything one hardware run needs to distinguish
// the possible causes of a black perimeter (renderer coverage, crop,
// viewport/scissor, comfort vignette) plus the scale/IPD arithmetic, with
// verdicts computed from the REAL submitted values -- never from a
// subjective report. Verdict words are placed mid-line so the build gate's
// "  FAIL  " line matcher can never trip on them.
static void LogOpticsCheckOnce(const XrCompositionLayerProjectionView views[EYES])
{
    static bool logged = false;
    static int  stereoPairs = 0;
    if (logged)
        return;
    // Wait for the SECOND completed pair: the first pair's eye 0 renders
    // before the supersampled target exists, so its recorded world-pass
    // state would be judged against the wrong render target.
    if (++stereoPairs < 2)
        return;
    logged = true;

    const float r2d = 57.2957795f;
    bool fullRect = true;

    Log("");
    Log("======= VR OPTICS CHECK (first stereo pair) =======");
    for (int e = 0; e < EYES; ++e) {
        const XrRect2Di& r = views[e].subImage.imageRect;
        const XrFovf&    f = views[e].fov;
        const bool full = r.offset.x == 0 && r.offset.y == 0
            && r.extent.width  == (int)g_eyeSizeW[e]
            && r.extent.height == (int)g_eyeSizeH[e];
        fullRect = fullRect && full;
        Log("  eye%d: submit fov L%.2f R%.2f U%.2f D%.2f deg, rect %d,%d %dx%d "
            "of %ux%u%s",
            e, f.angleLeft * r2d, f.angleRight * r2d,
            f.angleUp * r2d, f.angleDown * r2d,
            r.offset.x, r.offset.y, r.extent.width, r.extent.height,
            g_eyeSizeW[e], g_eyeSizeH[e], full ? "" : "  <-- NOT FULL");
        Log("  eye%d: projection replaced %u times, serial %u, rendered origin "
            "%.1f %.1f %.1f u",
            e, g_projReplaced[e], g_eyeRecord[e].serial,
            g_lastEyeOriginU[e][0], g_lastEyeOriginU[e][1], g_lastEyeOriginU[e][2]);
    }
    Log("  culling fov %.1f x %.1f deg (symmetric superset of the eye frustum "
        "+ %.1f deg margin)",
        2.0f * g_lastCullHalfX * r2d, 2.0f * g_lastCullHalfY * r2d,
        g_cullMarginDeg);
    Log("  scene FBO %dx%d (%.2fx supersample of %dx%d); 3D capture is "
        "full FBO -> full eye texture -> full swapchain",
        g_sceneW, g_sceneH, g_superSample, g_sceneBaseW, g_sceneBaseH);
    for (int e = 0; e < EYES; ++e) {
        if (g_projGLStateValid[e])
            Log("  eye%d world pass: viewport %d,%d %dx%d, scissor %s %d,%d %dx%d "
                "(target %dx%d)",
                e,
                g_projGLViewport[e][0], g_projGLViewport[e][1],
                g_projGLViewport[e][2], g_projGLViewport[e][3],
                g_projGLScissorOn[e] ? "ON" : "off",
                g_projGLScissor[e][0], g_projGLScissor[e][1],
                g_projGLScissor[e][2], g_projGLScissor[e][3],
                g_projGLTargetW[e], g_projGLTargetH[e]);
        else
            Log("  eye%d world pass: GL state not recorded (no replacement)", e);
    }

    // IPD arithmetic: the runtime's real eye separation against the
    // separation of the two rendered origins.
    const float dx = g_views[1].pose.position.x - g_views[0].pose.position.x;
    const float dy = g_views[1].pose.position.y - g_views[0].pose.position.y;
    const float dz = g_views[1].pose.position.z - g_views[0].pose.position.z;
    const float ipdM = sqrtf(dx*dx + dy*dy + dz*dz);
    float su[3];
    for (int i = 0; i < 3; ++i)
        su[i] = g_lastEyeOriginU[1][i] - g_lastEyeOriginU[0][i];
    const float sepU = sqrtf(su[0]*su[0] + su[1]*su[1] + su[2]*su[2]);
    Log("  worldscale %.2f u/m (CoD standard: 1 unit = 1 inch), positional "
        "gain %.2f, head neutral %s",
        g_unitsPerMetre, g_positionalGain,
        g_headRefValid ? "latched (midpoint)" : "NOT latched");
    Log("  ipd: runtime %.4f m -> expected %.3f u; rendered origins %.3f u apart",
        ipdM, ipdM * g_unitsPerMetre, sepU);

    const float vig = Comfort_VignetteAmount();
    float vin = 0.0f, vout = 0.0f;
    Comfort_TestAperture(vig, &vin, &vout);
    Log("  vignette: amount %.2f (aperture inner %.2f outer %.2f; > 1.42 is "
        "entirely off screen)", vig, vin, vout);

    const bool projOk = g_projReplaced[0] > 0 && g_projReplaced[1] > 0;
    bool coverageKnown = true;
    bool vpOk = true, scOk = true;
    for (int e = 0; e < EYES; ++e) {
        // Judge each eye against the target IT rendered into; a note taken
        // before the supersampled FBO existed carries no target and cannot
        // be judged.
        if (!g_projGLStateValid[e] || g_projGLTargetW[e] < 2) {
            coverageKnown = false;
            continue;
        }
        vpOk = vpOk
            && VRM_ViewportCovers(g_projGLViewport[e][0], g_projGLViewport[e][1],
                                  g_projGLViewport[e][2], g_projGLViewport[e][3],
                                  g_projGLTargetW[e], g_projGLTargetH[e]);
        scOk = scOk
            && VRM_ScissorCovers(g_projGLScissorOn[e],
                                 g_projGLScissor[e][0], g_projGLScissor[e][1],
                                 g_projGLScissor[e][2], g_projGLScissor[e][3],
                                 g_projGLTargetW[e], g_projGLTargetH[e]);
    }
    const bool anyGLState = coverageKnown;
    const bool posOk = g_unitsPerMetre > 30.0f && g_unitsPerMetre < 50.0f;
    const bool ipdOk = ipdM > 0.02f && fabsf(sepU - ipdM * g_unitsPerMetre) < 0.15f;
    Log("  VERDICT projection=%s fullImageRect=%s viewportCoverage=%s "
        "scissorCoverage=%s",
        projOk ? "PASS" : "FAIL(no GL intercept fired)",
        fullRect ? "PASS" : "FAIL",
        anyGLState ? (vpOk ? "PASS" : "FAIL") : "N/A",
        anyGLState ? (scOk ? "PASS" : "FAIL") : "N/A");
    Log("  VERDICT positionalScale=%s ipdScale=%s vignetteBlackBorderRisk=%s",
        posOk ? "PASS" : "FAIL", ipdOk ? "PASS" : "FAIL",
        vig > 0.05f ? "YES" : "NO");

    // ---- EYE LEDGER: locate -> camera -> optics -> capture -> submit ----
    // Everything the left/right identity claim rests on, per eye, with the
    // laterality SELF-SCORED: 64 mm of numeric separation proves nothing
    // about which side each camera ended up on, so compute that explicitly
    // from the rendered origins and the view's own left axis.
    Log("  ---- EYE LEDGER ----");
    for (int e = 0; e < EYES; ++e) {
        const XrVector3f& p = g_views[e].pose.position;
        const XrFovf&     f = g_views[e].fov;
        Log("  eye%d: XR pos %+.4f %+.4f %+.4f m | fov centre %+.2f deg | "
            "tan L%+.4f R%+.4f | origin %.1f %.1f %.1f u | tex %u | chain %p",
            e, p.x, p.y, p.z,
            (f.angleLeft + f.angleRight) * 0.5f * r2d,
            g_eyeTan[e][0], g_eyeTan[e][1],
            g_lastEyeOriginU[e][0], g_lastEyeOriginU[e][1], g_lastEyeOriginU[e][2],
            g_eyeTex[e], (void*)g_swapchain[e]);
    }
    {
        // Rendered eye 0 must sit to the view's LEFT of rendered eye 1 by
        // exactly the runtime IPD in world units. su = origin1 - origin0
        // points from eye 0 to eye 1, so with a correct layout it points
        // view-RIGHT and dot(su, left) is negative; negate it so the number
        // reads "how far LEFT eye 0 sits" and is positive when correct.
        float left[3];
        for (int i = 0; i < 3; ++i)
            left[i] = (g_lastEyeAxisRows[0][3 + i] + g_lastEyeAxisRows[1][3 + i]) * 0.5f;
        const float eye0LeftBy = -(su[0] * left[0] + su[1] * left[1] + su[2] * left[2]);
        const float wantU = ipdM * g_unitsPerMetre;
        const bool latOk = eye0LeftBy > 0.0f && fabsf(eye0LeftBy - wantU) < 0.25f;
        Log("  laterality: rendered eye0 sits %+.3f u along view-left of eye1 "
            "(want %+.3f): %s",
            eye0LeftBy, wantU, latOk ? "LEFT-OK" : "WRONG-SIDE");
        // The left eye's frustum is left-biased (nasal edge on the right),
        // and each submitted fov must be the located one, same index.
        const bool nasalOk = (g_views[0].fov.angleLeft + g_views[0].fov.angleRight) < 0.0f
                          && (g_views[1].fov.angleLeft + g_views[1].fov.angleRight) > 0.0f;
        bool submitOk = true;
        for (int e = 0; e < EYES; ++e) {
            const XrFovf& a = views[e].fov;
            const XrFovf& b = g_views[e].fov;
            if (fabsf(a.angleLeft - b.angleLeft)  > 1e-5f ||
                fabsf(a.angleRight - b.angleRight) > 1e-5f ||
                fabsf(a.angleUp - b.angleUp)      > 1e-5f ||
                fabsf(a.angleDown - b.angleDown)  > 1e-5f)
                submitOk = false;
        }
        Log("  identity: fovNasal=%s submitFovMatchesLocate=%s pairSerials=%s",
            nasalOk ? "OK" : "SWAPPED",
            submitOk ? "OK" : "MISMATCH",
            (g_eyeRecord[0].serial == g_eyeRecord[1].serial
             && g_eyeRecord[0].serial == g_xrFrameSerial) ? "OK" : "MIXED");
    }
    Log("===================================================");
}

// Relocate the views at the pair's own predicted display time and measure,
// per eye, how far the pose we are about to submit disagrees with the
// runtime's CURRENT answer for that same instant. The submitted pose is the
// locate the cameras rendered from, so the mismatch is exactly the
// prediction revision the compositor will correct for by reprojection --
// rotation exactly, translation only planar (no depth layer is submitted).
// A consistently large translation number here is the mechanism that makes
// far objects slide under head translation.
// ============================================================================
// STEREO FAIL-SAFE
// ============================================================================
//
// The last gate before a stereo pair reaches the compositor. A pair that
// fails ANY of these is not submitted: the frame ends with zero layers, the
// runtime reprojects the previous good pair, and the player sees a brief hold
// instead of a nauseating one. A held frame is uncomfortable; independent
// per-eye camera motion is harmful, and this is the line between them.
//
// Hardware run 2026-08-23 submitted 30.730 u of eye separation against a true
// 2.456 u (12.5x), with the eyes composed on bases ~25 deg apart, for a whole
// session. Nothing downstream of the camera math was able to notice.
static unsigned g_stereoRejects;
static unsigned g_stereoRejectReasonCount[VR_STEREO_REJECT_COUNT];
static int      g_lastStereoReject = -1;
static bool     g_stereoFailsafeTripped;

unsigned VR_StereoRejectCount() { return g_stereoRejects; }
unsigned VR_NativePairRejectCount() { return g_statNativePairRejects; }
int      VR_LastStereoReject()  { return g_lastStereoReject; }
unsigned VR_StereoRejectReasonCount(int reason)
{
    return (reason >= 0 && reason < VR_STEREO_REJECT_COUNT)
         ? g_stereoRejectReasonCount[reason] : 0;
}

static bool Finite3(const float v[3])
{
    for (int i = 0; i < 3; ++i)
        if (!(v[i] == v[i]) || v[i] > 1e17f || v[i] < -1e17f)
            return false;
    return true;
}

static void RejectStereo(int reason, const char* fmt, ...)
{
    ++g_stereoRejects;
    g_lastStereoReject = reason;
    if (reason >= 0 && reason < VR_STEREO_REJECT_COUNT)
        ++g_stereoRejectReasonCount[reason];

    // Bounded logging: a systematic fault must be visible without drowning
    // the log the way 1086 rebase lines did.
    static unsigned logged = 0;
    if (logged < 12) {
        ++logged;
        char msg[320];
        va_list ap;
        va_start(ap, fmt);
        _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
        va_end(ap);
        Log("VR: STEREO PAIR REJECTED -- %s", msg);
    }

    // First rejection disarms every experimental controller-weapon feature and
    // returns the session to clean HMD-only camera behaviour. This is the
    // "fail safe instead of nausea" rule: a broken weapon experiment gets one
    // bad pair, not a whole session.
    if (!g_stereoFailsafeTripped) {
        g_stereoFailsafeTripped = true;
        VRBinds_LockOutExperimentalWeapon("stereo pair rejected");
        VRWeapon_SetTransformEnabled(0);
        VRWeapon_SetEnabled(0);
        Log("VR: FAIL-SAFE -- experimental controller-weapon features disabled; "
            "returning to HMD-only camera for the rest of the session");
    }
}

int VR_StereoFailsafeTripped() { return g_stereoFailsafeTripped ? 1 : 0; }

// The invariant set itself, as a pure function of the composed pair. Taking
// the geometry as arguments lets the self-test inject each hardware failure
// mode directly, with no way for a mutant to be masked by module state.
static bool StereoPairAcceptableFor(const float o0[3], const float o1[3],
                                    const float ax0[9], const float ax1[9],
                                    float ipdM, float unitsPerMetre,
                                    bool baseLatched)
{
    // Any collision-hook camera mutation is fatal on its own: it means the
    // retired 37/38 path, or something shaped like it, is writing the camera.
    if (CODVR_CollisionHookCameraMutationCount() != 0) {
        RejectStereo(VR_STEREO_REJECT_COLLISION_MUTATION,
                     "collisionHookCameraMutationCount = %u (must be 0) -- a "
                     "collision/content hook is writing camera bytes",
                     CODVR_CollisionHookCameraMutationCount());
        return false;
    }

    if (!Finite3(o0) || !Finite3(o1) || !Finite3(&ax0[0]) || !Finite3(&ax1[0])) {
        RejectStereo(VR_STEREO_REJECT_NON_FINITE, "non-finite camera basis");
        return false;
    }

    // Both eyes must have been composed on the SAME latched base pose. This
    // is the invariant the whole 2026-08-23 failure violated.
    if (!baseLatched) {
        RejectStereo(VR_STEREO_REJECT_NO_BASE_LATCH,
                     "no latched pair base camera");
        return false;
    }

    // Expected separation comes from the runtime's OWN eye poses, converted
    // by worldscale. Nothing game-side participates.
    const float expectU = ipdM * unitsPerMetre;

    const float sx = o1[0] - o0[0], sy = o1[1] - o0[1], sz = o1[2] - o0[2];
    const float actualU = sqrtf(sx*sx + sy*sy + sz*sz);

    // A runtime that reports no separation at all (mono, or an IPD read
    // failure) has nothing to check against; the pair is not corrupt.
    if (expectU > 0.05f) {
        if (actualU > expectU * 1.5f) {
            RejectStereo(VR_STEREO_REJECT_SEPARATION_HIGH,
                         "eye separation %.3f u against expected %.3f u "
                         "(runtime IPD %.4f m) -- %.1fx too wide",
                         actualU, expectU, ipdM, actualU / expectU);
            return false;
        }
        if (actualU < expectU * 0.5f) {
            RejectStereo(VR_STEREO_REJECT_SEPARATION_LOW,
                         "eye separation %.3f u against expected %.3f u "
                         "(runtime IPD %.4f m) -- collapsed",
                         actualU, expectU, ipdM);
            return false;
        }

        // Laterality: rendered eye 0 must sit to the view's LEFT of eye 1.
        // Scored on eye 0's own left axis, not an average of the two -- with
        // divergent bases the average is meaningless, which is precisely why
        // the old check could not tell a side reversal from a magnitude error.
        const float* left0 = &ax0[3];
        const float eye0LeftBy = -(sx*left0[0] + sy*left0[1] + sz*left0[2]);
        if (eye0LeftBy <= 0.0f) {
            RejectStereo(VR_STEREO_REJECT_LATERALITY,
                         "laterality REVERSED -- eye0 sits %+.3f u along "
                         "view-left of eye1 (want %+.3f)", eye0LeftBy, expectU);
            return false;
        }
    }

    // The two eyes' bases must agree. They are composed from one latched
    // basis plus the runtime's own per-eye orientations, so the only
    // legitimate difference is the runtime's eye-to-eye canting -- fractions
    // of a degree on every headset in circulation. 25 deg apart, as measured
    // on 2026-08-23, is the "each eye rotating independently" symptom.
    {
        // Chord form, not acos(dot): near-parallel axes make acos
        // ill-conditioned -- a single float ULP in the dot product of two
        // IDENTICAL bases reads as 0.02 deg of divergence that is not there.
        // 2*asin(|f0-f1|/2) is exact at zero and well-conditioned throughout.
        const float* f0 = &ax0[0];
        const float* f1 = &ax1[0];
        const float cx = f1[0] - f0[0], cy = f1[1] - f0[1], cz = f1[2] - f0[2];
        float halfChord = sqrtf(cx*cx + cy*cy + cz*cz) * 0.5f;
        if (halfChord > 1.0f) halfChord = 1.0f;
        const float divergeDeg = 2.0f * asinf(halfChord) * 57.2957795f;
        if (divergeDeg > VR_STEREO_MAX_EYE_DIVERGE_DEG) {
            RejectStereo(VR_STEREO_REJECT_ORIENTATION_DIVERGENCE,
                         "eye orientations diverge %.2f deg (limit %.2f) -- "
                         "the eyes are on different bases",
                         divergeDeg, (float)VR_STEREO_MAX_EYE_DIVERGE_DEG);
            return false;
        }
    }

    return true;
}

// Production entry point: the module's own last-composed pair, measured
// against the runtime's own located eye poses.
static bool StereoPairAcceptable()
{
    return StereoPairAcceptableFor(g_lastEyeOriginU[0], g_lastEyeOriginU[1],
                                   g_lastEyeAxisRows[0], g_lastEyeAxisRows[1],
                                   g_acceptedIpdM, g_unitsPerMetre,
                                   g_baseLatchValid);
}

// Test seam: score an arbitrary composed pair. Used to inject each named
// 2026-08-23 hardware failure as a mutant and require a rejection. The
// fail-safe side effects are deliberately suppressed here -- a mutant must
// not disable the features the NEXT scenario is about to test.
int VR_TestStereoPairAcceptable(const float o0[3], const float o1[3],
                                const float ax0[9], const float ax1[9],
                                float ipdM, float unitsPerMetre)
{
    const bool wasTripped = g_stereoFailsafeTripped;
    g_stereoFailsafeTripped = true;               // suppress the lockout
    const bool ok = StereoPairAcceptableFor(o0, o1, ax0, ax1, ipdM,
                                            unitsPerMetre, true);
    g_stereoFailsafeTripped = wasTripped;
    return ok ? 1 : 0;
}

static bool MeasureRenderSubmitConsistency()
{
    if (g_session == XR_NULL_HANDLE || g_space == XR_NULL_HANDLE)
        return false;
    if (!g_eyeRecord[0].valid || !g_eyeRecord[1].valid)
        return false;

    XrViewLocateInfo vli = {};
    vli.type = XR_TYPE_VIEW_LOCATE_INFO;
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = g_frameState.predictedDisplayTime;
    vli.space = g_space;
    XrViewState vs = {};
    vs.type = XR_TYPE_VIEW_STATE;
    XrView now[EYES] = {};
    now[0].type = now[1].type = XR_TYPE_VIEW;
    uint32_t got = 0;
    if (XR_FAILED(xrLocateViews(g_session, &vli, &vs, EYES, &got, now)) || got != EYES)
        return false;
    if (!(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
        return false;

    for (int e = 0; e < EYES; ++e) {
        const XrPosef& a = g_eyeRecord[e].pose;
        const XrPosef& b = now[e].pose;
        const float dx = a.position.x - b.position.x;
        const float dy = a.position.y - b.position.y;
        const float dz = a.position.z - b.position.z;
        g_consistencyPosM[e] = sqrtf(dx*dx + dy*dy + dz*dz);
        float dot = a.orientation.x * b.orientation.x
                  + a.orientation.y * b.orientation.y
                  + a.orientation.z * b.orientation.z
                  + a.orientation.w * b.orientation.w;
        if (dot < 0.0f) dot = -dot;
        if (dot > 1.0f) dot = 1.0f;
        g_consistencyRotDeg[e] = 2.0f * acosf(dot) * 57.2957795f;
    }
    g_consistencyValid = 1;
    return true;
}

static void LogRenderSubmitConsistency()
{
    const bool pass = g_consistencyPosM[0] < 0.003f && g_consistencyPosM[1] < 0.003f
                   && g_consistencyRotDeg[0] < 0.25f && g_consistencyRotDeg[1] < 0.25f;
    const double elapsedMs = (CODVR_NowSeconds() - g_pairLocateTime) * 1000.0;
    Log("");
    Log("======= RENDER/SUBMIT POSE CONSISTENCY =======");
    Log("  pair serial %u, %.2f ms locate -> submit", g_xrFrameSerial, elapsedMs);
    for (int e = 0; e < EYES; ++e)
        Log("  eye%d: translation mismatch %.4f m, rotation mismatch %.3f deg",
            e, g_consistencyPosM[e], g_consistencyRotDeg[e]);
    Log("  (submitted pose IS the locate the cameras rendered from; nonzero =");
    Log("   the runtime revised its prediction between locate and submit)");
    Log("  renderSubmitPoseConsistency = %s (tol 3 mm / 0.25 deg)",
        pass ? "PASS" : "MISMATCH");
    Log("==============================================");
}

// Copy both captured eyes to the swapchains and end the XR frame with
// exactly one stereo projection layer. Both records are known-valid for the
// current generation when this runs (VR_SubmitFrame verifies before
// calling). If any copy fails, the whole layer is withheld -- a layer with a
// missing view is malformed and the runtime rejects the frame -- and the
// frame is ended with zero layers instead; the compositor simply reprojects
// the previous frame, and the next boundary starts a clean pair.
static void SubmitBothEyesAndEnd(bool stereo)
{
    XrCompositionLayerProjectionView views[EYES] = {};
    XrCompositionLayerProjection layer = {};
    const XrCompositionLayerBaseHeader* layers[1];
    uint32_t layerCount = 0;
    bool viewsComplete = true;

    for (int eye = 0; eye < EYES; ++eye) {
        if (!g_eyeTexValid[eye] || !g_eyeRecord[eye].valid || !BlitToEye(eye, eye)) {
            viewsComplete = false;
            break;
        }

        // Everything below comes from the record made when the pixels were
        // captured, so the compositor is told exactly how this image was
        // rendered.
        const EyeRenderRecord& rec = g_eyeRecord[eye];
        const XrFovf& f = rec.runtimeFov;
        // A 2D panel (halfX == 0) is fitted to the eye image, not rendered
        // through a frustum, so it keeps its symmetric presentation. A 3D eye
        // was rendered through the runtime's OWN asymmetric frustum (the
        // projection intercept), so the FULL image is submitted with exactly
        // that fov -- the q3vr / RTCWQuest / hello_xr shape. The old
        // symmetric-superset -> sub-rectangle crop is gone from the 3D path:
        // it recreated the frustum after the fact and left the picture at the
        // mercy of each runtime's imageRect handling, which on VDXR showed up
        // as a zoomed world inside a black-bordered square.
        const bool fitted = (rec.halfX <= 0.0f || rec.halfY <= 0.0f);

        views[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        // A pinned panel keeps the pose it appeared at. A 3D eye reports the
        // pose its image was rendered with -- both eyes of a pair share one
        // located view set, so the poses are the two eyes of one head sample
        // even though the captures are a game frame apart.
        views[eye].pose = !stereo ? g_panelPose[eye] : rec.pose;
        if (fitted) {
            const float halfX = fabsf(f.angleLeft) > fabsf(f.angleRight)
                              ? fabsf(f.angleLeft) : fabsf(f.angleRight);
            const float halfY = fabsf(f.angleUp) > fabsf(f.angleDown)
                              ? fabsf(f.angleUp) : fabsf(f.angleDown);
            views[eye].fov.angleLeft  = -halfX;
            views[eye].fov.angleRight =  halfX;
            views[eye].fov.angleUp    =  halfY;
            views[eye].fov.angleDown  = -halfY;
        } else {
            views[eye].fov = f;          // exactly what the eye rendered with
        }
        views[eye].subImage.swapchain = g_swapchain[eye];
        views[eye].subImage.imageArrayIndex = 0;
        views[eye].subImage.imageRect.offset.x = 0;
        views[eye].subImage.imageRect.offset.y = 0;
        views[eye].subImage.imageRect.extent.width  = (int32_t)g_eyeSizeW[eye];
        views[eye].subImage.imageRect.extent.height = (int32_t)g_eyeSizeH[eye];

        static bool fovLogged = false;
        if (!fovLogged && stereo && eye == 1) {
            fovLogged = true;
            Log("VR: runtime fov L%.2f R%.2f U%.2f D%.2f deg",
                f.angleLeft  * 57.2957795f, f.angleRight * 57.2957795f,
                f.angleUp    * 57.2957795f, f.angleDown  * 57.2957795f);
            Log("VR: per-eye asymmetric projection; submitting the FULL "
                "%ux%u eye image with the runtime fov",
                g_eyeSizeW[eye], g_eyeSizeH[eye]);
        }
    }

    if (stereo && viewsComplete)
        LogOpticsCheckOnce(views);

    // The Phase-7 probe: once automatically (pair 240, ~2 s into stereo, well
    // clear of the strict locate-count windows the offline suite pins on the
    // first pairs) or on demand via the test arm.
    if (stereo && viewsComplete
        && (g_consistencyArmed || (!g_consistencyDone && g_xrFrameSerial >= 240))) {
        const bool armed = g_consistencyArmed;
        g_consistencyArmed = false;
        g_consistencyDone = true;
        if (MeasureRenderSubmitConsistency() && !armed)
            LogRenderSubmitConsistency();
    }

    // ---- STEREO FAIL-SAFE: never submit a pair that would make the user ill.
    // Runs on the composed, about-to-be-submitted geometry -- the last point
    // where a corrupt pair can still be stopped.
    if (viewsComplete && stereo && !StereoPairAcceptable())
        viewsComplete = false;                  // abort THIS pair, keep the loop alive

    // ---- NATIVE-OBJECT PAIR VERDICT: the same last point, for the hands and
    // the weapon. Eye 0 rendered provisionally, eye 1 rendered, and only now
    // can the pair be judged from what each eye ACTUALLY did. A half-native
    // pair, or two finals that do not reconstruct to one PairWorld object,
    // is ended with zero layers exactly like a camera stereo fault -- the
    // compositor reprojects the previous frame and the next boundary starts
    // clean. Judged AFTER the camera gate so an already-rejected pair is not
    // double-counted, and only for real stereo pairs.
    if (viewsComplete && stereo) {
        VRNHPairVerdict pv;
        const int ok = VRNH_PairPreSubmit(g_xrFrameSerial, &pv);
        // objapply: the first few judged pairs in which anything applied.
        static unsigned objApplyLines = 0;
        const bool anyApplied =
            pv.obj[0].applied[0] || pv.obj[0].applied[1] ||
            pv.obj[1].applied[0] || pv.obj[1].applied[1] ||
            pv.obj[2].applied[0] || pv.obj[2].applied[1];
        if ((anyApplied && objApplyLines < 10) || (!ok && objApplyLines < 30)) {
            ++objApplyLines;
            Log("objapply: pair=%u Rapplied=%d/%d Lapplied=%d/%d gunApplied=%d/%d "
                "RfinalPairWorld=(%.4f %.4f %.4f | %.4f %.4f %.4f) "
                "LfinalPairWorld=(%.4f %.4f %.4f | %.4f %.4f %.4f) "
                "gunRearGripPairWorld=(%.4f %.4f %.4f | %.4f %.4f %.4f) "
                "err R %.3fmm/%.3fdeg L %.3fmm/%.3fdeg gun %.3fmm/%.3fdeg "
                "pairStereoVerdict=%s%s%s",
                pv.pairSerial,
                pv.obj[0].applied[0], pv.obj[0].applied[1],
                pv.obj[1].applied[0], pv.obj[1].applied[1],
                pv.obj[2].applied[0], pv.obj[2].applied[1],
                pv.obj[0].pairWorldPosM[0][0], pv.obj[0].pairWorldPosM[0][1], pv.obj[0].pairWorldPosM[0][2],
                pv.obj[0].pairWorldPosM[1][0], pv.obj[0].pairWorldPosM[1][1], pv.obj[0].pairWorldPosM[1][2],
                pv.obj[1].pairWorldPosM[0][0], pv.obj[1].pairWorldPosM[0][1], pv.obj[1].pairWorldPosM[0][2],
                pv.obj[1].pairWorldPosM[1][0], pv.obj[1].pairWorldPosM[1][1], pv.obj[1].pairWorldPosM[1][2],
                pv.obj[2].pairWorldPosM[0][0], pv.obj[2].pairWorldPosM[0][1], pv.obj[2].pairWorldPosM[0][2],
                pv.obj[2].pairWorldPosM[1][0], pv.obj[2].pairWorldPosM[1][1], pv.obj[2].pairWorldPosM[1][2],
                pv.obj[0].posErrMm, pv.obj[0].rotErrDeg,
                pv.obj[1].posErrMm, pv.obj[1].rotErrDeg,
                pv.obj[2].posErrMm, pv.obj[2].rotErrDeg,
                ok ? "OK" : "REJECT",
                ok ? "" : " ", ok ? "" : VRNH_VerdictName(pv.reason));
        }
        if (!ok) {
            ++g_statNativePairRejects;
            static int moans = 0;
            if (moans < 8) {
                ++moans;
                Log("VR: native-object pair %u REFUSED at pre-submit (%s on %s) "
                    "-- ending XR frame with zero layers",
                    pv.pairSerial, VRNH_VerdictName(pv.reason),
                    VRNH_ObjName(pv.objIndex));
            }
            viewsComplete = false;
        }
    }

    if (viewsComplete) {
        layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
        layer.space = g_space;
        layer.viewCount = EYES;
        layer.views = views;
        layers[0] = (const XrCompositionLayerBaseHeader*)&layer;
        layerCount = 1;
    } else if (stereo && g_pairPhase == PAIR_SECOND_EYE) {
        // Both eyes were captured but never reached the compositor: that is
        // an aborted pair even though the frame is still ended correctly.
        ++g_statAbortedPairs;
    }

    // A blit failure above can escalate all the way into a session teardown
    // (persistent-failure threshold). The frame died with the session; there
    // is nothing left to end, and calling xrEndFrame on a destroyed handle
    // through the real loader is not survivable.
    if (!g_frameBegun || g_session == XR_NULL_HANDLE) {
        g_pairPhase = PAIR_IDLE;
        g_pairFirstCapTime = 0.0;
        return;
    }

    XrFrameEndInfo fei = {};
    fei.type = XR_TYPE_FRAME_END_INFO;
    fei.displayTime = g_frameState.predictedDisplayTime;
    fei.environmentBlendMode = g_blendMode;
    fei.layerCount = layerCount;
    fei.layers = layerCount ? layers : nullptr;
    XrResult r = xrEndFrame(g_session, &fei);
    if (XR_FAILED(r))
        NoteXrFailure("xrEndFrame", r);
    else if (viewsComplete) {
        NoteXrHealthy();          // a whole frame went through cleanly
        ++g_winSubmitted;
    }

    g_frameBegun = false;
    g_pairPhase = PAIR_IDLE;      // successful or not, this frame is over
    g_pairFirstCapTime = 0.0;
}

// Startup menu contract: upright, level, in front of the user -- never
// mirrored, rolled, or inverted. The 2026-08-20 run pinned the panel to the
// RAW head pose of the first flat frame, which at startup is whatever
// attitude the headset happened to have (face-down on a desk, mid-donning),
// leaving the menu hanging crooked or upside-down in world space. The panel
// therefore never inherits the full head attitude: it takes the head's
// horizontal facing only, gravity-levelled with +Y up, and a merely-valid
// orientation is enough to present it (position is cosmetic for a fitted
// panel and is taken only when finite).
static void PanelPoseFromViews(XrPosef out[EYES])
{
    for (int e = 0; e < EYES; ++e) {
        out[e].orientation.x = 0.0f; out[e].orientation.y = 0.0f;
        out[e].orientation.z = 0.0f; out[e].orientation.w = 1.0f;
        out[e].position.x = out[e].position.y = out[e].position.z = 0.0f;

        // forward = q * (0,0,-1); its horizontal azimuth is the only part of
        // the head attitude the panel adopts. Yaw about +Y keeps the panel
        // upright and right-handed for ANY head attitude -- an upside-down or
        // rolled headset still yields a level, non-mirrored panel.
        const XrQuaternionf& q = g_views[e].pose.orientation;
        const float fx = -2.0f * (q.x * q.z + q.w * q.y);
        const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
        if (fx * fx + fz * fz > 0.01f) {      // not looking straight up/down
            const float yaw = atan2f(-fx, -fz);
            out[e].orientation.y = sinf(yaw * 0.5f);
            out[e].orientation.w = cosf(yaw * 0.5f);
        }
        if (FiniteVec(g_views[e].pose.position))
            out[e].position = g_views[e].pose.position;
    }
}

// A flat frame (menu, briefing, loading screen): one game frame carries the
// whole presentation, captured to BOTH eyes and submitted on the XR frame
// that is already open -- flat screens keep the old one-XR-frame-per-game-
// frame cadence, and forcing them through eye pairing would just double
// their latency for nothing. If a 3D pair was half-way through when the menu
// appeared, it is flushed first: its capture is invalidated and counted,
// because an eye from the previous gameplay pair must never be composed with
// a freshly captured panel.
static void SubmitFlatFrame(int fbWidth, int fbHeight)
{
    if (g_pairPhase == PAIR_SECOND_EYE) {
        static int moans = 0;
        if (moans < 5) {
            ++moans;
            Log("VR: flat frame interrupted a half-complete pair -- flushing it");
        }
        ResetPairState();                 // counts the abort, drops the capture
        g_pairPhase = PAIR_FIRST_EYE;     // the open XR frame now belongs to the panel
    }

    // Pin the panel -- upright and levelled, facing the player's current
    // horizontal azimuth (PanelPoseFromViews) -- once orientation TRACKING
    // is up. Until then the pose is rebuilt every frame from whatever the
    // runtime reports, so a pre-tracking or garbage attitude can never be
    // latched: worst case the menu follows the head for a moment and pins
    // when tracking arrives. The pin drops when real 3D resumes (stereo
    // path) and on any recenter, so the next panel re-pins where the player
    // is looking then.
    if (!g_panelPinned) {
        PanelPoseFromViews(g_panelPose);
        if (g_lastViewStateFlags & XR_VIEW_STATE_ORIENTATION_TRACKED_BIT)
            g_panelPinned = true;
    }

    // A 2D screen belongs to neither eye, so it goes to both, with halfX = 0
    // marking "fitted panel, not a frustum". The records still carry this
    // frame's serial: a panel from one generation cannot leak into another.
    for (int e = 0; e < EYES; ++e) {
        if (VR_CaptureEye(e, fbWidth, fbHeight, true)) {
            EyeRenderRecord& rec = g_eyeRecord[e];
            rec.valid      = true;
            rec.pose       = g_views[e].pose;
            rec.runtimeFov = g_views[e].fov;
            rec.halfX      = 0.0f;
            rec.halfY      = 0.0f;
            rec.serial     = g_xrFrameSerial;
        }
    }

    if (!g_eyeRecord[0].valid || !g_eyeRecord[1].valid
        || g_eyeRecord[0].serial != g_xrFrameSerial
        || g_eyeRecord[1].serial != g_xrFrameSerial) {
        AbortPair("panel capture failed");
        return;
    }

    SubmitBothEyesAndEnd(false);
}

void VR_SubmitFrame(int fbWidth, int fbHeight, bool stereo, int renderedEye)
{
    // The frame's GL is finished by the time the swap hook calls this, so
    // the projection-override window closes here -- nothing drawn from now
    // until the next VR_GetEyeCamera (menus, loading screens, our own
    // blits) can be mistaken for the 3D scene.
    DisarmProjection();

    if (renderedEye < 0 || renderedEye >= EYES)
        renderedEye = 0;
    // Skip-render and inactive frames were already ended inside
    // VR_BeginFrameEx, so nothing here runs for them -- no capture, no
    // vignette, no blits, no stale-pose layers.
    if (!g_frameBegun)
        return;

    if (fbWidth < 2 || fbHeight < 2) {
        // Nothing sensible to copy. Still end the frame, or the runtime
        // stalls waiting on a frame that never arrives.
        static bool moaned = false;
        if (!moaned) { moaned = true; Log("VR: back buffer %dx%d - nothing to submit", fbWidth, fbHeight); }
        AbortPair("no back buffer");
        return;
    }

    if (!g_everLocated) {
        // A begun frame implies a successful locate; if that invariant ever
        // breaks, end empty rather than submit nonsense.
        AbortPair("no located views");
        return;
    }

    if (!stereo) {
        SubmitFlatFrame(fbWidth, fbHeight);
        return;
    }

    // ------------------------------------------------------- 3D pair path
    // Real 3D is running again: drop the panel pin so the next flat screen
    // re-pins where the player is looking when it appears.
    g_panelPinned = false;

    if (g_pairPhase == PAIR_IDLE) {
        // Impossible by construction (a begun frame is FIRST or SECOND);
        // self-diagnose rather than guess which eye this was.
        Log("VR: PAIR BUG -- 3D submit with no open pair");
        AbortPair("no open pair");
        return;
    }
    const int expected = (g_pairPhase == PAIR_SECOND_EYE) ? 1 : 0;
    if (renderedEye != expected) {
        Log("VR: PAIR BUG -- rendered eye %d but the pair expected eye %d",
            renderedEye, expected);
        AbortPair("unexpected eye");
        return;
    }

    if (!VR_CaptureEye(renderedEye, fbWidth, fbHeight, false)) {
        static int moans = 0;
        if (moans < 5) {
            ++moans;
            Log("VR: eye %d capture failed -- aborting the pair", renderedEye);
        }
        AbortPair("eye capture failed");
        return;
    }
    // Colour effects stay in this eye's completed image; failure leaves the
    // original image available to the compositor. Menus remain unprocessed.
    VR_PostprocessEye(renderedEye);
    // Tunnel the periphery while the player is moving or turning. After
    // capture so it lands on the finished frame; only on 3D.
    Comfort_DrawVignette(renderedEye);
    // The VR Options overlay (brief 2026-09-04, P4), on top of everything
    // and only when it is open.
    VRMenu_Draw(renderedEye);
    // In-engine snapshot of the finished eye (2026-09-06), only when armed by
    // vr_snapshot and due; a pure readback, no state left behind.
    VRSnap_AfterEye(renderedEye);

    // RECORD how this eye was rendered -- pose, runtime fov, symmetric
    // superset, generation -- from the PAIR's single located view set. The
    // record travels with the texture all the way to submission.
    EyeRenderRecord& rec = g_eyeRecord[renderedEye];
    rec.valid      = true;
    rec.pose       = g_views[renderedEye].pose;
    rec.runtimeFov = g_views[renderedEye].fov;
    rec.halfX      = g_submitHalfX[renderedEye];
    rec.halfY      = g_submitHalfY[renderedEye];
    rec.serial     = g_xrFrameSerial;
    ++g_statFreshTotal[renderedEye];
    ++g_winFresh[renderedEye];

    const double now = CODVR_NowSeconds();
    if (renderedEye == 0) {
        // FIRST EYE: capture only. The XR frame stays open, nothing is
        // submitted, and -- the entire point -- no xrWaitFrame runs before
        // the other eye. Control goes straight back to the game, which
        // renders eye 1 as fast as it can.
        g_pairFirstCapTime = now;
        g_pairPhase = PAIR_SECOND_EYE;
        return;
    }

    // SECOND EYE: the pair is complete. Measure how long the game needed to
    // produce the second eye -- the number that says whether pairing
    // actually doubled the fresh rate (a few ms) or the engine itself is the
    // bottleneck (a whole display interval).
    if (g_pairFirstCapTime > 0.0 && now >= g_pairFirstCapTime) {
        const double sep = now - g_pairFirstCapTime;
        g_winSepSum += sep;
        if (sep > g_winSepMax) g_winSepMax = sep;
        ++g_winSepCount;
    }
    g_pairFirstCapTime = 0.0;

    // Never submit a mixed-generation stereo pair: both captures must belong
    // to THIS located XR frame. The state machine should make a mismatch
    // impossible; if one appears anyway, say so loudly and end with zero
    // layers instead of silently reintroducing "one fresh / one stale".
    if (!g_eyeRecord[0].valid || !g_eyeRecord[1].valid
        || g_eyeRecord[0].serial != g_xrFrameSerial
        || g_eyeRecord[1].serial != g_xrFrameSerial) {
        Log("VR: PAIR BUG -- generation mismatch at submit (serials %d/%d, frame %u)",
            g_eyeRecord[0].valid ? (int)g_eyeRecord[0].serial : -1,
            g_eyeRecord[1].valid ? (int)g_eyeRecord[1].serial : -1,
            g_xrFrameSerial);
        AbortPair("mixed-generation pair");
        return;
    }

    SubmitBothEyesAndEnd(true);
}

void VR_Shutdown()
{
    TeardownInstance("shutdown");
}

// Testing seam: return the module to a truly cold state -- handles, neutrals,
// retained records, counters, failure latches, retry throttle -- so scenario
// tests are independent of each other. GL texture/FBO ids survive (the test
// process keeps one context throughout); zeroed sizes force re-creation.
void VR_TestResetAll()
{
    TeardownInstance("test reset");

    g_startupFailed  = false;
    g_userExited     = false;
    g_lastRetryTick  = 0;
    g_blendMode      = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    g_frameState     = {};
    g_frameState.type = XR_TYPE_FRAME_STATE;

    g_headRefValid = g_headYawRefValid = false;
    g_relatchPending = true;
    g_lastSaneValid[0] = g_lastSaneValid[1] = false;
    g_pairFarStreak = 0;
    g_originAnomalyLogged = false;
    g_lastViewStateFlags = 0;
    g_headWorldValid = false;
    g_headYawOffsetDeg = g_headPitchOffsetDeg = 0.0f;
    g_headLocalYawValid = false;
    g_headLocalYawDeg = 0.0f;
    g_consistencyArmed = false;
    g_consistencyDone = false;
    g_consistencyValid = 0;
    g_consistencyPosM[0] = g_consistencyPosM[1] = 0.0f;
    g_consistencyRotDeg[0] = g_consistencyRotDeg[1] = 0.0f;
    g_pairLocateTime = 0.0;

    g_prevOriginValid = g_prevAimYawValid = false;
    g_prevMotionTime = 0.0;
    g_motionDtAccum = 0.0;
    g_pendingDist = g_pendingDyawDeg = 0.0f;
    g_playerSpeed = g_turnRate = 0.0f;

    for (int e = 0; e < EYES; ++e) {
        g_eyeRecord[e].valid = false;
        g_eyeTexValid[e] = false;
        g_capSizeW[e] = g_capSizeH[e] = 0;
        g_eyeSizeW[e] = g_eyeSizeH[e] = 0;
        g_submitHalfX[e] = g_submitHalfY[e] = 0.0f;
        g_eyeTan[e][0] = g_eyeTan[e][1] = g_eyeTan[e][2] = g_eyeTan[e][3] = 0.0f;
        g_projReplaced[e] = 0;
        g_projLoggedEye[e] = false;
        g_lastEyeOriginU[e][0] = g_lastEyeOriginU[e][1] = g_lastEyeOriginU[e][2] = 0.0f;
        memset(g_lastEyeAxisRows[e], 0, sizeof(g_lastEyeAxisRows[e]));
        g_projGLStateValid[e] = false;
        g_projGLScissorOn[e] = 0;
        g_projGLTargetW[e] = g_projGLTargetH[e] = 0;
    }
    g_projArmedEye = -1;
    g_lastCullHalfX = g_lastCullHalfY = 0.0f;
    g_positionalGain = 1.0f;
    g_cullMarginDeg  = 5.0f;
    g_panelPinned = false;
    g_sceneBound  = false;
    g_sceneFailed = false;
    g_menuScale = 0.80f;      // keep equal to the static default above
    g_menuDistM = 0.0f;
    g_menuContentHalfX = g_menuContentHalfY = 0.0f;
    g_menuVisHalfX = g_menuVisHalfY = 0.0f;
    g_menuSafeValid = false;
    g_heightAdjustM = 0.0f;
    VRInput_TestReset();
    VRWeapon_TestReset();

    g_pairPhase = PAIR_IDLE;
    g_xrFrameSerial = 0;
    g_pairFirstCapTime = 0.0;
    g_testFailCaptures = 0;
    g_testNow = -1.0;

    g_xrTargetHz = 0.0f;
    g_statSkipped = g_statDiscarded = g_statLocateFail = g_statXrErrors = 0;
    g_statFreshTotal[0] = g_statFreshTotal[1] = 0;
    g_statAbortedPairs = 0;
    g_statNativePairRejects = 0;
    g_perfWindowStart = 0.0;
    g_winXrFrames = g_winGameFrames = g_winSubmitted = 0;
    g_winFresh[0] = g_winFresh[1] = 0;
    g_winSepSum = g_winSepMax = 0.0;
    g_winSepCount = 0;
    g_lastPresentHz = g_lastGameHz = g_lastSubmitHz = 0.0f;
    g_lastFreshHz[0] = g_lastFreshHz[1] = 0.0f;
    g_lastSepMeanMs = g_lastSepMaxMs = 0.0f;
    g_lastPairVerdict = VR_PAIR_UNKNOWN;
    g_perfValid = false;
    g_perfNextLog = 0.0;
    g_perfLogCount = 0;
    g_xrConsecutiveFailures = 0;

    sprintf_s(g_status, "not started");
}

// ------------------------------------------------ pair-machine test seams

int VR_TestPairPhase()
{
    return (int)g_pairPhase;
}

unsigned VR_TestFrameSerial()
{
    return g_xrFrameSerial;
}

int VR_TestEyeRecordSerial(int eye)
{
    if (eye < 0 || eye >= EYES || !g_eyeRecord[eye].valid)
        return -1;
    return (int)g_eyeRecord[eye].serial;
}

void VR_TestFailNextCaptures(int n)
{
    g_testFailCaptures = n > 0 ? n : 0;
}

void VR_TestCorruptEyeSerial(int eye)
{
    if (eye >= 0 && eye < EYES && g_eyeRecord[eye].valid)
        g_eyeRecord[eye].serial -= 1;
}





