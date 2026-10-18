// ---------------------------------------------------------------------------
// LIVE WIRING: real OpenXR controller poses -> the pair-latch -> the
// GL-surface hand render targets.
// ---------------------------------------------------------------------------
//
// Every piece this module connects already existed and was already tested in
// isolation: vr_input samples the controllers once per XR pair, vr_objects
// carries a latch safely across the two eyes, and vr_handpose/vr_glsurf
// recover the authored palm pose from each eye's own vertices and turn a
// desired pose into a GL delta. What did not exist was the glue between
// "here is a controller pose" and "here is what GLSurf_SetHandTarget wants" --
// that is this file, and nothing else.
//
// COORDINATE SPACES (see docs/COORDINATE-SPACES.md)
//
// GLSurf_SetHandTarget takes a RELATIVE offset + rotation, added to whatever
// palm pose vr_handpose recovers FRESH from that eye's own draw -- not an
// absolute placement. That is deliberate (docs/COORDINATE-SPACES.md's stereo
// rule: an absolute target is not eye-invariant, a relative offset is), and
// it resolves a chicken-and-egg problem for free: the offset must be decided
// once per pair, before either eye has drawn anything, yet "where the
// authored pose actually is" is only known AFTER a draw runs. This module
// breaks that circle by using the LAST pair's recovered authored pose
// (GLSurf_GetHandSolve) as this pair's baseline estimate. Because the loop
// re-anchors to real vertex data every single pair (~72-90 Hz) rather than
// integrating a velocity, a one-pair-old baseline never accumulates drift --
// it only ever lags by the animation's actual per-pair motion, which is
// small at that rate.
//
// The controller's own pose is converted into the same space that baseline
// lives in (the space viewmodel vertices are submitted in) via
// VRM_WorldToLocalVec/Axis + VRM_QuakeToGLEyeVec/Axis (vr_math.h), using the
// head pose THIS pair was actually located with -- not the recentred neutral
// reference, because a viewmodel is baked relative to wherever the camera
// really is this frame, not to a fixed "forward".
//
// CAMERA SAFETY
//
// This module never receives refdef.vieworg/viewaxis, never receives an Xr*
// handle, and never calls into GL or OpenXR. Its entire input is plain
// floats vr.cpp already has in hand at the pair boundary, and its entire
// output is calls into vr_objects/vr_glsurf, both of which are load-bearing
// members of the "never touches the camera" set documented in vr_glsurf.h.
#pragma once

#include "vr_input.h"

// Strength 0..1. Zero restores direct handling. Recoil reduction scales
// the authored firing motion while braced; engine damage/spread are untouched.
void VRNH_SetHandling(float stability, float recoilReduction);
// SUPPORT POSE LOCK (2026-09-06): while the two-hand support is attached the
// LEFT hand rides the weapon in the game's own authored hold (no clipping);
// the left controller still drives attach/release and reload intent.
void VRNH_SetSupportPoseLock(int on);
int  VRNH_SupportPoseLock();
// TRIGGER HAND LOCK (2026-09-06): the RIGHT hand is drawn where the game's
// animation holds it on the weapon whenever the weapon is native -- rigid
// with the gun through bar alignment, stabilisation, kick and the sighted
// animation. The controller still drives the weapon through the palm
// calibration; only the rendered hand rides the result.
void VRNH_SetTriggerHandLock(int on);
int  VRNH_TriggerHandLock();
void VRNH_GetHandling(float* stability, float* recoilReduction);
void VRNH_SetEnabled(int on);
int  VRNH_Enabled();

// Controller -> palm calibration, per OpenXR hand (VRI_LEFT / VRI_RIGHT).
// offsetM: metres, expressed in the grip's own frame. rotDeg: Quake
// pitch/yaw/roll, applied in the grip's frame. Zero (the default) is the
// honest answer offline: this is a property of how a specific player grips a
// specific physical controller and cannot be known without a headset (see
// docs/NATIVE-HANDS-STATUS.md's hardware-only acceptance list, item 1).
void VRNH_SetCalibration(int xrHand, const float offsetM[3], const float rotDeg[3]);

// THE ANATOMICAL CALIBRATION (brief 2026-09-03, section 7). One canonical
// definition per hand and per rig, derived from the shipped assets against
// the OpenXR grip convention (src/vr_palm_calibration.inc): the palm BONE
// frame expressed in the controller grip frame, plus the grip-origin ->
// bone-origin offset. Used by default; `VRNH_SetCalibrationTable(0)` falls
// back to the raw grip (the pre-2026-09-03 behaviour), and explicit rows
// from config replace the table for that hand. The Euler/offset values from
// VRNH_SetCalibration are operator TRIMS applied on top of either.
void VRNH_SetCalibrationTable(int on);
int  VRNH_CalibrationTable();

