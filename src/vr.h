// OpenXR side of the mod, kept behind a narrow interface so the hook code in
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
// still works. Startup is all-or-nothing: on any failure after session
// creation everything partially created is destroyed, so a later call retries
// from a clean slate instead of trusting a half-built session.
bool VR_Startup(HDC hdc, HGLRC glrc);

// True once a session is running and frames should be submitted.
bool VR_Active();

// What this XR frame wants from the caller.
//
//  VR_FRAME_INACTIVE    - no usable session (not started, lost, or stopped).
//                         Do no VR work at all.
//  VR_FRAME_SKIP_RENDER - the frame loop ran, but the runtime does not want a
//                         picture (headset off, dashboard open, poses gone).
//                         The XR frame has ALREADY been ended with zero
//                         layers; skip capture/vignette/blit entirely.
//  VR_FRAME_RENDER      - views located; render, then VR_SubmitFrame.
enum VRFrameDisposition {
    VR_FRAME_INACTIVE    = 0,
    VR_FRAME_SKIP_RENDER = 1,
    VR_FRAME_RENDER      = 2
};

// Drive the XR frame loop for the coming GAME frame.
//
// One OpenXR frame spans TWO 3D game frames (one per eye), so this is only
// sometimes a real xrWaitFrame:
//
//  * at a pair boundary it runs the full wait/begin/(end-if-not-rendering)/
//    locate protocol, so every xrWaitFrame is paired correctly no matter what
//    the caller does next;
//  * BETWEEN the two eyes of a pair (first eye captured, XR frame still open)
//    it only pumps events and returns RENDER: no xrWaitFrame, no xrBeginFrame,
//    no xrLocateViews, and the predicted display time is untouched, so both
//    eyes render against the same located view pair. Removing the wait from
//    between the eyes is what lets an unthrottled engine produce two fresh
//    eyes per display interval instead of one.
VRFrameDisposition VR_BeginFrameEx();

// ---------------------------------------------------------- stereo fail-safe
//
// Reasons a composed stereo pair can be refused submission. Each is a
// condition under which submitting would produce a physically wrong or
// nausea-inducing image, so the pair is dropped and the runtime reprojects
// the last good one instead.
enum {
    VR_STEREO_REJECT_COLLISION_MUTATION = 0, // a collision hook wrote camera bytes
    VR_STEREO_REJECT_NON_FINITE,             // NaN/inf in a camera basis
    VR_STEREO_REJECT_NO_BASE_LATCH,          // eyes not composed on one pair base
    VR_STEREO_REJECT_SEPARATION_HIGH,        // > 1.5x the runtime IPD
    VR_STEREO_REJECT_SEPARATION_LOW,         // < 0.5x the runtime IPD
    VR_STEREO_REJECT_LATERALITY,             // eye 0 is not to the view's left
    VR_STEREO_REJECT_ORIENTATION_DIVERGENCE, // the eyes are on different bases
    VR_STEREO_REJECT_COUNT
};

// Maximum legitimate angle between the two eyes' forward axes. Real runtimes
// cant the eyes by fractions of a degree; anything larger means the eyes were
// composed on different bases. The 2026-08-23 hardware run ran at ~25 deg.
#define VR_STEREO_MAX_EYE_DIVERGE_DEG 2.0f

unsigned VR_StereoRejectCount();
// Stereo pairs refused at pre-submit by the native-object verdict
// (half-native, or the two eyes' finals disagree in PairWorld). Cumulative.
unsigned VR_NativePairRejectCount();
unsigned VR_StereoRejectReasonCount(int reason);
int      VR_LastStereoReject();          // -1 until something is rejected
int      VR_StereoFailsafeTripped();     // 1 once experimental features are off

// Test seam: score an arbitrary composed pair against the same invariants the
// production gate uses, without the fail-safe side effects. Lets the suite
// inject each named hardware failure as a mutant and require a rejection.
int VR_TestStereoPairAcceptable(const float eye0Origin[3], const float eye1Origin[3],
                                const float eye0Axis[9], const float eye1Axis[9],
                                float ipdMetres, float unitsPerMetre);

