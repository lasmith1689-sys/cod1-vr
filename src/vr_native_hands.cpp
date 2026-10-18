#include "vr_native_hands.h"

#include <string.h>
#include <math.h>

#include "vr_math.h"
#include "vr_handling.h"
#include "vr_objects.h"
#include "vr_handpose.h"
#include "vr_glsurf.h"
#include "vr_reload.h"
#include "vr_binds.h"
#include "vr_weapon.h"
#include "vr_menu.h"
#include "vr_palm_calibration.inc"

// ---------------------------------------------------------------------------
// Physical OpenXR hand -> RENDERED hand identity.
//
// PINNED by the 2026-08-25 live sweep (see vr_handpose.h / vr_glsurf.h): the
// physical RIGHT controller drives the RENDERED trigger hand (GLSC_HAND_R),
// physical LEFT drives RENDERED support (GLSC_HAND_L). That is the natural
// anatomical mapping, but the index spaces involved do NOT agree with each
// other numerically:
//
//   vr_input.h   : VRI_LEFT = 0, VRI_RIGHT = 1     (physical-hand order)
//   vr_objects.h : VROBJ_HAND_R = 0, VROBJ_HAND_L = 1   (rendered-side order)
//   vr_glsurf.h  : GLSC_HAND_L, GLSC_HAND_R (unordered enum members)
//
// so a loop written "hand == 0 means right" is a coin flip depending on
// which of the three headers it was thinking about. This project has already
// paid for exactly that kind of mix-up twice (see docs/NATIVE-HANDS-STATUS.md
// section 2). Every conversion between them goes through one of these two
// named functions and nowhere else.
static int VriToGLClass(int xrHand)   { return xrHand == VRI_RIGHT ? GLSC_HAND_R  : GLSC_HAND_L; }
static int VriToVrobjHand(int xrHand) { return xrHand == VRI_RIGHT ? VROBJ_HAND_R : VROBJ_HAND_L; }

// ON by default for the controller-hands candidate: a supported weapon in a
// tracked right hand IS the engagement condition -- there must be no hidden
// mode the user has to discover. `vr_native_hands 0` in codvr.cfg opts out.
// (Flat probe runs are untouched either way: VRNH_OnPairBegin only runs at
// the XR pair boundary, which a flat run never reaches.)
static int    g_enabled = 1;
static float  g_calOffsetM[VRI_HANDS][3];
static float  g_calRotDeg[VRI_HANDS][3];
static float  g_visualTrimDeg[VRI_HANDS][3];   // rendered hand only, see the header
void VRNH_SetHandVisualTrim(int xrHand, const float rotDeg[3])
{
    if (xrHand < 0 || xrHand >= VRI_HANDS) return;
    for (int k = 0; k < 3; ++k)
        g_visualTrimDeg[xrHand][k] = rotDeg ? rotDeg[k] : 0.0f;
}
void VRNH_GetHandVisualTrim(int xrHand, float rotDeg[3])
{
    for (int k = 0; k < 3; ++k)
        rotDeg[k] = (xrHand >= 0 && xrHand < VRI_HANDS) ? g_visualTrimDeg[xrHand][k] : 0.0f;
}
// The anatomical table (vr_palm_calibration.inc) is the default; explicit
// rows per hand override it.
static int    g_calTable = 1;
static float  g_calRows[VRI_HANDS][9];
static int    g_calRowsSet[VRI_HANDS];
// The 2026-09-04 hand-orientation correction (see vr_native_hands.h). ON by
// default; the hardware retest can disable it with vr_native_hands_palm_flip 0.
// Accepted for configuration compatibility and reported in the log; the
// corrected calibration supersedes both, so neither changes the pose.
static int    g_palmFlip = 0;
// Pair-level ownership bookkeeping for the pre-submit policy.
static int    g_pairEngaged[VRNH_OBJS];
static int    g_fallbackStreak[VRNH_OBJS];
// The GUN's own controller->rear-grip calibration, independent of the palm's.
//
// COORDINATED, NEVER PARENT/CHILD. The right controller determines two things
// separately: where the RIGHT PALM should be (g_cal*[VRI_RIGHT]) and where the
// WEAPON'S REAR GRIP should be (these). They coincide on screen because both
// default to the same grip frame -- not because one is computed from the
// other. Deriving the gun's anchor from the hand's TARGET, as this did until
// 2026-08-29, makes the palm calibration silently move the weapon too: the
// hand is then the gun's parent in everything but the render matrix.
static float  g_gunCalOffsetM[3];
static float  g_gunCalRotDeg[3];
// This pair's RIGHT grip in the render space -- the frame BOTH calibrations
// hang off. Stashed by the hand loop, consumed by GunPairTick.
static float  g_rightGripEyeish[3];
static float  g_rightGripAxisEyeish[9];
static int    g_rightGripValid;
static float  g_trackingGraceS = 0.5f;
static double g_lastGoodTime[VRI_HANDS];
static VRNHDebug g_dbg;

// ---- this pair's frame, for the pre-submit verdict ----------------------
static float    g_pairEyeRoomPos[2][3];   // each eye, room metres, Quake axes
static float    g_pairHeadAxis[9];        // the pair's shared head basis
static int      g_pairHeadValid;
static unsigned g_pairSerial;
static int      g_cooldown[VRNH_OBJS];    // pairs left to hold vanilla
static VRNHPairStats   g_pairStats;
static VRNHPairVerdict g_lastVerdict;
static int g_ownerConflictStreak;

// ---- the gun on the right controller, the left grab, the reload ----------
static int    g_twoHandEnabled = 1;
static int    g_reloadEnabled  = 1;
static int    g_reloadMode = 1;
static int    g_reloadAvailable;
static int    g_lastReloadVerts;
static int    g_2hState = VRNH_2H_LEFT_FREE;
static float  g_2hT     = 0.5f;
static int    g_lastGunVerts;
static float  g_appliedGunAxis[9];
static int    g_haveAppliedAxis;
static double g_lastGunNow;
static int   g_supportPoseLock = 1;
void VRNH_SetSupportPoseLock(int on) { g_supportPoseLock = on ? 1 : 0; }
int  VRNH_SupportPoseLock() { return g_supportPoseLock; }
static int   g_triggerHandLock = 1;
void VRNH_SetTriggerHandLock(int on) { g_triggerHandLock = on ? 1 : 0; }
int  VRNH_TriggerHandLock() { return g_triggerHandLock; }
static float g_twoHandStability = 0.75f;
static float g_twoHandRecoilReduction = 0.80f;
static VRHandlingFilter g_handlingFilter;
static float g_restHoldRows[9], g_restHoldPos[3];
static int g_restHoldValid;
static double g_lastHandlingFire = -1.0;
void VRNH_SetHandling(float stability, float recoilReduction)
{
    if (stability >= 0 && stability <= 1) g_twoHandStability = stability;
    if (recoilReduction >= 0 && recoilReduction <= 1) g_twoHandRecoilReduction = recoilReduction;
}
void VRNH_GetHandling(float* stability, float* recoilReduction)
{
    if (stability) *stability = g_twoHandStability;
    if (recoilReduction) *recoilReduction = g_twoHandRecoilReduction;
}
// The orientation blend is armed ONLY by a two-hand transition (see below).
static int    g_2hPrevState = VRNH_2H_LEFT_FREE;
static double g_axisBlendUntil = -1.0;

// TWO-HAND TUNING (brief 2026-09-04, sections 9-13; see the header). The
// hardware run of 2026-09-04c attached with the slide clamped at t=0 on
// most beats: the left hand sat behind a +/-10 cm slide around a support
// point measured for a 7.5-17 cm grab radius, so the grab was hard to
// enter and easy to lose. Forgiving floor on the radius, a longer legal
// slide, immediate attach on grip, and a release hysteresis independent of
// the tight measured radius.
static float g_2hGrabRadiusM = VRNH_2H_GRAB_RADIUS_M;
static float g_2hReleaseM    = VRNH_2H_RELEASE_M;
static float g_2hSlideHalfM  = VRNH_2H_SLIDE_HALF_M;
static float g_2hBlendS      = VRNH_2H_BLEND_S;
// Detach when the support residual exceeds this multiple of the grab radius
// (or the release distance, whichever is larger).
static const float kDetachFactor = 1.6f;
static unsigned g_2hAttachCount, g_2hDetachCount;
static int      g_2hLastDetach = VRNH_2H_DETACH_NONE;

void VRNH_SetTwoHandTuning(float grabRadiusM, float releaseM, float slideHalfM, float blendS)
{
    if (grabRadiusM >= 0.0f && grabRadiusM <= 0.5f) g_2hGrabRadiusM = grabRadiusM;
    if (releaseM >= 0.0f && releaseM <= 1.0f)       g_2hReleaseM = releaseM;
    if (slideHalfM >= 0.0f && slideHalfM <= 0.5f)   g_2hSlideHalfM = slideHalfM;
    if (blendS >= 0.0f && blendS <= 1.0f)           g_2hBlendS = blendS;
}
void VRNH_GetTwoHandTuning(float* grabRadiusM, float* releaseM, float* slideHalfM, float* blendS)
{
    if (grabRadiusM) *grabRadiusM = g_2hGrabRadiusM;
    if (releaseM)    *releaseM = g_2hReleaseM;
    if (slideHalfM)  *slideHalfM = g_2hSlideHalfM;
    if (blendS)      *blendS = g_2hBlendS;
}
const char* VRNH_TwoHandDetachName(int reason)
{
    switch (reason) {
        case VRNH_2H_DETACH_NONE:      return "none";
        case VRNH_2H_DETACH_GRIP:      return "GRIP_RELEASED";
        case VRNH_2H_DETACH_RESIDUAL:  return "RESIDUAL";
        case VRNH_2H_DETACH_LEFT_LOST: return "LEFT_LOST";
        case VRNH_2H_DETACH_WEAPON:    return "WEAPON_CHANGED";
        case VRNH_2H_DETACH_DISABLED:  return "DISABLED";
        default:                       return "?";
    }
}

void VRNH_SetEnabled(int on) { g_enabled = on ? 1 : 0; }
int  VRNH_Enabled() { return g_enabled; }

const char* VRNH_HandReasonName(int reason)
{
    switch (reason) {
    case VRNH_HAND_ENGAGED:     return "ENGAGED";
    case VRNH_HAND_DISABLED:    return "DISABLED";
    case VRNH_HAND_NO_HEAD:     return "NO_HEAD";
    case VRNH_HAND_NO_GRIP:     return "NO_GRIP";
    case VRNH_HAND_TRACKING:    return "TRACKING";
    case VRNH_HAND_NO_BASELINE: return "NO_BASELINE";
    case VRNH_HAND_STEREO_DISAGREE: return "STEREO_DISAGREE";
    case VRNH_HAND_PAIR_COOLDOWN:   return "PAIR_COOLDOWN";
    default:                    return "?";
    }
}