// THE 2026-09-04 HARDWARE FIX. On hardware the anatomical calibration placed
// BOTH rendered hands "backwards and upside down" (knuckles pointed at the
// user, palm faced up), left/right preserved -- a 180 deg rotation about the
// grip-local X axis (equivalently, about the palm's thumb/lateral axis:
// knuckles and palm-out negate, thumb side kept, determinant +1, no mirror).
// The correction is applied to the HAND RENDER TARGET ORIENTATION ONLY;
// positions and, crucially, the value the gun anchor reads
// (VRNHHandDebug::targetAxisEyeish) are untouched, so gun / barrel / aim are
// byte-identical. Default ON; 0 restores the pre-fix (flipped) alignment.
// INERT since 2026-09-04d: the palm calibration is derived against the
// OpenXR grip convention as specified, so there is no runtime flip to
// enable. Kept so an older codvr.cfg parses and is told so in the log.
void VRNH_SetPalmFlip(int on);
int  VRNH_PalmFlip();

// RIGHT/LEFT HAND VISUAL TRIM (brief 2026-09-04e, section 6): a rotation
// applied to the RENDERED hand only, about the palm pivot, in the palm's
// own axes (pitch, yaw, roll, degrees). The physical target -- and so the
// weapon, the bore, the aim and the other hand -- is untouched. Ships at
// identity; it exists so a hold that reads wrong on hardware can be turned
// without touching the calibration.
void VRNH_SetHandVisualTrim(int xrHand, const float rotDeg[3]);
void VRNH_GetHandVisualTrim(int xrHand, float rotDeg[3]);
// The gun anchors on the corrected limb basis (default 0: weapon frozen on
// the uncorrected basis, byte-identical to the hardware-tested candidate).
void VRNH_SetGunFollowsLimbFix(int on);
int  VRNH_GunFollowsLimbFix();
void VRNH_SetCalibrationRows(int xrHand, const float rows[9]);   // null = table
// The rows/offset in effect for this hand and rig (rig index as
// GLSurfHandSolve::lastRig; -1 = the default hands_new4 tables).
int  VRNH_GetEffectiveCalibration(int xrHand, int rig, float rows[9], float offsetM[3]);
// The anatomical directions (knuckles, thumb side, palm-out) in the palm
// BONE frame for this hand and rig, for the calibration fixture.
int  VRNH_GetCalibrationAnatomy(int xrHand, int rig, float k[3], float t[3], float n[3]);

// The WEAPON's own controller -> rear-grip calibration, in the right grip's
// frame. Separate from the palm calibration above ON PURPOSE: the right
// controller determines the desired palm pose and the desired gun rear-grip
// pose INDEPENDENTLY, and they coincide on screen only because both default
// to the same grip frame. Change one and only that object moves. The gun does
// not inherit the hand's transform and the hand does not inherit the gun's --
// not in the render matrices (proved by the three-object separation gate) and
// not in the derivation either.
void VRNH_SetGunCalibration(const float offsetM[3], const float rotDeg[3]);

// Bounded grace, seconds, a hand keeps driving the render through a tracking
// dropout (VRI_TRACK_LOST) before disengaging. Default 0.5s.
void VRNH_SetTrackingGrace(float seconds);

// Called ONCE per XR pair, at the pair boundary, after VRInput_Sync has run.
// vr.cpp is the only production caller. All inputs are already plain data.
//
//   pairSerial               : this pair's serial (vr.cpp's g_xrFrameSerial).
//   in                       : VRInput_State(), already synced for this pair.
//   headRoomPosM/headRoomAxis: this pair's OWN located head pose -- metres,
//                              Quake axis convention (rows fwd/left/up).
//   headValid                : 0 leaves both hands disengaged this pair
//                              (fail closed: no head, no placement).
//   now                      : seconds, for the tracking-loss grace window.
//
// Opens the vr_objects/GLSurf latches for this pair, latches the head and
// both grips, and -- for each hand that is enabled, tracked (or within
// grace) and has a usable prior authored recovery -- computes and installs
// the GLSurf_SetHandTarget offset/rotation that will carry this pair's
// controller pose onto the render.
void VRNH_OnPairBegin(unsigned pairSerial, const VRInputState* in,
                      const float headRoomPosM[3], const float headRoomAxis[9],
                      int headValid, double now);