// Yaw (degrees) of the base camera the CURRENT XR pair was latched with, and
// the latch serial identifying that pair. Returns 0 before any latch exists.
// The steering-leak watchdog compares the rendered camera against this rather
// than against a live body yaw: the camera changes only at pair boundaries.
int VR_GetPairBaseYawDeg(float* yawDeg, unsigned* latchSerial);

// Test seam: pair-base latch accounting. latches = pairs that captured a
// fresh base camera; reuses = eye-1 renders that composed onto that latch.
// In steady stereo the two must track each other 1:1 -- a reuse count that
// falls behind means eyes are being composed on separate bases.
void VR_TestGetBaseLatchCounts(unsigned* latches, unsigned* reuses);

// Which eye the pair machine expects the NEXT 3D game frame to render:
// 0 at a pair boundary (or anywhere outside a pair), 1 between the eyes.
// The frame hook renders this eye rather than blindly alternating, so aborts,
// menus and session churn always restart cleanly at eye 0.
int VR_ExpectedEye();

// Compatibility wrapper: true exactly when VR_BeginFrameEx() says RENDER, so
// existing callers keep their meaning ("false" always meant "do not drive the
// camera this frame").
bool VR_BeginFrame();

// Per-eye camera, already converted into Quake's convention and composed on
// top of the game's own view. Pass the game's refdef values in; get the eye's
// values out. fovX/fovY are degrees, matching refdef's fov_x/fov_y.
//
// The returned fov is a CULLING SUPERSET ONLY: the smallest symmetric
// frustum containing this eye's asymmetric runtime frustum, plus a safety
// margin, so the engine's visibility code never discards geometry the real
// view can see. It is NOT the optics -- the actual projection is the eye's
// asymmetric frustum, installed over the engine's own perspective matrix at
// glLoadMatrix time (CODVR_MaybeReplaceProjection). This call also ARMS that
// interception for the coming scene render.
void VR_GetEyeCamera(int eye,
                     const float gameOrigin[3], const float gameAxis[9],
                     int viewW, int viewH,
                     float outOrigin[3], float outAxis[9],
                     float* fovXdeg, float* fovYdeg);

// Pixel offset that puts the flat 2D overlay's centre on this eye's optical
// axis. Under the asymmetric projection the image centre is ~15 deg off the
// view axis, mirrored between the eyes; drawn unshifted the same HUD element
// would diverge by twice that and be unfusable. Zero until a 3D eye has been
// driven. dx positive = right, dy positive = up (GL viewport space), in the
// engine's own pixels (before supersample scaling).
void VR_GetHudPrincipalShift(int eye, int viewW, int viewH, int* dxPx, int* dyPx);

// Scales how far physical head translation moves the in-game camera.
// 1.0 = physically correct. Applied to the head-centre delta only -- never to
// the inter-eye offset, so stereo depth is independent of it.
void VR_SetPositionalGain(float gain);

// Extra degrees added to each side of the symmetric culling fov beyond the
// exact superset of the eye frustum. Costs a little overdraw, protects
// against the engine recomputing fov_y from the viewport aspect.
void VR_SetCullMargin(float deg);

// Hand the finished game frame to the pair machine. fbWidth/fbHeight describe
// the back buffer.
//
// Each game frame still renders ONE eye across the FULL back buffer (the
// engine consumes its entity list on the first scene render, so drawing both
// eyes in one frame required the fragile scene replay). But the two eyes of
// one XR frame are now a PAIR sharing a single located view set:
//
//   first eye of the pair  (renderedEye 0): capture + vignette ONLY. The XR
//     frame stays open -- no layers submitted, no xrEndFrame -- and control
//     returns so the engine immediately renders the other eye.
//   second eye of the pair (renderedEye 1): capture + vignette, then verify
//     both captures belong to THIS XR frame (serial check), copy both to the
//     swapchains, submit exactly one stereo projection layer, xrEndFrame once.
//
// Any failure (wrong eye, capture failure, swapchain failure, serial
// mismatch, dead back buffer) aborts the pair: the XR frame is ended with
// zero layers and the machine resets to "need eye 0". A mixed-generation
// stereo pair is never submitted.
//
// renderedEye : which eye the back buffer currently holds.
// stereo=false: nothing 3D was drawn (menu, loading screen). The frame goes to
//               BOTH eyes, aspect-preserved, in ONE game frame -- flat
//               presentation keeps the old one-XR-frame-per-game-frame shape,
//               and a half-complete 3D pair is flushed first so a gameplay eye
//               is never paired with a panel.
void VR_SubmitFrame(int fbWidth, int fbHeight, bool stereo, int renderedEye);