const char* VRNH_VerdictName(int verdict)
{
    switch (verdict) {
    case VRNH_VERDICT_OK:          return "OK";
    case VRNH_VERDICT_HALF_NATIVE: return "HALF_NATIVE";
    case VRNH_VERDICT_DISAGREE:    return "DISAGREE";
    case VRNH_VERDICT_VANILLA_FALLBACK: return "VANILLA_FALLBACK";
    case VRNH_VERDICT_OWNER_CONFLICT:   return "OWNER_CONFLICT";
    default:                       return "?";
    }
}

const char* VRNH_ObjName(int obj)
{
    switch (obj) {
    case VRNH_OBJ_R:   return "R";
    case VRNH_OBJ_L:   return "L";
    case VRNH_OBJ_GUN: return "GUN";
    default:           return "?";
    }
}

const char* VRNH_GunReasonName(int reason)
{
    switch (reason) {
    case VRNH_GUN_DRIVING:          return "DRIVING";
    case VRNH_GUN_DISABLED:         return "DISABLED";
    case VRNH_GUN_HAND_NOT_ENGAGED: return "HAND_NOT_ENGAGED";
    case VRNH_GUN_NO_PALM_BASELINE: return "NO_PALM_BASELINE";
    case VRNH_GUN_NO_BODY:          return "NO_BODY";
    case VRNH_GUN_UNKNOWN_WEAPON:   return "UNKNOWN_WEAPON";
    case VRNH_GUN_PAIR_COOLDOWN:    return "PAIR_COOLDOWN";
    default:                        return "?";
    }
}

void VRNH_SetCalibration(int xrHand, const float offsetM[3], const float rotDeg[3])
{
    if (xrHand < 0 || xrHand >= VRI_HANDS) return;
    if (offsetM) memcpy(g_calOffsetM[xrHand], offsetM, sizeof(g_calOffsetM[xrHand]));
    if (rotDeg)  memcpy(g_calRotDeg[xrHand],  rotDeg,  sizeof(g_calRotDeg[xrHand]));
}

void VRNH_SetCalibrationTable(int on) { g_calTable = on ? 1 : 0; }
int  VRNH_CalibrationTable() { return g_calTable; }

void VRNH_SetPalmFlip(int on) { g_palmFlip = on ? 1 : 0; }
int  VRNH_PalmFlip() { return g_palmFlip; }

// THE PALM ANCHOR (2026-09-04d). There is ONE controller->palm
// calibration and no runtime patch on top of it: the shipped table
// (src/vr_palm_calibration.inc) is derived from the shipped skeleton and
// mesh against the OpenXR grip convention AS SPECIFIED. The 180 deg
// grip-local flip earlier candidates carried is GONE -- it was compensating
// for a table whose Y and Z grip axes were swapped, and it could not: the
// flipped rows were still 90.0 deg from the spec frame about the palm
// normal, which is the "controller owns the weapon body, not my palm"
// the 2026-09-04d hardware run reported.
// The GUN anchors on the RIGHT hand's target basis. By the project-lead
// brief of 2026-09-04 the weapon transform is frozen, so by default the gun
// keeps the UNCORRECTED basis and is byte-identical to the hardware-tested
// candidate. The hardware logs, however, show the barrel-derived fire
// direction sitting 150-180 deg from the body facing while the controller
// pointed forward (aim(beat) lines, both 2026-09-04 sessions), which is what
// a gun anchored on the flipped basis does. `vr_native_hands_gun_follow_fix
// 1` anchors the gun on the CORRECTED basis instead, so hand and weapon stay
// the authored rigid pair with the barrel forward. Default OFF (frozen).
static int g_gunFollowsFix = 0;   // inert since 2026-09-04d (see above)
void VRNH_SetGunFollowsLimbFix(int on) { g_gunFollowsFix = on ? 1 : 0; }
int  VRNH_GunFollowsLimbFix() { return g_gunFollowsFix; }

void VRNH_SetCalibrationRows(int xrHand, const float rows[9])
{
    if (xrHand < 0 || xrHand >= VRI_HANDS) return;
    if (rows) { memcpy(g_calRows[xrHand], rows, sizeof(g_calRows[xrHand])); g_calRowsSet[xrHand] = 1; }
    else      { g_calRowsSet[xrHand] = 0; }
}

int VRNH_GetEffectiveCalibration(int xrHand, int rig, float rows[9], float offsetM[3])
{
    if (xrHand < 0 || xrHand >= VRI_HANDS) return 0;
    float r[9] = { 1,0,0, 0,1,0, 0,0,1 };
    float o[3] = { 0, 0, 0 };
    int have = 0;
    if (g_calRowsSet[xrHand]) {
        memcpy(r, g_calRows[xrHand], sizeof(r));
        have = 1;
    } else if (g_calTable) {
        // Rig -1 (the default hands_new4 tables) and rig 0 are the same rig.
        const int ri = (rig < 0) ? 0 : rig;
        const CodvrHandRigDef* rd = VRHandPose_Rig(ri);
        const int hand = (xrHand == VRI_RIGHT) ? VRHP_TRIGGER : VRHP_SUPPORT;
        const int n = (int)(sizeof(kPalmCal) / sizeof(kPalmCal[0]));
        for (int i = 0; i < n; ++i) {
            if (kPalmCal[i].hand != hand) continue;
            if (rd && strcmp(kPalmCal[i].rig, rd->name) != 0) continue;
            memcpy(r, kPalmCal[i].rows, sizeof(r));
            memcpy(o, kPalmCal[i].offsetM, sizeof(o));
            have = 1;
            break;
        }
    }
    // The operator trim offset adds to the table's.
    for (int k = 0; k < 3; ++k) o[k] += g_calOffsetM[xrHand][k];
    if (rows) memcpy(rows, r, sizeof(r));
    if (offsetM) memcpy(offsetM, o, sizeof(o));
    return have;
}

int VRNH_GetCalibrationAnatomy(int xrHand, int rig, float k[3], float t[3], float n[3])
{
    if (xrHand < 0 || xrHand >= VRI_HANDS) return 0;
    const int ri = (rig < 0) ? 0 : rig;
    const CodvrHandRigDef* rd = VRHandPose_Rig(ri);
    const int hand = (xrHand == VRI_RIGHT) ? VRHP_TRIGGER : VRHP_SUPPORT;
    const int cnt = (int)(sizeof(kPalmCal) / sizeof(kPalmCal[0]));
    for (int i = 0; i < cnt; ++i) {
        if (kPalmCal[i].hand != hand) continue;
        if (rd && strcmp(kPalmCal[i].rig, rd->name) != 0) continue;
        if (k) memcpy(k, kPalmCal[i].kBone, 3 * sizeof(float));
        if (t) memcpy(t, kPalmCal[i].tBone, 3 * sizeof(float));
        if (n) memcpy(n, kPalmCal[i].nBone, 3 * sizeof(float));
        return 1;
    }
    return 0;
}

void VRNH_SetGunCalibration(const float offsetM[3], const float rotDeg[3])
{
    if (offsetM) memcpy(g_gunCalOffsetM, offsetM, sizeof(g_gunCalOffsetM));
    if (rotDeg)  memcpy(g_gunCalRotDeg,  rotDeg,  sizeof(g_gunCalRotDeg));
}

void VRNH_SetTrackingGrace(float seconds)
{
    if (seconds >= 0.0f) g_trackingGraceS = seconds;
}

void VRNH_GetDebug(VRNHDebug* out) { if (out) *out = g_dbg; }

void VRNH_SetTwoHandEnabled(int on) { g_twoHandEnabled = on ? 1 : 0; }
void VRNH_SetReloadEnabled(int on)
{
    g_reloadEnabled = on ? 1 : 0;
    if (!g_reloadEnabled) g_reloadAvailable = 0;
    g_lastReloadVerts = 0;
}
void VRNH_SetReloadMode(int mode)
{
    if (mode < 0 || mode > 2) return;
    g_reloadMode = mode;
    VRNH_SetReloadEnabled(mode != 0);
}
int VRNH_ReloadMode() { return g_reloadEnabled ? g_reloadMode : 0; }
int VRNH_PhysicalReloadAvailable() { return g_reloadEnabled && g_reloadAvailable; }

void VRNH_TestReset()
{
    g_ownerConflictStreak = 0;
    memset(g_visualTrimDeg, 0, sizeof(g_visualTrimDeg));
    g_enabled = 0;
    memset(g_calOffsetM, 0, sizeof(g_calOffsetM));
    memset(g_calRotDeg, 0, sizeof(g_calRotDeg));
    // Offline cases model the raw-grip contract; the production default is
    // the anatomical table (module init), and the fixtures that want it
    // switch it on.
    g_calTable = 0;
    // The hardware correction is a property of the runtime grip basis, so it
    // stays ON across TestReset -- the offline hand-render fixtures model the
    // corrected production path and assert the gun is unaffected by it.
    g_palmFlip = 0;
    g_gunFollowsFix = 0;
    memset(g_calRows, 0, sizeof(g_calRows));
    memset(g_calRowsSet, 0, sizeof(g_calRowsSet));
    memset(g_pairEngaged, 0, sizeof(g_pairEngaged));
    memset(g_fallbackStreak, 0, sizeof(g_fallbackStreak));
    VRWeapon_SetBarrelOverride(0, 0, 0);
    memset(g_gunCalOffsetM, 0, sizeof(g_gunCalOffsetM));
    memset(g_gunCalRotDeg, 0, sizeof(g_gunCalRotDeg));
    g_rightGripValid = 0;
    g_trackingGraceS = 0.5f;
    memset(g_lastGoodTime, 0, sizeof(g_lastGoodTime));
    memset(&g_dbg, 0, sizeof(g_dbg));
    g_twoHandEnabled = 1;
    g_reloadEnabled  = 1;
    g_reloadMode = 1;
    g_reloadAvailable = 0;
    g_lastReloadVerts = 0;
    g_2hState = VRNH_2H_LEFT_FREE;
    g_2hT = 0.5f;
    g_2hGrabRadiusM = VRNH_2H_GRAB_RADIUS_M;
    g_2hReleaseM    = VRNH_2H_RELEASE_M;
    g_2hSlideHalfM  = VRNH_2H_SLIDE_HALF_M;
    g_2hBlendS      = VRNH_2H_BLEND_S;
    g_2hAttachCount = g_2hDetachCount = 0;
    g_2hLastDetach = VRNH_2H_DETACH_NONE;
    g_lastGunVerts = 0;
    memset(g_appliedGunAxis, 0, sizeof(g_appliedGunAxis));
    g_haveAppliedAxis = 0;
    g_lastGunNow = 0.0;
    memset(&g_handlingFilter, 0, sizeof(g_handlingFilter));
    g_restHoldValid = 0;
    g_lastHandlingFire = -1.0;
    // Old direct-geometry tests explicitly exercise strength zero. Production
    // configuration and the handling regression exercise the assisted setting.
    g_twoHandStability = g_twoHandRecoilReduction = 0.0f;
    g_2hPrevState = VRNH_2H_LEFT_FREE;
    g_axisBlendUntil = -1.0;
    g_supportPoseLock = 1;
    g_triggerHandLock = 1;
    memset(g_pairEyeRoomPos, 0, sizeof(g_pairEyeRoomPos));
    memset(g_pairHeadAxis, 0, sizeof(g_pairHeadAxis));
    g_pairHeadAxis[0] = g_pairHeadAxis[4] = g_pairHeadAxis[8] = 1.0f;
    g_pairHeadValid = 0;
    g_pairSerial = 0;
    memset(g_cooldown, 0, sizeof(g_cooldown));
    memset(&g_pairStats, 0, sizeof(g_pairStats));
    memset(&g_lastVerdict, 0, sizeof(g_lastVerdict));
    g_lastVerdict.objIndex = -1;
}