// The production form: BOTH eyes' located positions (room metres, Quake
// axes). "Eye space" differs between the eyes by the interpupillary offset,
// so every absolute target is expressed in the head-MIDPOINT frame and each
// eye's own offset is installed in vr_glsurf for the delta to subtract. The
// three-argument form above is the monoscopic wrapper (both eyes at the
// given head position, zero offsets) that fixtures and the flat probe use.
void VRNH_OnPairBeginEyes(unsigned pairSerial, const VRInputState* in,
                          const float eyeRoomPosM[2][3], const float headRoomAxis[9],
                          int headValid, double now);

// ------------------------------------------------ the pre-submit verdict
//
// TEMPORAL CORRECTNESS (project-lead review 2026-08-29, sections 4-6).
//
// One XR pair is two GAME frames: eye 0's scene renders, then eye 1's, then
// the pair is submitted. At the instant eye 0's hand draw is transformed,
// nothing can know whether eye 1's solve will succeed -- so eye 0 renders
// PROVISIONALLY, eye 1 renders, and before xrEndFrame the pair is judged as a
// whole from what each eye ACTUALLY did (vr_glsurf's per-eye apply records):
//
//   * an object one eye applied a native delta to and the other did not is a
//     HALF-NATIVE pair: refused;
//   * an object both eyes applied is reconstructed from each eye's final
//     pose back into PairWorld (room metres, Quake axes); the two must agree
//     within 0.5 mm / 0.05 deg, or the pair is refused (DISAGREE).
//
// A refused pair is ended with zero layers by vr.cpp's existing abort path
// (the compositor reprojects the previous frame), and the failing object is
// held vanilla for the following pair (cooldown) so a persistent refusal
// degrades to alternating vanilla/abort rather than abort/abort. Per-eye
// authored poses are NOT compared: the engine may advance the animation
// between the two game frames, and each eye's delta absorbs its own.
enum { VRNH_OBJ_R = 0, VRNH_OBJ_L = 1, VRNH_OBJ_GUN = 2, VRNH_OBJS = 3 };
// VANILLA_FALLBACK (brief 2026-09-03, section 6): an object that was
// ENGAGED this pair and that both eyes drew but neither could own -- a
// coherent vanilla pair in the middle of native ownership, the centred
// viewmodel flashing back. It is refused (the compositor holds the previous
// native pair) up to VRNH_FALLBACK_ABORT_BUDGET consecutive times per
// object; a refusal that persists past the budget is then accepted as a
// coherent vanilla pair, so a hand that really has stopped solving degrades
// to vanilla instead of freezing the display. No stale pose is ever shown.
// OWNER_CONFLICT (brief 2026-09-04, section 3): native/stale AND vanilla
// weapon geometry in one eye-scene -- two weapons. Refused within the same
// budget as the vanilla fallback, then accepted so a persistent conflict
// (a modded weapon) degrades instead of freezing the display.
enum { VRNH_VERDICT_OK = 0, VRNH_VERDICT_HALF_NATIVE = 1, VRNH_VERDICT_DISAGREE = 2,
       VRNH_VERDICT_VANILLA_FALLBACK = 3, VRNH_VERDICT_OWNER_CONFLICT = 4 };
#define VRNH_FALLBACK_ABORT_BUDGET 3
const char* VRNH_VerdictName(int verdict);
const char* VRNH_ObjName(int obj);

struct VRNHObjVerdict {
    int   drew[2];               // the eye drew the object under this serial
    int   applied[2];            // ...and a native delta reached GL
    float pairWorldPosM[2][3];   // final anchor per eye, PairWorld (metres)
    float posErrMm;              // |eye0 - eye1|; -1 unless both applied
    float rotErrDeg;             // basis angle between the eyes; -1 likewise
    int   verdict;               // VRNH_VERDICT_*
};
struct VRNHPairVerdict {
    unsigned       pairSerial;
    int            ok;           // 1 = the pair may be submitted
    int            reason;       // VRNH_VERDICT_* of the first failing object
    int            objIndex;     // that object, -1 when ok
    VRNHObjVerdict obj[VRNH_OBJS];
};
// Judge the pair whose serial is about to be submitted. Returns ok. Called
// by vr.cpp exactly once per stereo submit, after the camera stereo gate.
int VRNH_PairPreSubmit(unsigned pairSerial, VRNHPairVerdict* out);
void VRNH_GetLastVerdict(VRNHPairVerdict* out);