// Copy the finished frame into one eye's texture, at the headset's own per-eye
// resolution. Called by the frame hook once the 2D pass has been drawn over the
// scene. baseW/baseH describe the game's own buffer, used only when the
// supersampled target is unavailable. letterbox fits a flat 2D screen at its
// own aspect instead of stretching it across the eye.
bool VR_CaptureEye(int eye, int baseW, int baseH, bool letterbox);

// Colour-only processing of one completed eye. No depth, reprojection, or
// sampling from the other eye; unavailable shaders leave the capture intact.
struct VRPostprocessConfig {
    int enabled;
    float sharpen, bloom, contrast, saturation, shadowLift;
};
void VR_SetPostprocessEnabled(int on);
void VR_SetPostprocessSharpen(float amount);    // 0..0.75
void VR_SetPostprocessBloom(float amount);      // 0..0.20
void VR_SetPostprocessContrast(float amount);   // 0..0.10
void VR_SetPostprocessSaturation(float amount); // 0.80..1.20
void VR_SetPostprocessShadowLift(float amount); // 0..0.05, black remains black
void VR_GetPostprocessConfig(VRPostprocessConfig* out);
bool VR_PostprocessEye(int eye);
void VR_TestFailNextPostprocess();

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
// Per-axis version. The scene target matches the runtime EYE aspect, not the
// engine's square refdef, so the two factors differ and rect remapping must
// use both. Returns 1,1 when the target is not in use.
void  VR_SupersampleXY(float* sx, float* sy);
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
float VR_WorldScale();
float VR_PositionalGain();

// Latched neutral references, for the controller layer: returns a validity
// mask (bit0 = yaw valid, bit1 = midpoint valid). yawRad is the neutral yaw;
// headMidM the neutral head midpoint in Quake-mapped XR metres. Controller
// poses MUST be measured against these same references or the hands drift
// from the head by exactly the recenter correction.
int VR_GetNeutralRefs(float* yawRad, float headMidM[3]);

// Current head midpoint from the last located views (Quake-mapped XR metres).
int VR_GetHeadRoomMid(float outM[3]);

// Seated-height comfort: metres ADDED to the head's vertical translation.
void VR_SetHeightAdjust(float metres);

// ---- menu safe area ----
// The flat 2D panel (menus, briefings) is fitted inside the BINOCULARLY
// visible part of the eye's fov times this scale, so content is never pushed
// past what the lens shows. 1.0 fills the visible box edge-to-edge.
void VR_SetMenuScale(float scale01);
// VR UI SAFE AREA (brief 2026-09-04, P3). One inset applied to EVERY 2D
// surface -- flat menus and loading screens (the angular panel fit above)
// and the gameplay HUD band (dllmain's HudRemap) -- aspect preserved,
// centred. `scale` multiplies the existing fits; `safeX` / `safeY` bound the
// usable fraction of the visible rectangle per axis. World rendering keeps
// the full eye. Defaults 0.92 / 1.0 / 1.0: ~92 % of the previous extent.
void  VR_SetUiSafeArea(float scale, float safeX, float safeY);
void  VR_GetUiSafeArea(float* scale, float* safeX, float* safeY);
float VR_UiInsetX();    // scale * safeX
float VR_UiInsetY();    // scale * safeY
#define VR_UI_SCALE_DEFAULT 0.92f
// Optional convergence: shifts each eye's panel nasally as if the panel hung
// this many metres away. 0 leaves it at infinity (identical images).
void VR_SetMenuDistance(float metres);
// What the last flat capture produced, degrees: content half-angles vs the
// visible half-angles it had to stay inside. Returns 0 before any flat frame.
int VR_GetMenuSafeArea(float* contentHalfXDeg, float* contentHalfYDeg,
                       float* visHalfXDeg, float* visHalfYDeg);