// Room-space (metres, Quake axes) position -> the space viewmodel vertices
// are recovered in (docs/COORDINATE-SPACES.md's PoseGLEye), relative to the
// head this pair was actually located with.
static void RoomPosToEyeish(const float roomPosM[3], const float headRoomPosM[3],
                            const float headAxis[9], float outEyeish[3])
{
    const float rel[3] = {
        roomPosM[0] - headRoomPosM[0],
        roomPosM[1] - headRoomPosM[1],
        roomPosM[2] - headRoomPosM[2],
    };
    float localM[3];
    VRM_WorldToLocalVec(headAxis, rel, localM);
    const float localU[3] = { localM[0] * 39.37f, localM[1] * 39.37f, localM[2] * 39.37f };
    VRM_QuakeToGLEyeVec(localU, outEyeish);
}

static void RoomAxisToEyeish(const float roomAxisQuake[9], const float headAxis[9],
                             float outEyeish[9])
{
    float localAxis[9];
    VRM_WorldToLocalAxis(headAxis, roomAxisQuake, localAxis);
    VRM_QuakeToGLEyeAxis(localAxis, outEyeish);
}

// ---------------------------------------------------------------------------
// PAIR-LEVEL STEREO ELIGIBILITY
//
// eye 0 and eye 1 may NEVER independently choose native versus vanilla for the
// same physical hand. Each eye solves the palm from its OWN draw, so with a
// real refusal rate the two answers differ roughly as often as a coin -- and a
// build that acts on each answer as it arrives renders one eye's hand on the
// controller and the other's on the weapon. Two irreconcilable images of one
// hand is the same class of stereo fault as the 2026-08-23 nausea build.
//
// So the decision is made ONCE per pair, from BOTH eyes' evidence, and the
// answer is installed as a gate vr_glsurf enforces on every draw.
//
// RECONSTRUCTING PAIRWORLD. The comparison is made in SUBMITTED space -- the
// pose each eye recovered BEFORE it was mapped through that eye's modelview --
// and that choice is the whole trick:
//
//   * Both eyes of a latched pair are handed the SAME authored viewmodel, so
//     in submitted space the two recoveries must be the same pose. Position
//     and orientation are both directly comparable, and the correct answer is
//     zero difference in both.
//   * Comparing EYE-space poses instead would require knowing whether the
//     engine's viewmodel modelview carries the inter-eye offset at all. It
//     mostly does not -- the hand batch arrives already skinned camera-relative
//     with a ~0 translation -- so a check written against an assumed IPD is a
//     check against an assumption. Submitted space needs none.
//
// A disagreement past the tolerances means the two eyes did not recover the
// same physical hand, and the honest answer for the pair is vanilla on BOTH.

static float PairAxisAngleDeg(const float a[9], const float b[9])
{
    // NOT acos((trace - 1) / 2). Near zero rotation that form is numerically
    // hopeless: c = 1 - theta^2/2, so the smallest angle a float can resolve
    // is sqrt(2*eps), about 0.03 degrees, and two IDENTICAL bases routinely
    // read as 0.056 degrees of disagreement -- which is over the 0.05 degree
    // pair tolerance and would refuse a perfect pair. (The temporal soak
    // caught exactly that: 13 gun "disagreements" at 0.000 mm / 0.0560 deg.)
    //
    // For a rotation R = b * a^T,  ||R - I||_F = 2*sqrt(2)*sin(theta/2),
    // which is linear in theta near zero and resolves down to eps itself.
    // Accumulated in double so the ruler is finer than the thing it measures.
    double m[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double t = 0.0;
            for (int k = 0; k < 3; ++k)
                t += (double)b[i * 3 + k] * (double)a[j * 3 + k];
            m[i][j] = t;
        }
    double fro = 0.0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const double d = m[i][j] - (i == j ? 1.0 : 0.0);
            fro += d * d;
        }
    double s = sqrt(fro) / (2.0 * 1.4142135623730951);
    if (s > 1.0) s = 1.0;
    return (float)(2.0 * asin(s) * 57.29577951308232);
}

// Returns 1 when every PARTICIPATING eye recovered the same physical hand.
//
// "Participating" means the eye actually drew this class recently. That
// distinction is the difference between a safety net and a blanket refusal:
//   * an eye that never draws the hand poses no stereo hazard, and demanding
//     its agreement would disable native hands in every monoscopic context
//     (the flat probe; the game before VR engages);
//   * an eye that DOES draw it and produces no usable baseline is precisely
//     the half-native hazard, and must fail the pair for BOTH eyes.
//
// WHAT IS NOT COMPARED HERE (review 2026-08-29, section 5): the two eyes'
// raw AUTHORED baselines. They come from two consecutive GAME frames and the
// engine legitimately advances the animation between them; each eye's delta
// absorbs its own authored pose (Desired * inverse(Authored_eye)), so a
// difference there is expected and harmless. What must agree is the FINAL
// object, and that is judged by VRNH_PairPreSubmit on the actual applied
// deltas, before submission. This predictive gate only asks: has every eye
// that draws this hand been recovering it?
static int PairEligible(int cls, float* outPosErrU, float* outRotErrDeg)
{
    if (outPosErrU)   *outPosErrU   = -1.0f;
    if (outRotErrDeg) *outRotErrDeg = -1.0f;

    int nPart = 0;
    for (int i = 0; i < 2; ++i) {
        const int part = GLSurf_HandEyeParticipating(cls, i);
        const int have = GLSurf_GetHandBaselineEye(cls, i, nullptr);
        if (part) {
            ++nPart;
            // A participating eye with no usable baseline: THE half-native
            // case. Refuse for the whole pair rather than let the other eye
            // render native on its own.
            if (!have) return 0;
        }
    }
    if (!nPart) return 0;                    // nothing has rendered this hand
    return 1;
}

// ---------------------------------------------------------------------------
// THE GUN ON THE RIGHT CONTROLLER, THE LEFT GRAB, THE PHYSICAL RELOAD
//
// The desired weapon pose is composed from the AUTHORED palm->rear-grip
// relation, re-measured every pair from the two live baselines (the solved
// right palm and the solved gun body, both in the same submitted space).
// Carrying that relation onto the controller-driven palm target makes the
// rendered right hand hold the weapon exactly as the artists posed it --
// the palm/rear-grip residual is zero BY CONSTRUCTION, and the one set of
// hand-calibration values moves hand and weapon together.

static float NHLen3(const float v[3])
{
    return sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
}

static void NHLocalToWorldAxis(const float base[9], const float local[9],
                               float out[9])
{
    for (int r = 0; r < 3; ++r)
        VRM_LocalToWorldVec(base, local + r * 3, out + r * 3);
}

// Rows are forward/left/up, right-handed: up = forward x left.
static void NHOrthonormalizeRows(float a[9])
{
    float* r0 = a; float* r1 = a + 3; float* r2 = a + 6;
    float n = NHLen3(r0);
    if (n < 1e-6f) return;
    for (int k = 0; k < 3; ++k) r0[k] /= n;
    const float d = r1[0]*r0[0] + r1[1]*r0[1] + r1[2]*r0[2];
    for (int k = 0; k < 3; ++k) r1[k] -= d * r0[k];
    n = NHLen3(r1);
    if (n < 1e-6f) return;
    for (int k = 0; k < 3; ++k) r1[k] /= n;
    r2[0] = r0[1]*r1[2] - r0[2]*r1[1];
    r2[1] = r0[2]*r1[0] - r0[0]*r1[2];
    r2[2] = r0[0]*r1[1] - r0[1]*r1[0];
}

// Rotate every row of `rows` by the minimal arc taking unit direction `from`
// onto unit `to`. Near-antiparallel is ambiguous: refuse (return 0) rather
// than pick an arbitrary flip -- the caller keeps the previous orientation,
// which is the roll-flip prevention the acceptance list demands.
static int NHMinArcRotateRows(const float from[3], const float to[3],
                              float rows[9])
{
    float axis[3] = { from[1]*to[2] - from[2]*to[1],
                      from[2]*to[0] - from[0]*to[2],
                      from[0]*to[1] - from[1]*to[0] };
    const float s = NHLen3(axis);
    const float c = from[0]*to[0] + from[1]*to[1] + from[2]*to[2];
    if (c > 0.99999f)
        return 1;
    if (c < -0.999f || s < 1e-6f)
        return 0;
    for (int k = 0; k < 3; ++k) axis[k] /= s;
    for (int r = 0; r < 3; ++r) {
        float* v = rows + r * 3;
        const float av = axis[0]*v[0] + axis[1]*v[1] + axis[2]*v[2];
        const float cx[3] = { axis[1]*v[2] - axis[2]*v[1],
                              axis[2]*v[0] - axis[0]*v[2],
                              axis[0]*v[1] - axis[1]*v[0] };
        for (int k = 0; k < 3; ++k)
            v[k] = v[k]*c + cx[k]*s + axis[k]*av*(1.0f - c);
    }
    return 1;
}