struct VRNHPairStats {
    unsigned checked;            // stereo pairs judged
    unsigned accepted;
    unsigned rejected;
    unsigned rejectHalfNative;
    unsigned rejectDisagree;
    unsigned cooldownPairs;      // pairs an object was held vanilla after a reject
    unsigned objBothApplied[VRNH_OBJS];   // pairs in which both eyes applied
    // Ownership counters (brief 2026-09-03, section 3).
    unsigned pairNative;         // accepted pairs in which every engaged object applied in both eyes
    unsigned pairFailClosed;     // pairs refused at pre-submit (any reason)
    unsigned mixedEyeRejected;   // ...of which half-native (one eye native, one vanilla)
    unsigned vanillaFallbackAborted;   // engaged object, both eyes vanilla: refused (within budget)
    unsigned vanillaFallbackAccepted;  // ...accepted as coherent vanilla (budget exhausted)
    unsigned gunOwnerConflictPairs;    // pairs in which an eye-scene drew native AND vanilla weapon
    unsigned rejectOwnerConflict;      // ...of which refused (within budget)
};
void VRNH_GetPairStats(VRNHPairStats* out);

#define VRNH_PAIRWORLD_POS_TOL_MM 0.5f
#define VRNH_PAIRWORLD_ROT_TOL_DEG 0.05f

// ------------------------------------------------ the gun and the left grab
//
// Config for the weapon-follow layer. Both default ON: there must be no
// hidden mode the user has to discover -- a supported weapon in a tracked
// right hand IS the engagement condition.
void VRNH_SetTwoHandEnabled(int on);
void VRNH_SetReloadEnabled(int on);
// 0 off, 1 gestures plus button fallback, 2 gestures for measured weapons
// (the button remains available for an unknown/unmeasured model).
void VRNH_SetReloadMode(int mode);
int  VRNH_ReloadMode();
int  VRNH_PhysicalReloadAvailable();

// Pair-level stereo eligibility tolerances: how closely the two eyes must
// agree, in the common (submitted) frame, that they recovered the SAME
// physical hand before native ownership may become active for that pair.
#define VRNH_PAIR_POS_TOL_U   (0.0005f * 39.3701f)   // 0.5 mm, game units
#define VRNH_PAIR_ROT_TOL_DEG 0.05f

// Two-hand support states for the rendered LEFT hand.
enum { VRNH_2H_LEFT_FREE = 0, VRNH_2H_CANDIDATE = 1, VRNH_2H_ATTACHED = 2 };
// Why the last attachment ended.
enum { VRNH_2H_DETACH_NONE = 0, VRNH_2H_DETACH_GRIP = 1, VRNH_2H_DETACH_RESIDUAL = 2,
       VRNH_2H_DETACH_LEFT_LOST = 3, VRNH_2H_DETACH_WEAPON = 4, VRNH_2H_DETACH_DISABLED = 5 };
const char* VRNH_TwoHandDetachName(int reason);

// TWO-HAND TUNING (brief 2026-09-04, sections 9-13). The rifle is a rigid
// bar held at two points: the rear grip pinned to the RIGHT controller, the
// support point sliding along the legal fore-end segment to the LEFT.
//   grabRadiusM : attach when the left grip is within max(model radius,
//                 this) of the segment AND the left grip is squeezed --
//                 immediately, no dwell (deliberate but forgiving).
//   releaseM    : once attached, release only on grip release or when the
//                 residual exceeds max(1.6 x radius, this) -- hysteresis.
//   slideHalfM  : half-length of the legal slide about the measured support point.
//   blendS      : time constant of the attach / release transition blend;
//                 the blend is armed ONLY by an attach or a release and lasts
//                 3 time constants. Steady attached control is rigid.
void  VRNH_SetTwoHandTuning(float grabRadiusM, float releaseM, float slideHalfM, float blendS);
void  VRNH_GetTwoHandTuning(float* grabRadiusM, float* releaseM, float* slideHalfM, float* blendS);
#define VRNH_2H_GRAB_RADIUS_M 0.12f
#define VRNH_2H_RELEASE_M     0.25f
#define VRNH_2H_SLIDE_HALF_M  0.15f
#define VRNH_2H_BLEND_S       0.08f

// -------------------------------------------------------------- diagnostics