// Per-eye world-pass coverage from the projection-state notes (the same data
// the optics check judges), for the VR INPUT / WEAPON CHECK verdicts.
void VR_GetWorldPassCoverage(int eye, int* known, int* vpOk, int* scOk);

// Re-zero the headset: the player's current position and facing become neutral.
// Seated tracking drifts and people start sessions facing the wrong way, so
// this is bound to a key.
void VR_Recenter();

// How far the head is turned away from the game's aim direction, in degrees
// (positive yaw left, positive pitch up). Drives head-aim steering and the
// comfort vignette.
void VR_GetHeadAimOffset(float* yawDeg, float* pitchDeg);

// The yaw of the RENDERED camera (the composed basis the last eye-0 camera
// actually used), world degrees. The steering-leak detector watches this.
int VR_GetRenderedViewYawDeg(float* yawDeg);

// The head's OWN yaw (neutral-cancelled, before any game-aim composition),
// degrees. Subtracting body+this from the rendered yaw isolates whatever the
// composition added -- which must be nothing but the selected basis.
int VR_GetHeadLocalYawDeg(float* yawDeg);

// ---- render/submit pose consistency (Phase-7 probe) ----
// The layer relocates the views at the pair's own displayTime right before
// xrEndFrame and measures, per eye, how far the submitted pose disagrees
// with the runtime's current answer for that instant. Runs once by itself
// (pair serial 240) and logs a PASS/MISMATCH block; the seam arms it for the
// next stereo submit so the offline suite can drive it deterministically.
void VR_TestArmConsistencyCheck();
int  VR_TestGetRenderSubmitConsistency(float posM[2], float rotDeg[2]);

// The flat-panel (menu) pose currently in force for one eye, plus whether it
// is pinned. The startup contract under test: upright (+Y), zero pitch/roll,
// yaw = the head's horizontal azimuth, never mirrored -- for ANY head
// attitude, including garbage attitudes before tracking.
int  VR_TestGetPanelPose(int eye, float quat[4], float posM[3], int* pinned);

// Virtual motion the player cannot physically feel -- engine units per second
// of travel and degrees per second of turning. This, not head movement, is what
// causes simulator sickness, so the comfort vignette keys off it.
void VR_GetPlayerMotion(float* speed, float* turnRateDeg);

// The per-eye texture and its size, so the comfort layer can draw an overlay
// into the finished frame before it reaches the compositor.
// VR_EyeTextureSize keeps its old signature (eye 0's size) because existing
// code links against it; new code should ask per eye -- the runtime is allowed
// to recommend different sizes per view.
unsigned VR_EyeTexture(int eye);
void     VR_EyeTextureSize(int* w, int* h);
void     VR_EyeTextureSizeFor(int eye, int* w, int* h);

// ------------------------------------------------------------ perf metrics
//
// The numbers that decide whether the frame loop is healthy. With paired
// rendering, xrWaitFrame paces the game to TWO game frames per display
// interval, so the interesting comparison is game cadence against 2x the XR
// target -- and the single most important new number is the eye-pair
// separation: the wall-clock time between the two captures of one XR frame.
// If the engine really runs unthrottled between waits, that separation is a
// few milliseconds, not a display interval.

// Derived pair-throughput verdict: can the engine feed two fresh eyes per
// display interval? (game cadence >= 1.9x target = PASS, >= 1.6x = MARGINAL.)
// Diagnostic only -- nothing acts on it automatically.
enum VRPairVerdict {
    VR_PAIR_UNKNOWN  = 0,    // no window measured yet, or no 3D pairs ran
    VR_PAIR_PASS     = 1,
    VR_PAIR_MARGINAL = 2,
    VR_PAIR_FAIL     = 3
};