// Map a body-frame point (metres) into the render space under the desired
// rear-grip pose. rearGripM is the pose's anchor: E(p) = grip +
// axisRows * (p - rearGripM) * unitsPerMetre.
static void NHBodyPointToEyeish(const float gripPosU[3], const float axis[9],
                                const float rearGripM[3], const float pM[3],
                                float outU[3])
{
    const float rel[3] = { (pM[0] - rearGripM[0]) * 39.3701f,
                           (pM[1] - rearGripM[1]) * 39.3701f,
                           (pM[2] - rearGripM[2]) * 39.3701f };
    float w[3];
    VRM_LocalToWorldVec(axis, rel, w);
    for (int k = 0; k < 3; ++k)
        outU[k] = gripPosU[k] + w[k];
}

static void EyeishToRoomAxis(const float eyeish[9], const float headAxis[9],
                             float outRoom[9]);

static void GunPairTick(const VRInputState* in, double now)
{
    GLSurf_SetGunSolveWanted(g_enabled);
    VRNHGunDebug& gd = g_dbg.gun;
    const float dt = (g_lastGunNow > 0.0 && now > g_lastGunNow)
                   ? (float)(now - g_lastGunNow) : 0.0f;
    g_lastGunNow = now;
    gd.targetInstalled = 0;
    gd.twoHandState = g_2hState;

    GLSurfGunSolve gs;
    GLSurf_GetGunSolve(&gs);
    gd.vertexCount = gs.lastVertexCount;
    const int weaponChanged = (gs.lastVertexCount != g_lastGunVerts);
    g_lastGunVerts = gs.lastVertexCount;

    // The right hand must be live and the weapon must be a known model with
    // a STRONG rear grip; anything less leaves the vanilla weapon. Both
    // baselines go through the same freshness rule as hand engagement: a
    // refusal on a later draw must not un-know them, but age does.
    const VRNHHandDebug& rh = g_dbg.hand[VRI_RIGHT];
    const VRNHHandDebug& lh = g_dbg.hand[VRI_LEFT];
    GLSurfHandSolve rhs;
    const int rhsFresh = GLSurf_GetHandBaseline(GLSC_HAND_R, &rhs);
    const unsigned scene = GLSurf_SceneFrameCount();
    const int bodyFresh = gs.haveBody
        && (scene - gs.bodyScene) <= (unsigned)GLS_BASELINE_FRESH_SCENES;
    const CodvrGunModel* model =
        gs.lastVertexCount ? VRGunPose_ByVertexCount(gs.lastVertexCount) : 0;
    const int cooldown = g_cooldown[VRNH_OBJ_GUN] > 0;
    if (cooldown) { --g_cooldown[VRNH_OBJ_GUN]; ++g_pairStats.cooldownPairs; }
    const int drive = g_enabled && rh.engaged && rh.haveTarget && g_rightGripValid
                   && rhsFresh && bodyFresh && model != 0 && !cooldown;
    if (drive)                                gd.reason = VRNH_GUN_DRIVING;
    else if (!g_enabled)                      gd.reason = VRNH_GUN_DISABLED;
    else if (cooldown)                        gd.reason = VRNH_GUN_PAIR_COOLDOWN;
    else if (!rh.engaged || !rh.haveTarget)   gd.reason = VRNH_GUN_HAND_NOT_ENGAGED;
    else if (!rhsFresh)                       gd.reason = VRNH_GUN_NO_PALM_BASELINE;
    else if (!bodyFresh)                      gd.reason = VRNH_GUN_NO_BODY;
    else                                      gd.reason = VRNH_GUN_UNKNOWN_WEAPON;

    float desiredGrip[3] = { 0, 0, 0 };
    float desiredAxis[9] = { 1,0,0, 0,1,0, 0,0,1 };
    if (drive) {
        // Authored palm -> rear-grip relation, from the two live baselines.
        float relPos[3];
        const float d0[3] = { gs.pivotU[0] - rhs.pos[0],
                              gs.pivotU[1] - rhs.pos[1],
                              gs.pivotU[2] - rhs.pos[2] };
        VRM_WorldToLocalVec(rhs.axis, d0, relPos);
        float relRot[9];
        VRM_WorldToLocalAxis(rhs.axis, gs.bodyAxis, relRot);

        // Remember the authored idle holding relation, not any room-space
        // controller pose. During supported fire, attenuate animation kick
        // around that relation; physical wrist movements remain available.
        const int firing = in && in->hand[VRI_RIGHT].triggerDown;
        const int supportLive = in && in->valid && in->focused
            && VRI_HandTrackingActive(in->hand[VRI_LEFT])
            && VRI_HandTrackingActive(in->hand[VRI_RIGHT])
            && in->hand[VRI_LEFT].squeezeDown && g_twoHandEnabled;
        if (weaponChanged) { g_restHoldValid = 0; g_handlingFilter.valid = 0; }
        if (firing) g_lastHandlingFire = now;
        if (!g_restHoldValid || (!firing && now-g_lastHandlingFire > 0.20)) {
            memcpy(g_restHoldRows,relRot,sizeof(relRot));
            memcpy(g_restHoldPos,relPos,sizeof(relPos));
            g_restHoldValid=1;
        }
        if (supportLive && g_2hState == VRNH_2H_ATTACHED
            && now-g_lastHandlingFire <= 0.20 && g_twoHandRecoilReduction > 0) {
            VRHandlingBlendRows(g_restHoldRows,relRot,1-g_twoHandRecoilReduction,relRot);
            for (int k=0; k<3; ++k)
                relPos[k]=g_restHoldPos[k]+(relPos[k]-g_restHoldPos[k])*(1-g_twoHandRecoilReduction);
        }

        // Carry it onto the weapon's OWN controller-driven anchor.
        //
        // COORDINATED (brief 2026-09-03, sections 8-9): the anchor is the
        // CALIBRATED right palm frame -- the same ControllerGripToPalm the
        // hand target uses -- composed with the gun's OWN trim. The authored
        // palm->rear-grip relation is then applied in that frame, so the gun
        // sits in the rendered hand exactly as the artists posed it whatever
        // the palm calibration is: RIGHT calibration moves hand and weapon
        // together as one held object. The gun keeps its own independent
        // parameter (VRNH_SetGunCalibration) on top, and the LEFT hand's
        // calibration reaches nothing here.
        float anchorPos[3], anchorAxis[9];
        VRHandPose_DesiredFromGrip(rh.targetEyeish, rh.targetAxisEyeish,
                                   g_gunCalOffsetM, g_gunCalRotDeg,
                                   anchorPos, anchorAxis);
        float w[3];
        VRM_LocalToWorldVec(anchorAxis, relPos, w);
        for (int k = 0; k < 3; ++k)
            desiredGrip[k] = anchorPos[k] + w[k];
        NHLocalToWorldAxis(anchorAxis, relRot, desiredAxis);

        // ---- two-hand support: LEFT_FREE -> CANDIDATE -> ATTACHED --------
        const VRHandState* ls = in ? &in->hand[VRI_LEFT] : 0;
        const int leftLive = g_twoHandEnabled && model->supportUsable
                          && lh.haveGrip && ls
                          && VRI_HandTrackingActive(*ls)
                          && !VRReload_HandBusy();
        if (!leftLive || weaponChanged) {
            if (g_2hState == VRNH_2H_ATTACHED) {
                ++g_2hDetachCount;
                g_2hLastDetach = weaponChanged ? VRNH_2H_DETACH_WEAPON
                               : !g_twoHandEnabled ? VRNH_2H_DETACH_DISABLED
                               : VRNH_2H_DETACH_LEFT_LOST;
            }
            g_2hState = VRNH_2H_LEFT_FREE;
            gd.grabDistanceU = -1.0f;
            gd.angularResidualDeg = -1.0f;
            // Clear the support telemetry with the state. Leaving the last
            // grab's numbers standing latched a stale residual into every
            // subsequent pair's object set and would have made a log read as
            // though a support hand were still near the fore-end.
            g_2hT = 0.5f;
            gd.supportT = 0.0f;
            gd.supportResidualU = 0.0f;
        } else {
            // Support segment endpoints under the CURRENT one-hand pose.
            float sM[3], segPt[3];
            const float tHalf = g_2hSlideHalfM;
            // Solve t in [0,1]: S(t) = support + (2t-1)*tHalf*barrelFwd.
            // Project the left grip onto the segment in body space.
            float gripBody[3];
            {
                const float rel[3] = { lh.gripEyeish[0] - desiredGrip[0],
                                       lh.gripEyeish[1] - desiredGrip[1],
                                       lh.gripEyeish[2] - desiredGrip[2] };
                float local[3];
                VRM_WorldToLocalVec(desiredAxis, rel, local);
                for (int k = 0; k < 3; ++k)
                    gripBody[k] = model->rearGripM[k] + local[k] / 39.3701f;
            }
            const float* bf = model->barrelFwd;
            // WHICH POINT OF THE FORE-END THE LEFT HAND IS ON.
            //
            // Not the projection of the hand onto the segment: the weapon is
            // about to be ROTATED so that the segment point lies along the
            // physical rear-grip -> left-hand line, and after that rotation
            // the projection has moved -- which left the bar a fraction of a
            // degree out until the next pair (0.18 deg worst, measured over a
            // soak at human hand speed; iterating it made the worst case
            // worse, not better).
            //
            // The exact criterion is DISTANCE: the support point must sit as
            // far from the rear grip as the hand does, because the rotation
            // then lands it exactly on the hand. |A + t*B| = dl is a
            // quadratic in t, solved here and clamped to the legal segment,
            // so the fixed point is reached inside the pair rather than
            // chased across pairs.
            float t;
            {
                float A[3], B[3];
                for (int k = 0; k < 3; ++k) {
                    A[k] = model->supportM[k] - model->rearGripM[k] - tHalf * bf[k];
                    B[k] = 2.0f * tHalf * bf[k];
                }
                const float dl = NHLen3(gripBody) > 0.0f ? 0.0f : 0.0f;   // placeholder
                (void)dl;
                float handRel[3];
                for (int k = 0; k < 3; ++k) handRel[k] = gripBody[k] - model->rearGripM[k];
                const float want = NHLen3(handRel);
                const float bb = B[0]*B[0] + B[1]*B[1] + B[2]*B[2];
                const float ab = A[0]*B[0] + A[1]*B[1] + A[2]*B[2];
                const float aa = A[0]*A[0] + A[1]*A[1] + A[2]*A[2];
                const float disc = ab * ab - bb * (aa - want * want);
                if (bb > 1e-9f && disc >= 0.0f) {
                    const float r = sqrtf(disc);
                    const float t1 = (-ab + r) / bb, t2 = (-ab - r) / bb;
                    // the root inside the segment, else the nearer end
                    t = (t1 >= 0.0f && t1 <= 1.0f) ? t1
                      : (t2 >= 0.0f && t2 <= 1.0f) ? t2
                      : (fabsf(t1 - 0.5f) < fabsf(t2 - 0.5f) ? t1 : t2);
                } else {
                    // No point of the segment is the hand's distance away:
                    // fall back to the projection, which is the closest point.
                    float along = 0.0f;
                    for (int k = 0; k < 3; ++k)
                        along += (gripBody[k] - model->supportM[k]) * bf[k];
                    t = (along / (2.0f * tHalf)) + 0.5f;
                }
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
            }
            for (int k = 0; k < 3; ++k)
                sM[k] = model->supportM[k] + (2.0f*t - 1.0f) * tHalf * bf[k];
            NHBodyPointToEyeish(desiredGrip, desiredAxis, model->rearGripM,
                                sM, segPt);
            float resid[3] = { segPt[0] - lh.gripEyeish[0],
                               segPt[1] - lh.gripEyeish[1],
                               segPt[2] - lh.gripEyeish[2] };
            float residU = NHLen3(resid);
            // Forgiving grab: the model's measured radius, floored.
            const float radiusU  = fmaxf(model->supportRadiusM, g_2hGrabRadiusM) * 39.3701f;
            // Hysteresis: once attached, only a clearly separated hand releases.
            const float releaseU = fmaxf(radiusU * kDetachFactor, g_2hReleaseM * 39.3701f);
            gd.grabDistanceU = residU;
            gd.angularResidualDeg = -1.0f;

            // State decision first (a grip inside the radius attaches THIS
            // pair, no candidate dwell), then the attached alignment.
            const int wasAttached = (g_2hState == VRNH_2H_ATTACHED);
            if (wasAttached) {
                if (!ls->squeezeDown)      { g_2hState = VRNH_2H_LEFT_FREE; g_2hLastDetach = VRNH_2H_DETACH_GRIP; }
                else if (residU > releaseU){ g_2hState = VRNH_2H_LEFT_FREE; g_2hLastDetach = VRNH_2H_DETACH_RESIDUAL; }
            } else {
                if (residU <= radiusU) g_2hState = ls->squeezeDown ? VRNH_2H_ATTACHED : VRNH_2H_CANDIDATE;
                else                   g_2hState = VRNH_2H_LEFT_FREE;
            }
            switch (g_2hState) {
            case VRNH_2H_LEFT_FREE:
            case VRNH_2H_CANDIDATE:
                break;
            case VRNH_2H_ATTACHED:
                // Physical R->L geometry sets pitch/yaw; roll stays anchored
                // to the right controller via the minimal-arc construction.
                {
                    float d[3] = { lh.gripEyeish[0] - desiredGrip[0],
                                   lh.gripEyeish[1] - desiredGrip[1],
                                   lh.gripEyeish[2] - desiredGrip[2] };
                    const float dl = NHLen3(d);
                    float vLoc[3] = { (sM[0] - model->rearGripM[0]),
                                      (sM[1] - model->rearGripM[1]),
                                      (sM[2] - model->rearGripM[2]) };
                    const float vl = NHLen3(vLoc);
                    if (dl > 2.0f && vl > 1e-4f) {   // > 2 in apart: solvable
                        for (int k = 0; k < 3; ++k) { d[k] /= dl; vLoc[k] /= vl; }
                        float vW[3];
                        VRM_LocalToWorldVec(desiredAxis, vLoc, vW);
                        const float vwl = NHLen3(vW);
                        if (vwl > 1e-4f) {
                            for (int k = 0; k < 3; ++k) vW[k] /= vwl;
                            float rot[9];
                            memcpy(rot, desiredAxis, sizeof(rot));
                            if (NHMinArcRotateRows(vW, d, rot)) {
                                memcpy(desiredAxis, rot, sizeof(rot));
                                NHOrthonormalizeRows(desiredAxis);
                            }
                            // else: antiparallel singularity -- keep the
                            // one-hand orientation this pair.
                        }
                    }
                    // Refresh the residual under the attached orientation,
                    // and the angle between the bore line to the support
                    // point and the physical R->L line: the rigid-bar error.
                    NHBodyPointToEyeish(desiredGrip, desiredAxis,
                                        model->rearGripM, sM, segPt);
                    for (int k = 0; k < 3; ++k)
                        resid[k] = segPt[k] - lh.gripEyeish[k];
                    residU = NHLen3(resid);
                    {
                        float a[3] = { segPt[0] - desiredGrip[0], segPt[1] - desiredGrip[1],
                                       segPt[2] - desiredGrip[2] };
                        float b[3] = { lh.gripEyeish[0] - desiredGrip[0], lh.gripEyeish[1] - desiredGrip[1],
                                       lh.gripEyeish[2] - desiredGrip[2] };
                        const float al = NHLen3(a), bl = NHLen3(b);
                        if (al > 1e-4f && bl > 1e-4f) {
                            float c = (a[0]*b[0] + a[1]*b[1] + a[2]*b[2]) / (al * bl);
                            if (c > 1.0f) c = 1.0f;
                            if (c < -1.0f) c = -1.0f;
                            gd.angularResidualDeg = acosf(c) * 57.2957795f;
                        }
                    }
                    if (residU > releaseU) {
                        g_2hState = VRNH_2H_LEFT_FREE;
                        g_2hLastDetach = VRNH_2H_DETACH_RESIDUAL;
                    }
                }
                break;
            }
            if (!wasAttached && g_2hState == VRNH_2H_ATTACHED) ++g_2hAttachCount;
            if (wasAttached && g_2hState != VRNH_2H_ATTACHED)  ++g_2hDetachCount;
            g_2hT = t;
            gd.supportT = t;
            gd.supportResidualU = residU;
        }
        gd.attachCount = g_2hAttachCount;
        gd.detachCount = g_2hDetachCount;
        gd.lastDetachReason = g_2hLastDetach;

        // Smooth attach/release -- and ONLY attach/release.
        //
        // A two-hand grab changes where the weapon's orientation comes from,
        // and that step needs easing. Ordinary one-hand tracking does not: it
        // is a rigid carry of the controller's own basis. Running the blend on
        // every pair low-passed the WEAPON with a 0.10 s time constant while
        // the rendered hand stayed rigid and exact, so 25 degrees of
        // controller yaw reached the gun as 5.4 -- the weapon lagged and
        // visibly separated from the hand holding it. Arm the blend on a
        // state change, let it expire, and be rigid the rest of the time.
        // Armed ONLY by an attach or a release (FREE <-> CANDIDATE changes
        // nothing about where the orientation comes from, and arming on
        // them low-passed the one-hand weapon every time the left hand
        // drifted across the radius). Steady attached control is rigid.
        if ((g_2hState == VRNH_2H_ATTACHED) != (g_2hPrevState == VRNH_2H_ATTACHED))
            g_axisBlendUntil = now + 3.0 * (double)g_2hBlendS;
        g_2hPrevState = g_2hState;
        if (g_haveAppliedAxis && dt > 0.0f && dt < 0.25f && g_2hBlendS > 0.0f
            && now < g_axisBlendUntil) {
            const float alpha = 1.0f - expf(-dt / g_2hBlendS);
            for (int k = 0; k < 9; ++k)
                g_appliedGunAxis[k] += (desiredAxis[k] - g_appliedGunAxis[k]) * alpha;
            NHOrthonormalizeRows(g_appliedGunAxis);
        } else {
            memcpy(g_appliedGunAxis, desiredAxis, sizeof(desiredAxis));
        }
        g_haveAppliedAxis = 1;

        // Continuous support stabilization is separate from attach/release
        // blending. History is in ROOM space, so turning the head introduces
        // no artificial gun lag. Both eyes and gameplay aim consume this one
        // filtered pose. Release, tracking loss, weapon switch and long gaps
        // reset the history; the palm/grip position is never low-pass filtered.
        const int steady = supportLive && g_2hState == VRNH_2H_ATTACHED
                         && g_pairHeadValid && !weaponChanged;
        if (g_twoHandStability > 0 && steady) {
            float room[9], filtered[9];
            EyeishToRoomAxis(g_appliedGunAxis,g_pairHeadAxis,room);
            VRHandlingFilterRows(&g_handlingFilter,room,dt,g_twoHandStability,1,filtered);
            RoomAxisToEyeish(filtered,g_pairHeadAxis,g_appliedGunAxis);
        } else g_handlingFilter.valid=0;

        GLSurf_SetGunTarget(1, desiredGrip, g_appliedGunAxis);
        VRObj_SetGunPose(desiredGrip, g_appliedGunAxis);
        gd.targetInstalled = 1;
        memcpy(gd.desiredGripEyeish, desiredGrip, sizeof(desiredGrip));

        // THE PHYSICAL BARREL -> THE GAME'S AIM (brief 2026-09-03, section
        // 10). The rendered weapon's bore, from the model's measured barrel
        // direction under the pose the gun is ACTUALLY drawn in, expressed in
        // the room frame the aim model steers from. Hands the fire direction
        // to vr_weapon; that layer converts it against the body facing into
        // the game's own view angles through the ordinary aim input. The
        // camera never participates.
        gd.barrelValid = 0;
        {
            // THE BORE, not the body axis (brief 2026-09-04d, P1). The shot
            // follows the direction the user ALIGNS: the weapon's sight line,
            // cross-checked at generation time against the muzzle tag's own
            // rotation. `barrelFwd` -- the chord from the weapon frame's
            // origin to the muzzle -- sits 6.64 deg above the Thompson's
            // sights, which is 0.91 m (three feet) high at 7.9 m: the
            // 2026-09-04d hardware report. It is used only where it belongs,
            // as the body long axis the two-hand support slides along.
            const float* bf = model->boreUsable ? model->boreFwd : model->barrelFwd;
            gd.boreFromSightLine = model->boreUsable ? 1 : 0;
            const float bfl = NHLen3(bf);
            if (bfl > 0.5f && g_pairHeadValid) {
                float roomAxis[9];
                EyeishToRoomAxis(g_appliedGunAxis, g_pairHeadAxis, roomAxis);
                float b[3];
                VRM_LocalToWorldVec(roomAxis, bf, b);
                const float bl = NHLen3(b);
                if (bl > 1e-4f) {
                    for (int k = 0; k < 3; ++k) gd.barrelRoom[k] = b[k] / bl;
                    memcpy(gd.barrelUpRoom, roomAxis + 6, sizeof(gd.barrelUpRoom));
                    gd.barrelValid = 1;
                    // Where the shot leaves the weapon, for the log and for
                    // anything that wants a muzzle position rather than a ray.
                    if (model->boreUsable) {
                        // The muzzle hangs off the REAR GRIP, which is the
                        // point the weapon is pinned by, so the offset is
                        // muzzle - rearGrip in the body frame.
                        const float mU[3] = {
                            (model->muzzleM[0] - model->rearGripM[0]) * 39.3701f,
                            (model->muzzleM[1] - model->rearGripM[1]) * 39.3701f,
                            (model->muzzleM[2] - model->rearGripM[2]) * 39.3701f };
                        float rel[3];
                        VRM_LocalToWorldVec(g_appliedGunAxis, mU, rel);
                        for (int k = 0; k < 3; ++k)
                            gd.muzzleEyeish[k] = desiredGrip[k] + rel[k];
                        gd.muzzleValid = 1;
                    }
                }
            }
        }
        VRWeapon_SetBarrelOverride(gd.barrelValid, gd.barrelRoom, gd.barrelUpRoom);
    } else {
        gd.barrelValid = 0;
        VRWeapon_SetBarrelOverride(0, 0, 0);
        GLSurf_SetGunTarget(0, 0, 0);
        VRObj_ClearGunPose();
        g_2hState = VRNH_2H_LEFT_FREE;
        g_2hPrevState = VRNH_2H_LEFT_FREE;
        g_axisBlendUntil = -1.0;
        g_haveAppliedAxis = 0;
        g_handlingFilter.valid = 0;
        g_restHoldValid = 0;
    }
    gd.twoHandState = g_2hState;

    // ---- the physical reload ---------------------------------------------
    g_reloadAvailable = 0;
    if (g_reloadEnabled) {
        if (gs.lastVertexCount != g_lastReloadVerts) {
            VRReload_SelectByVertexCount(gs.lastVertexCount);
            g_lastReloadVerts = gs.lastVertexCount;
            VRReloadProfile supportProfile;
            VRReload_Get(&supportProfile);
            if (supportProfile.valid) {
                // The right controller owns the gun. Let the free support
                // hand manipulate its measured magwell/receiver, including
                // rifle profiles whose stock animation moves the right hand.
                // Measuring that same right hand relative to its own gun
                // makes a physical reach impossible.
                supportProfile.hand = VRRL_HAND_SUPPORT;
                if (model && model->supportUsable)
                    memcpy(supportProfile.firingGripM, model->supportM,
                           sizeof(supportProfile.firingGripM));
                VRReload_SetProfile(&supportProfile);
            }
        }
        VRReloadProfile prof;
        VRReload_Get(&prof);
        g_reloadAvailable = prof.valid && gd.targetInstalled;
        VRReloadInput ri;
        memset(&ri, 0, sizeof(ri));
        ri.now = now;
        ri.menuOpen = (in ? in->menuDown : 0) || VRBinds_MenuMode() || VRMenu_Open();
        ri.weaponChanged = weaponChanged;
        const int loaderXr = VRI_LEFT;
        const VRNHHandDebug& loader = g_dbg.hand[loaderXr];
        const VRHandState* lsold = in ? &in->hand[loaderXr] : 0;
        if (gd.targetInstalled && loader.haveGrip && lsold
            && VRI_HandTrackingActive(*lsold)) {
            ri.valid = 1;
            // The loading hand in the weapon's BODY frame, metres.
            const float rel[3] = { loader.gripEyeish[0] - desiredGrip[0],
                                   loader.gripEyeish[1] - desiredGrip[1],
                                   loader.gripEyeish[2] - desiredGrip[2] };
            float local[3];
            VRM_WorldToLocalVec(g_appliedGunAxis, rel, local);
            for (int k = 0; k < 3; ++k)
                ri.handM[k] = (model ? model->rearGripM[k] : 0.0f)
                            + local[k] / 39.3701f;
            // Head-relative, Quake axes (+X fwd, +Y left, +Z up), metres --
            // eyeish is GL eye space (x right, y up, -z fwd).
            ri.handWorldM[0] = -loader.gripEyeish[2] / 39.3701f;
            ri.handWorldM[1] = -loader.gripEyeish[0] / 39.3701f;
            ri.handWorldM[2] =  loader.gripEyeish[1] / 39.3701f;
            ri.gripHeld = (lsold->squeezeDown != 0);
        }
        const int previousReloadState = VRReload_State();
        const VRReloadOutput ro = VRReload_Step(&ri);
        if (ro.state != previousReloadState) {
            switch (ro.state) {
            case VRRL_GRIPPED:  VRBinds_ReloadFeedback(loaderXr, 0.30f, 10); break;
            case VRRL_PULLED:   VRBinds_ReloadFeedback(loaderXr, 0.45f, 18); break;
            case VRRL_ACQUIRED: VRBinds_ReloadFeedback(loaderXr, 0.45f, 15); break;
            case VRRL_INSERT:   VRBinds_ReloadFeedback(loaderXr, 0.25f, 8);  break;
            case VRRL_COMMIT:   VRBinds_ReloadFeedback(loaderXr, 0.70f, 30); break;
            }
        }
        if (ro.issueReload)
            VRBinds_TapControl(VRB_RELOAD);
        gd.reloadState = ro.state;
        gd.reloadTaps = VRBinds_TapCount(VRB_RELOAD);
    } else {
        VRReloadInput cancelled = {};
        cancelled.now = now;
        VRReload_Step(&cancelled);
        gd.reloadState = VRRL_IDLE;
    }

    // One latched left-state word for the pair, for downstream consumers.
    int leftState = VROBJ_LEFT_FREE;
    if (g_2hState == VRNH_2H_ATTACHED)
        leftState = VROBJ_LEFT_SUPPORT;
    if (g_reloadEnabled && gd.reloadState > 0) {
        switch (gd.reloadState) {
        case VRRL_REACH:    leftState = VROBJ_LEFT_RELOAD_REACH;   break;
        case VRRL_GRIPPED:
        case VRRL_PULLED:   leftState = VROBJ_LEFT_RELOAD_GRABBED; break;
        case VRRL_POUCH:
        case VRRL_ACQUIRED: leftState = VROBJ_LEFT_AMMO_ACQUIRE;   break;
        case VRRL_INSERT:   leftState = VROBJ_LEFT_INSERT;         break;
        case VRRL_COMMIT:   leftState = VROBJ_LEFT_COMMIT;         break;
        case VRRL_RECOVER:  leftState = VROBJ_LEFT_RECOVER;        break;
        default: break;
        }
    }
    VRObj_LatchLeftState(leftState, g_2hState == VRNH_2H_ATTACHED, g_2hT,
                         gd.supportResidualU * 2.54f);
}