// Why a hand is not driving the render this pair. Enumerated so a hardware
// log names the FIRST broken stage instead of showing a bare engaged=0.
enum {
    VRNH_HAND_ENGAGED = 0,
    VRNH_HAND_DISABLED,        // native hands switched off
    VRNH_HAND_NO_HEAD,         // pair carried no valid head pose
    VRNH_HAND_NO_GRIP,         // controller grip pose invalid this pair
    VRNH_HAND_TRACKING,        // not ACTIVE and past the tracking grace
    VRNH_HAND_NO_BASELINE,     // no fresh authored palm solve to engage on
    // A participating eye has no usable authored baseline (it drew the hand
    // recently and could not recover it), so BOTH eyes fall back to vanilla
    // for this pair. Never one eye each: a half-native pair is two
    // irreconcilable images of one hand. (Predictive, from the previous
    // pair's evidence; the pre-submit verdict is the hard guarantee.)
    VRNH_HAND_STEREO_DISAGREE,
    // The previous pair was refused at pre-submit because of this hand; it
    // is held vanilla for one pair so the player sees a consistent vanilla
    // pair rather than a second aborted one.
    VRNH_HAND_PAIR_COOLDOWN,
};
const char* VRNH_HandReasonName(int reason);

// Why the weapon layer is not driving the gun this pair.
enum {
    VRNH_GUN_DRIVING = 0,
    VRNH_GUN_DISABLED,
    VRNH_GUN_HAND_NOT_ENGAGED, // right hand not engaged/targeted
    VRNH_GUN_NO_PALM_BASELINE, // right palm baseline stale/absent
    VRNH_GUN_NO_BODY,          // no fresh authored gun-body solve
    VRNH_GUN_UNKNOWN_WEAPON,   // vertex-count key matched no shipped model
    VRNH_GUN_PAIR_COOLDOWN,    // held vanilla one pair after a refused pair
};
const char* VRNH_GunReasonName(int reason);

struct VRNHHandDebug {
    int   engaged;           // driving GLSurf_SetHandTarget this pair
    int   reason;            // VRNH_HAND_* when engaged == 0
    int   trackState;        // VRI_TRACK_*
    int   haveTarget;        // targetEyeish is meaningful this pair
    float targetEyeish[3];   // controller-derived absolute target, CoD units
    float targetAxisEyeish[9];
    int   haveGrip;          // gripEyeish is meaningful this pair
    float gripEyeish[3];     // raw grip in the render space, CoD units
    int   haveBaseline;      // lastAuthored* came from a real prior recovery
    float lastAuthoredPos[3];
    // Pair-level stereo eligibility (predictive): every participating eye has
    // a fresh authored baseline. The two error fields are kept for the log
    // format and hold the LAST pre-submit verdict's PairWorld disagreement for
    // this hand (-1 until both eyes applied a delta in some judged pair).
    int   pairEligible;
    float pairPosErrU;
    float pairRotErrDeg;
};

struct VRNHGunDebug {
    int      targetInstalled;   // gun target driven this pair
    int      reason;            // VRNH_GUN_* when targetInstalled == 0
    int      vertexCount;       // identified weapon batch key (0 = none yet)
    float    desiredGripEyeish[3];
    int      twoHandState;      // VRNH_2H_*
    float    supportT;          // slide parameter while attached
    float    supportResidualU;  // |support point - left grip|, CoD units
    // Two-hand telemetry (brief 2026-09-04, section 14), low-rate readers.
    float    grabDistanceU;     // left grip -> legal support segment under the ONE-hand pose
    float    angularResidualDeg;// bore vs the physical R->L line while attached (0 = rigid bar)
    unsigned attachCount;
    unsigned detachCount;
    int      lastDetachReason;  // VRNH_2H_DETACH_*
    int      reloadState;       // VRRL_* (vr_reload.h)
    unsigned reloadTaps;        // commits delivered to the game so far
    // The physical barrel this pair, room frame (Quake axes, unit), handed
    // to the aim model as the fire direction (brief 2026-09-03, section 10).
    int      barrelValid;
    float    barrelRoom[3];
    float    barrelUpRoom[3];
    // Which axis the fire direction came from, and where the shot leaves the
    // weapon (brief 2026-09-04d, P1).
    int      boreFromSightLine;   // 1 = the derived bore; 0 = the body axis fallback
    int      muzzleValid;
    float    muzzleEyeish[3];
    int      leftRidesGun;        // support pose lock in effect this pair
    int      rightRidesGun;       // trigger hand lock in effect this pair
};

struct VRNHDebug {
    int           enabled;
    unsigned      pairs;
    VRNHHandDebug hand[VRI_HANDS];   // indexed VRI_LEFT / VRI_RIGHT
    VRNHGunDebug  gun;
    // This pair's eye positions in the head-midpoint eyeish frame, CoD
    // units -- what vr_glsurf subtracts per eye from every absolute target.
    float         eyeOffEyeish[2][3];
};
void VRNH_GetDebug(VRNHDebug* out);

// Testing seam: full cold reset (config included).
void VRNH_TestReset();