struct VRPerfStats {
    float    xrTargetHz;      // from predictedDisplayPeriod; 0 until known
    float    presentHz;       // XR frames begun (wait/begin) per second, last window
    float    gameHz;          // game frames driven per second, last window
    float    submitHz;        // xrEndFrame calls carrying layers per second
    float    freshHz[2];      // fresh 3D captures per eye per second, last window
    float    pairSepMeanMs;   // mean eye0->eye1 capture separation, last window
    float    pairSepMaxMs;    // worst separation in the window
    int      pairVerdict;     // VRPairVerdict for the last window
    unsigned abortedPairs;    // pairs discarded half-complete (cumulative)
    unsigned skippedFrames;   // frames the runtime said not to render (cumulative)
    unsigned discardedFrames; // XR_FRAME_DISCARDED seen at xrBeginFrame (cumulative)
    unsigned locateFailures;  // xrLocateViews failed or poses invalid (cumulative)
    unsigned xrErrors;        // acquire/wait/release/endFrame failures (cumulative)
    int      valid;           // 1 once at least one measurement window completed
};
void VR_GetPerfStats(VRPerfStats* out);

// Testing seam: exposes the OpenXR-quaternion to Quake-viewaxis conversion
// so it can be verified offline, without a headset.
void VR_TestQuatToAxis(const float quat[4], float outAxis[9]);

// Testing seam: composes a head rotation onto the game's view basis, the
// operation that makes mouse aim and head free-look coexist.
// This is the OLD in-frame composition, kept byte-identical because existing
// tests define its behaviour. The renderer itself now uses the roll-stable
// composition below.
void VR_TestComposeAxis(const float head[9], const float game[9], float out[9]);

// Testing seam: the roll-stable view composition actually used per frame.
// Head yaw stays a rotation about gravity and the game's pitch is applied
// about the view's own lateral axis, so no combination of mouse pitch and
// head yaw can invent horizon roll. With the head centred the output is the
// game's full aim exactly.
void VR_TestComposeView(const float headLocal[9], const float gameAxis[9], float out[9]);

// Testing seams: the motion filters feeding the comfort vignette. Pure
// functions of (current smoothed value, instantaneous sample, dt); the turn
// filter rejects single-frame yaw steps (snap turn, respawn) instead of
// reading them as a violent sustained rotation.
float VR_TestFilterSpeed(float current, float distUnits, float dtSeconds);
float VR_TestFilterTurn(float current, float dyawDegrees, float dtSeconds);

// Testing seam: tear down every XR object and reset ALL renderer state --
// neutrals, retained eye records, pair machine, counters, failure latches,
// retry throttle -- so scenario tests start from a truly cold module.
void VR_TestResetAll();

// Testing seams for the pair machine. Observation only, except the last two,
// which inject the faults the offline suite cannot produce any other way.
int      VR_TestPairPhase();            // 0 idle, 1 awaiting eye 0, 2 awaiting eye 1
unsigned VR_TestFrameSerial();          // generation of the current/last located XR frame
int      VR_TestEyeRecordSerial(int eye);   // serial the eye's capture belongs to; -1 if none
void     VR_TestFailNextCaptures(int n);    // force the next n VR_CaptureEye calls to fail
void     VR_TestCorruptEyeSerial(int eye);  // age one record so the pair guard must fire

// ------------------------------------ projection-override observation seams
//
// The armed eye is production state (dllmain's glMultMatrix diagnostics read
// it too); the rest exist so the safe suite can drive the exact decision path
// the GL wrappers use, without a GL context.

// Which eye's projection the interception layer would install right now:
// 0/1 while a 3D scene render is in flight, -1 otherwise.
int VR_ProjectionArmedEye();

// Arm/disarm directly with explicit tangents (the production path arms from
// VR_GetEyeCamera; this seam lets the DLL suite exercise the real GL wrappers
// without a session).
void VR_TestArmProjection(int eye, float tanL, float tanR, float tanU, float tanD);
void VR_TestDisarmProjection();

// Runs the REAL gate+replace on a candidate matrix (in place). Returns 1 if
// it was replaced. This is exactly what the glLoadMatrix wrappers call.
int VR_TestProjectionDecision(float m[16]);

// Lifetime count of projections actually replaced for an eye.
unsigned VR_TestProjectionReplacedCount(int eye);

// The tangents the last VR_GetEyeCamera stored for an eye (tanL,tanR,tanU,tanD).
void VR_TestGetEyeTangents(int eye, float out[4]);

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