// ---------------------------------------------------------------------------
// THE PRE-SUBMIT VERDICT (see vr_native_hands.h)

// Inverse of RoomPosToEyeish for ONE eye: this eye's eyeish position back to
// room metres, Quake axes -- PairWorld.
static void EyeishToRoomPos(const float eyeish[3], const float eyeRoomPosM[3],
                            const float headAxis[9], float outRoomM[3])
{
    float q[3];
    VRM_GLEyeToQuakeVec(eyeish, q);
    const float localM[3] = { q[0] / 39.37f, q[1] / 39.37f, q[2] / 39.37f };
    float w[3];
    VRM_LocalToWorldVec(headAxis, localM, w);
    for (int k = 0; k < 3; ++k) outRoomM[k] = eyeRoomPosM[k] + w[k];
}

static void EyeishToRoomAxis(const float eyeish[9], const float headAxis[9],
                             float outRoom[9])
{
    float q[9];
    VRM_GLEyeToQuakeAxis(eyeish, q);
    NHLocalToWorldAxis(headAxis, q, outRoom);
}

static int ObjToClass(int obj)
{
    return obj == VRNH_OBJ_R ? GLSC_HAND_R : obj == VRNH_OBJ_L ? GLSC_HAND_L : GLSC_GUN;
}

int VRNH_PairPreSubmit(unsigned pairSerial, VRNHPairVerdict* out)
{
    VRNHPairVerdict v;
    memset(&v, 0, sizeof(v));
    v.pairSerial = pairSerial;
    v.ok = 1;
    v.objIndex = -1;
    ++g_pairStats.checked;
    int fallbackAbortedThisPair = 0, fallbackAcceptedThisPair = 0;

    for (int o = 0; o < VRNH_OBJS; ++o) {
        VRNHObjVerdict& ov = v.obj[o];
        ov.posErrMm = -1.0f;
        ov.rotErrDeg = -1.0f;
        ov.verdict = VRNH_VERDICT_OK;
        GLSurfEyeApply a[2];
        for (int e = 0; e < 2; ++e) {
            memset(&a[e], 0, sizeof(a[e]));
            GLSurf_GetEyeApply(ObjToClass(o), e, &a[e]);
            // A record from another pair is no record: that eye did not draw
            // this object under THIS serial.
            const int mine = (a[e].serial == pairSerial);
            ov.drew[e]    = mine ? a[e].drew    : 0;
            ov.applied[e] = mine ? a[e].applied : 0;
        }
        // Neither applied. One eye that never drew the object imposes
        // nothing (the engine itself drew no hand there). But an object that
        // was ENGAGED this pair and that BOTH eyes drew and neither could own
        // is the centred viewmodel flashing back in the middle of native
        // ownership: refused within the per-object budget (the compositor
        // holds the previous native pair), accepted as coherent vanilla once
        // the budget is spent so a hand that really stopped solving degrades
        // instead of freezing the display. Never a stale pose.
        if (!ov.applied[0] && !ov.applied[1]) {
            if (g_pairEngaged[o] && ov.drew[0] && ov.drew[1]) {
                if (g_fallbackStreak[o] < VRNH_FALLBACK_ABORT_BUDGET) {
                    ++g_fallbackStreak[o];
                    fallbackAbortedThisPair = 1;
                    ov.verdict = VRNH_VERDICT_VANILLA_FALLBACK;
                    if (v.ok) { v.ok = 0; v.reason = ov.verdict; v.objIndex = o; }
                } else {
                    fallbackAcceptedThisPair = 1;
                }
            }
            continue;
        }
        if (ov.applied[0] != ov.applied[1]) {
            const int other = ov.applied[0] ? 1 : 0;
            if (ov.drew[other]) {
                ov.verdict = VRNH_VERDICT_HALF_NATIVE;
                if (v.ok) { v.ok = 0; v.reason = ov.verdict; v.objIndex = o; }
            }
            continue;
        }
        // Both applied: reconstruct each eye's FINAL object into PairWorld.
        ++g_pairStats.objBothApplied[o];
        g_fallbackStreak[o] = 0;
        float axisRoom[2][9];
        for (int e = 0; e < 2; ++e) {
            EyeishToRoomPos(a[e].finalPos, g_pairEyeRoomPos[e], g_pairHeadAxis,
                            ov.pairWorldPosM[e]);
            EyeishToRoomAxis(a[e].finalAxis, g_pairHeadAxis, axisRoom[e]);
        }
        float d = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float r = ov.pairWorldPosM[1][k] - ov.pairWorldPosM[0][k];
            d += r * r;
        }
        ov.posErrMm = sqrtf(d) * 1000.0f;
        ov.rotErrDeg = PairAxisAngleDeg(axisRoom[0], axisRoom[1]);
        if (!(ov.posErrMm <= VRNH_PAIRWORLD_POS_TOL_MM)
            || !(ov.rotErrDeg <= VRNH_PAIRWORLD_ROT_TOL_DEG)) {
            ov.verdict = VRNH_VERDICT_DISAGREE;
            if (v.ok) { v.ok = 0; v.reason = ov.verdict; v.objIndex = o; }
        }
        // Surface the measured disagreement per hand in the debug block.
        if (o == VRNH_OBJ_R || o == VRNH_OBJ_L) {
            VRNHHandDebug& hd = g_dbg.hand[o == VRNH_OBJ_R ? VRI_RIGHT : VRI_LEFT];
            hd.pairPosErrU   = ov.posErrMm / 25.4f;
            hd.pairRotErrDeg = ov.rotErrDeg;
        }
    }

    // WEAPON OWNER CONFLICT (brief 2026-09-04, section 3): an eye-scene of
    // this pair drew native/stale weapon geometry AND vanilla weapon
    // geometry. That is two weapons on screen; the pair is refused within
    // the budget, then accepted so a persistent conflict degrades.
    {
        int conflict = 0;
        for (int e = 0; e < 2; ++e) {
            GLSurfGunScene gsc;
            if (GLSurf_GetGunScene(e, &gsc) && gsc.serial == pairSerial
                && gsc.weaponOwnerConflict)
                conflict = 1;
        }
        if (conflict) {
            ++g_pairStats.gunOwnerConflictPairs;
            if (g_ownerConflictStreak < VRNH_FALLBACK_ABORT_BUDGET) {
                ++g_ownerConflictStreak;
                v.obj[VRNH_OBJ_GUN].verdict = VRNH_VERDICT_OWNER_CONFLICT;
                if (v.ok) { v.ok = 0; v.reason = VRNH_VERDICT_OWNER_CONFLICT;
                            v.objIndex = VRNH_OBJ_GUN; }
            }
        } else {
            g_ownerConflictStreak = 0;
        }
    }
    // Counted per PAIR, not per object.
    if (fallbackAbortedThisPair) ++g_pairStats.vanillaFallbackAborted;
    else if (fallbackAcceptedThisPair) ++g_pairStats.vanillaFallbackAccepted;
    if (v.ok) {
        ++g_pairStats.accepted;
        int allNative = 1, anyEngaged = 0;
        for (int o = 0; o < VRNH_OBJS; ++o)
            if (g_pairEngaged[o]) {
                anyEngaged = 1;
                if (!(v.obj[o].applied[0] && v.obj[o].applied[1])) allNative = 0;
            }
        if (anyEngaged && allNative) ++g_pairStats.pairNative;
    } else {
        ++g_pairStats.rejected;
        ++g_pairStats.pairFailClosed;
        if (v.reason == VRNH_VERDICT_HALF_NATIVE) { ++g_pairStats.rejectHalfNative; ++g_pairStats.mixedEyeRejected; }
        else if (v.reason == VRNH_VERDICT_DISAGREE) ++g_pairStats.rejectDisagree;
        else if (v.reason == VRNH_VERDICT_OWNER_CONFLICT) ++g_pairStats.rejectOwnerConflict;
        // Hold a half-native or disagreeing object vanilla for the next
        // pair. A vanilla-fallback refusal is NOT held: the next pair tries
        // native again (that is the whole point of refusing it).
        for (int o = 0; o < VRNH_OBJS; ++o)
            if (v.obj[o].verdict == VRNH_VERDICT_HALF_NATIVE
                || v.obj[o].verdict == VRNH_VERDICT_DISAGREE)
                g_cooldown[o] = 1;
    }
    g_lastVerdict = v;
    if (out) *out = v;
    return v.ok;
}

void VRNH_GetLastVerdict(VRNHPairVerdict* out) { if (out) *out = g_lastVerdict; }
void VRNH_GetPairStats(VRNHPairStats* out)     { if (out) *out = g_pairStats; }

// ---------------------------------------------------------------------------

void VRNH_OnPairBegin(unsigned pairSerial, const VRInputState* in,
                      const float headRoomPosM[3], const float headRoomAxis[9],
                      int headValid, double now)
{
    // Monoscopic wrapper: both eyes at the head, zero eye offsets.
    float eyes[2][3];
    memcpy(eyes[0], headRoomPosM, sizeof(eyes[0]));
    memcpy(eyes[1], headRoomPosM, sizeof(eyes[1]));
    VRNH_OnPairBeginEyes(pairSerial, in, eyes, headRoomAxis, headValid, now);
}

void VRNH_OnPairBeginEyes(unsigned pairSerial, const VRInputState* in,
                          const float eyeRoomPosM[2][3], const float headRoomAxis[9],
                          int headValid, double now)
{
    VRObj_BeginPair(pairSerial);
    GLSurf_BeginLatch(pairSerial);

    // The pair's frame: the head midpoint every absolute target is expressed
    // in, and each eye's own position for the delta and the verdict.
    const float headRoomPosM[3] = {
        (eyeRoomPosM[0][0] + eyeRoomPosM[1][0]) * 0.5f,
        (eyeRoomPosM[0][1] + eyeRoomPosM[1][1]) * 0.5f,
        (eyeRoomPosM[0][2] + eyeRoomPosM[1][2]) * 0.5f,
    };
    memcpy(g_pairEyeRoomPos, eyeRoomPosM, sizeof(g_pairEyeRoomPos));
    memcpy(g_pairHeadAxis, headRoomAxis, sizeof(g_pairHeadAxis));
    g_pairHeadValid = headValid ? 1 : 0;
    g_pairSerial = pairSerial;
    {
        float off[2][3] = { { 0, 0, 0 }, { 0, 0, 0 } };
        if (headValid)
            for (int e = 0; e < 2; ++e)
                RoomPosToEyeish(eyeRoomPosM[e], headRoomPosM, headRoomAxis, off[e]);
        GLSurf_SetEyeOffsets(off);
        memcpy(g_dbg.eyeOffEyeish, off, sizeof(g_dbg.eyeOffEyeish));
    }

    g_dbg.enabled = g_enabled;
    ++g_dbg.pairs;

    // The user's floating-hands setting suppresses each complete arm/sleeve
    // class even during native-baseline reacquisition. Hand classes and their
    // tracking/ownership remain independent of this visibility choice.
    GLSurf_SetClassHidden(GLSC_SLEEVE_L, GLSurf_SleeveVisible()==0);
    GLSurf_SetClassHidden(GLSC_SLEEVE_R, GLSurf_SleeveVisible()==0);

    // Keep the palm solve running on every hand draw while enabled, whether
    // or not a target engages this pair. Without this the wiring deadlocks:
    // engagement needs a baseline solve, but the solve only ran on draws that
    // already had a target -- so the first enabled pair disengaged, produced
    // no baseline, and nothing ever engaged. (The 2026-08-26 quarantine's
    // "RIGHT: tracked + baseline -> engaged" FAIL was exactly this.)
    GLSurf_SetHandSolveWanted(GLSC_HAND_R, g_enabled);
    GLSurf_SetHandSolveWanted(GLSC_HAND_L, g_enabled);

    if (headValid)
        VRObj_LatchHead(headRoomPosM, headRoomAxis, 1);
    else {
        const float z[3] = { 0, 0, 0 };
        VRObj_LatchHead(z, headRoomAxis, 0);
    }

    for (int h = 0; h < VRI_HANDS; ++h) {
        const int cls        = VriToGLClass(h);
        const int vrobjHand  = VriToVrobjHand(h);
        VRNHHandDebug& hd    = g_dbg.hand[h];
        hd.engaged = 0;
        hd.reason = VRNH_HAND_DISABLED;
        hd.haveTarget = 0;
        hd.haveBaseline = 0;
        hd.haveGrip = 0;

        const VRHandState* hs = (in && h >= 0 && h < VRI_HANDS) ? &in->hand[h] : nullptr;
        hd.trackState = VRI_TrackCode(hs ? hs->trackState : VRI_TRACK_UNAVAILABLE);

        float gripRoomM[3] = { 0, 0, 0 };
        float gripAxisQuake[9] = { 1,0,0, 0,1,0, 0,0,1 };
        int haveGrip = 0;
        if (hs && hs->gripValid) {
            VRM_XrToQuakeVec(hs->gripPos, gripRoomM);
            VRM_QuatToQuakeAxis(hs->gripQuat, gripAxisQuake);
            haveGrip = 1;
        }

        // The grip in the render space, stashed for EVERY tracked hand --
        // the weapon layer needs the left grip (two-hand grab, reload) even
        // on pairs where the left hand's own render target is disengaged.
        float eyeishGripPos[3]  = { 0, 0, 0 };
        float eyeishGripAxis[9] = { 1,0,0, 0,1,0, 0,0,1 };
        if (haveGrip && headValid) {
            RoomPosToEyeish(gripRoomM, headRoomPosM, headRoomAxis, eyeishGripPos);
            RoomAxisToEyeish(gripAxisQuake, headRoomAxis, eyeishGripAxis);
            hd.haveGrip = 1;
            memcpy(hd.gripEyeish, eyeishGripPos, sizeof(eyeishGripPos));
        }

        // Latch the raw grip regardless of engagement: vr_objects is a
        // general-purpose pair-latch consumers besides rendering (future
        // support/reload wiring) can read, and a latch that only sometimes
        // fires would defeat its own "one sample per pair" guarantee.
        // "ACTIVE or LOST-within-grace" is the question -- ask it by name.
        const int trackedForLatch = hs && VRI_HandTrackingUsable(*hs);
        VRObj_LatchGrip(vrobjHand, gripRoomM, gripAxisQuake, trackedForLatch ? 1 : 0);

        if (!g_enabled || !headValid || !haveGrip) {
            hd.reason = !g_enabled ? VRNH_HAND_DISABLED
                      : !headValid ? VRNH_HAND_NO_HEAD : VRNH_HAND_NO_GRIP;
            GLSurf_SetHandPairEligible(cls, 0);   // fail closed
            GLSurf_SetHandTarget(cls, 0, nullptr, nullptr);
            VRObj_ClearHandPose(vrobjHand);
            continue;
        }

        bool engage = VRI_HandTrackingActive(*hs) != 0;
        if (engage) {
            g_lastGoodTime[h] = now;
        } else if (VRI_HandTrackingLost(*hs)
                   && (now - g_lastGoodTime[h]) <= (double)g_trackingGraceS) {
            engage = true;   // bounded grace: ride out a short dropout
        }

        if (!engage) {
            hd.reason = VRNH_HAND_TRACKING;
            GLSurf_SetHandPairEligible(cls, 0);   // fail closed
            GLSurf_SetHandTarget(cls, 0, nullptr, nullptr);
            VRObj_ClearHandPose(vrobjHand);
            continue;
        }

        // The absolute controller-driven target, in the same space
        // vr_handpose recovers the authored pose in (grip already converted
        // above, where the weapon layer stashed it too).
        float targetPos[3], targetAxis[9];
        {
            // ControllerGrip * ControllerGripToPalm: the rig's anatomical
            // rows and offset (brief section 7), then the operator trim.
            GLSurfHandSolve rigSolve;
            GLSurf_GetHandSolve(cls, &rigSolve);
            float calRows[9], calOff[3];
            const int haveCal = VRNH_GetEffectiveCalibration(
                h, rigSolve.everSolved ? rigSolve.lastRig : -1, calRows, calOff);
            // THE anchor: DesiredPalm = ControllerGrip * GripToPalm. This
            // one frame is what the rendered hand is carried onto AND what
            // the weapon is placed relative to. Nothing else is applied.
            VRHandPose_DesiredFromGripCal(eyeishGripPos, eyeishGripAxis,
                                          haveCal ? calRows : 0, calOff,
                                          g_calRotDeg[h], targetPos, targetAxis);
        }
        hd.haveTarget = 1;
        memcpy(hd.targetEyeish, targetPos, sizeof(targetPos));
        memcpy(hd.targetAxisEyeish, targetAxis, sizeof(targetAxis));
        if (h == VRI_RIGHT) {
            // The RAW grip frame, for the gun's own calibration to hang off.
            memcpy(g_rightGripEyeish, eyeishGripPos, sizeof(g_rightGripEyeish));
            memcpy(g_rightGripAxisEyeish, eyeishGripAxis,
                   sizeof(g_rightGripAxisEyeish));
            g_rightGripValid = 1;
        }

        // The baseline: the last RECENT recovered authored pose for this
        // class (GLSurf_GetHandBaseline). None yet, or none fresh -- disengage
        // for just this pair rather than guess; the next successful draw seeds
        // it and engagement resumes on its own next pair.
        //
        // NOT the raw last-attempt `valid` flag: the 2026-08-28 hardware run
        // proved roughly every other real hand-solve ATTEMPT refuses in live
        // gameplay, so gating on "the very last attempt succeeded" left the
        // hands permanently disengaged while fresh good solves existed. A
        // refusal on some draw must not un-know a solve that already
        // happened; only age (the freshness window) may.
        GLSurfHandSolve solve;
        if (!GLSurf_GetHandBaseline(cls, &solve)) {
            hd.reason = VRNH_HAND_NO_BASELINE;
            GLSurf_SetHandPairEligible(cls, 0);
            GLSurf_SetHandTarget(cls, 0, nullptr, nullptr);
            VRObj_ClearHandPose(vrobjHand);
            continue;
        }
        hd.haveBaseline = 1;

        // PAIR-LEVEL STEREO ELIGIBILITY (predictive). Every eye that has
        // been drawing this hand must have recovered it recently, or neither
        // eye may move it this pair. Decided here, once, and enforced by
        // vr_glsurf on every draw of the pair. The hard guarantee -- no
        // half-native pair is ever SUBMITTED -- is VRNH_PairPreSubmit.
        const int obj = (h == VRI_RIGHT) ? VRNH_OBJ_R : VRNH_OBJ_L;
        hd.pairEligible = PairEligible(cls, nullptr, nullptr);
        if (g_cooldown[obj] > 0) {
            --g_cooldown[obj];
            ++g_pairStats.cooldownPairs;
            hd.pairEligible = 0;
            hd.reason = VRNH_HAND_PAIR_COOLDOWN;
        } else if (!hd.pairEligible) {
            hd.reason = VRNH_HAND_STEREO_DISAGREE;
        }
        GLSurf_SetHandPairEligible(cls, hd.pairEligible);
        if (!hd.pairEligible) {
            GLSurf_SetHandTarget(cls, 0, nullptr, nullptr);
            VRObj_ClearHandPose(vrobjHand);
            continue;
        }
        memcpy(hd.lastAuthoredPos, solve.pos, sizeof(solve.pos));

        // The ABSOLUTE desired palm pose, head-midpoint frame. vr_glsurf
        // shifts it into each eye's own frame and builds that eye's delta
        // against that eye's own fresh authored solve. The RENDER uses the
        // 2026-09-04-corrected orientation; position is unchanged.
        // The RENDERED hand: the physical target turned by the visual trim
        // about the palm's own axes. Position untouched, so the pivot is
        // the same point; the weapon anchors on targetAxis above, so it
        // never sees this.
        float handAxis[9];
        {
            const float* tr = g_visualTrimDeg[h];
            if (tr[0] == 0.0f && tr[1] == 0.0f && tr[2] == 0.0f) {
                memcpy(handAxis, targetAxis, sizeof(handAxis));
            } else {
                float tmpPos[3];
                VRHandPose_DesiredFromGripCal(targetPos, targetAxis, 0, 0, tr, tmpPos, handAxis);
            }
        }
        GLSurf_SetHandTarget(cls, 1, targetPos, handAxis);
        VRObj_SetHandPose(vrobjHand, targetPos, handAxis);
        hd.engaged = 1;
        hd.reason = VRNH_HAND_ENGAGED;
    }

    // The weapon rides the pair the same way the hands do: composed once,
    // here, from this pair's latched values -- never re-sampled by an eye.
    GunPairTick(in, now);
    // HANDS RIDE THE WEAPON (2026-09-06). A hand that rides is drawn where
    // the game's own animation holds it on the gun: its authored palm carried
    // through the weapon's rigid transform (desired rear grip + basis against
    // the authored rear grip and body basis, both from the last real solves,
    // which are camera-relative and therefore eye-independent). ONE absolute
    // target per pair, like every other hand target, so both eyes build
    // their delta against the same pose and the stereo verdict holds by
    // construction; the pose lags the animation by one frame (invisible).
    //   LEFT  (vr_support_pose_lock): while the two-hand support is attached.
    //   RIGHT (vr_trigger_hand_lock): whenever the weapon is native. The
    //         controller still drives the weapon through the palm
    //         calibration (rh.targetEyeish above is what the gun anchors on
    //         and is NOT touched here); only the rendered hand follows the
    //         gun as applied -- bar-aligned, stabilised, kicking or sighted.
    //         The visual trim still turns it about the ridden palm.
    {
        GLSurfGunSolve gs; GLSurf_GetGunSolve(&gs);
        const unsigned scene = GLSurf_SceneFrameCount();
        const int gunFresh = g_dbg.gun.targetInstalled && g_haveAppliedAxis && gs.haveBody
                          && scene - gs.bodyScene <= (unsigned)GLS_BASELINE_FRESH_SCENES
                          && gs.lastVertexCount == g_dbg.gun.vertexCount;
        for (int side = 0; side < 2; ++side) {
            const int xr  = side ? VRI_RIGHT : VRI_LEFT;
            const int cls = side ? GLSC_HAND_R : GLSC_HAND_L;
            const int want = side ? g_triggerHandLock
                                  : (g_supportPoseLock && g_2hState == VRNH_2H_ATTACHED
                                     && !VRReload_HandBusy());
            VRNHHandDebug& hd = g_dbg.hand[xr];
            int ride = 0;
            GLSurfHandSolve hs;
            if (want && gunFresh && hd.engaged && GLSurf_GetHandBaseline(cls, &hs) && hs.valid) {
                float d0[3], relPos[3], relAxis[9], w[3], pos[3], axis[9];
                for (int k = 0; k < 3; ++k) d0[k] = hs.pos[k] - gs.pivotU[k];
                VRM_WorldToLocalVec(gs.bodyAxis, d0, relPos);
                for (int r = 0; r < 3; ++r)
                    VRM_WorldToLocalVec(gs.bodyAxis, hs.axis + r * 3, relAxis + r * 3);
                VRM_LocalToWorldVec(g_appliedGunAxis, relPos, w);
                for (int k = 0; k < 3; ++k) pos[k] = g_dbg.gun.desiredGripEyeish[k] + w[k];
                NHLocalToWorldAxis(g_appliedGunAxis, relAxis, axis);
                float handAxis[9];
                {
                    const float* tr = g_visualTrimDeg[xr];
                    if (tr[0] == 0.0f && tr[1] == 0.0f && tr[2] == 0.0f) {
                        memcpy(handAxis, axis, sizeof(handAxis));
                    } else {
                        float tmpPos[3];
                        VRHandPose_DesiredFromGripCal(pos, axis, 0, 0, tr, tmpPos, handAxis);
                    }
                }
                GLSurf_SetHandTarget(cls, 1, pos, handAxis);
                VRObj_SetHandPose(side ? VROBJ_HAND_R : VROBJ_HAND_L, pos, handAxis);
                if (!side) {
                    // The left hand has no downstream consumer of its
                    // controller target; publish the ridden pose. The RIGHT
                    // target stays the physical palm the gun anchors on.
                    memcpy(hd.targetEyeish, pos, sizeof(pos));
                    memcpy(hd.targetAxisEyeish, axis, sizeof(axis));
                }
                ride = 1;
            }
            if (side) g_dbg.gun.rightRidesGun = ride;
            else      g_dbg.gun.leftRidesGun = ride;
        }
    }

    // What was engaged this pair, for the pre-submit ownership policy.
    g_pairEngaged[VRNH_OBJ_R]   = g_dbg.hand[VRI_RIGHT].engaged ? 1 : 0;
    g_pairEngaged[VRNH_OBJ_L]   = g_dbg.hand[VRI_LEFT].engaged ? 1 : 0;
    g_pairEngaged[VRNH_OBJ_GUN] = g_dbg.gun.targetInstalled ? 1 : 0;
}
