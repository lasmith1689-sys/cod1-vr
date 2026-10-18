// Weapon/aim model implementation. Pure logic: no OpenXR, no OpenGL, no
// Windows input -- everything here is testable offline with plain numbers.
//
// The one non-obvious dependency is on vr.cpp's latched neutral references
// (VR_GetNeutralRefs / VR_GetHeadRoomMid): controller poses must be measured
// against the SAME neutral the head uses, or the hand would drift away from
// the head by exactly the recenter correction.

#include "vr_weapon.h"
#include "vr.h"
#include "codvr.h"
#include "vr_math.h"

#include <string.h>
#include <math.h>

extern void Log(const char* fmt, ...);

// ---------------------------------------------------------------- config

// DEFAULT OFF. The comfortable configuration must be what you get with no
// config file, a corrupt one, or a path the loader could not resolve -- not
// something a successfully-parsed key has to switch on. See
// EnforceCameraStableBaseline in dllmain.cpp.
static int   g_enabled          = 0;           // vr_aim_mode
static int   g_dominant         = VRI_RIGHT;   // vr_dominant_hand
// DEFAULT OFF and RETIRED: the transform this gated wrote through syscalls
// 37/38, which are collision queries on &cg.refdef.vieworg.
static int   g_transformEnabled = 0;           // vr_weapon_transform (retired)
static int   g_anchorKeepModel  = 1;           // vr_weapon_anchor: 1 model, 0 hand
static float g_calPosU[3]       = { 0, 0, 0 }; // game units, weapon frame
static float g_calRows[9]       = { 1,0,0, 0,1,0, 0,0,1 };
static int   g_physicalAds      = 1;
static float g_adsEnterDistM    = 0.32f;
static float g_adsExitDistM     = 0.42f;
static float g_adsEnterAngDeg   = 15.0f;
static float g_adsExitAngDeg    = 25.0f;
static int   g_twoHandEnabled   = 1;
static float g_twoHandMaxDistM  = 0.75f;
static float g_twoHandBlendSec  = 0.12f;
static float g_twoHandGrabM     = 0.22f;   // vr_twohand_grab_radius
static float g_twoHandReleaseM  = 0.32f;   // vr_twohand_release_radius

void VRWeapon_SetEnabled(int on)        { g_enabled = on ? 1 : 0; }
int  VRWeapon_Enabled()                 { return g_enabled; }
void VRWeapon_SetDominantHand(int hand) { if (hand == VRI_LEFT || hand == VRI_RIGHT) g_dominant = hand; }
int  VRWeapon_DominantHand()            { return g_dominant; }
void VRWeapon_SetTransformEnabled(int on){ g_transformEnabled = on ? 1 : 0; }

static bool   g_decoupledLive;
static int   g_barrelValid;
static float g_barrelFwdRoom[3], g_barrelUpRoom[3];
void VRWeapon_SetBarrelOverride(int valid, const float fwdRoom[3], const float upRoom[3])
{
    g_barrelValid = (valid && fwdRoom && upRoom) ? 1 : 0;
    if (g_barrelValid) {
        memcpy(g_barrelFwdRoom, fwdRoom, sizeof(g_barrelFwdRoom));
        memcpy(g_barrelUpRoom, upRoom, sizeof(g_barrelUpRoom));
    }
}
int VRWeapon_BarrelOverrideActive() { return g_barrelValid && g_decoupledLive; }
int  VRWeapon_TransformEnabled()        { return g_transformEnabled; }
void VRWeapon_SetAnchorMode(int keep)   { g_anchorKeepModel = keep ? 1 : 0; }
void VRWeapon_SetPhysicalAds(int on)    { g_physicalAds = on ? 1 : 0; }
int  VRWeapon_PhysicalAds()             { return g_physicalAds; }

void VRWeapon_SetCalibration(float px, float py, float pz,
                             float pitchDeg, float yawDeg, float rollDeg)
{
    g_calPosU[0] = px; g_calPosU[1] = py; g_calPosU[2] = pz;
    VRM_AnglesToRows(yawDeg, pitchDeg, rollDeg, g_calRows);
}

void VRWeapon_SetAdsThresholds(float enterDistM, float exitDistM,
                               float enterAngDeg, float exitAngDeg)
{
    if (enterDistM > 0.05f && enterDistM < 1.0f) g_adsEnterDistM = enterDistM;
    if (exitDistM > g_adsEnterDistM)             g_adsExitDistM  = exitDistM;
    if (enterAngDeg > 2.0f && enterAngDeg < 60)  g_adsEnterAngDeg = enterAngDeg;
    if (exitAngDeg > g_adsEnterAngDeg)           g_adsExitAngDeg  = exitAngDeg;
}

void VRWeapon_SetTwoHand(int enabled, float maxDistM, float blendSeconds)
{
    g_twoHandEnabled = enabled ? 1 : 0;
    if (maxDistM > 0.2f && maxDistM < 2.0f)          g_twoHandMaxDistM = maxDistM;
    if (blendSeconds >= 0.0f && blendSeconds < 1.0f) g_twoHandBlendSec = blendSeconds;
}

void VRWeapon_SetTwoHandRadii(float grabRadiusM, float releaseRadiusM)
{
    if (grabRadiusM > 0.02f && grabRadiusM < 1.0f)
        g_twoHandGrabM = grabRadiusM;
    if (releaseRadiusM > g_twoHandGrabM && releaseRadiusM < 1.5f)
        g_twoHandReleaseM = releaseRadiusM;
    else if (g_twoHandReleaseM <= g_twoHandGrabM)
        g_twoHandReleaseM = g_twoHandGrabM + 0.10f;
}

void VRWeapon_GetTwoHandRadii(float* grabRadiusM, float* releaseRadiusM)
{
    if (grabRadiusM)    *grabRadiusM    = g_twoHandGrabM;
    if (releaseRadiusM) *releaseRadiusM = g_twoHandReleaseM;
}

// ----------------------------------------------------------------- state

static float  g_bodyYawDeg;          // the body's facing in game world
static bool   g_bodyYawValid;
static bool   g_scripted;
static bool   g_rebasePending = true;

// Last REAL game aim, from the render hook (pre-substitution).
static float  g_gameAimYawDeg, g_gameAimPitchDeg;
static float  g_gameVieworg[3];
static float  g_gameAimRows[9] = { 1,0,0, 0,1,0, 0,0,1 };
static double g_gameAimTime = -1.0;
static bool   g_gameAimValid;

// Latest controller-derived weapon pose.
static bool   g_weaponValid;
static float  g_weaponWorldOrigin[3];
static float  g_weaponWorldRows[9];
static float  g_weaponRoomRows[9];       // raw room frame (for ADS geometry)
static float  g_targetYawDeg, g_targetPitchDeg;
static double g_lastPoseTime = -1.0;
static bool   g_menuMode = true;

// Two-hand machine.
static bool   g_twoHandAttached;
static float  g_twoHandBlend;            // 0..1, ramps over g_twoHandBlendSec
static int    g_twoHandChangedEdge;
static int    g_twoHandCandidate;        // diagnostics: attach gate this frame
static float  g_twoHandPerpM, g_twoHandAlongM, g_twoHandHandDistM;
static float  g_twoHandFwdRoom[3];       // hand-to-hand line (unit, room)
static float  g_oneHandFwdRoom[3];       // dominant-only forward (unit, room)

// Body-yaw rebase counter: the binds layer resets its assumed weapalt state
// whenever this bumps (spawns/loads always return the game to hip-fire).
static int    g_rebaseCount;

// Per-frame serial + rewrite memo. The view weapon is ONE persistent cgame
// struct offered up to 8 times per frame (2 base adds + attachments); only
// the first offer per frame may be transformed or the delta compounds.
static unsigned g_updateSerial;
enum { REWRITE_MEMO = 4 };
static unsigned g_rewriteKey[REWRITE_MEMO];
static unsigned g_rewriteSerial[REWRITE_MEMO];

// ADS machine.
static bool   g_adsActive;
static int    g_adsEnterEdge, g_adsExitEdge;
static double g_adsCandidateSince = -1.0;   // dwell timers
static double g_adsLostSince = -1.0;
static float  g_adsLastDistM, g_adsLastAngDeg;

// Entity classifier diagnostics.
static int    g_matchedThisFrame;
static int    g_matchedLastFrame;
static float  g_bakedOffsetU[3];
static bool   g_bakedOffsetValid;
static bool   g_bakedOffsetLogged;

// Room-space caches from the last update.
static float  g_gripRoomM[VRI_HANDS][3];
static bool   g_gripRoomValid[VRI_HANDS];
static float  g_aimRoomRows[VRI_HANDS][9];
static bool   g_aimRoomValid[VRI_HANDS];

float VRWeapon_BodyYawDeg() { return g_bodyYawDeg; }
int   VRWeapon_Scripted()   { return g_scripted ? 1 : 0; }
int   VRWeapon_TwoHandAttached() { return g_twoHandAttached ? 1 : 0; }
int   VRWeapon_AdsActive()  { return g_adsActive ? 1 : 0; }
int   VRWeapon_GetRebaseCount() { return g_rebaseCount; }

void VRWeapon_SetScripted(int on)
{
    const bool want = on != 0;
    if (want == g_scripted)
        return;
    g_scripted = want;
    Log("VR weapon: %s scripted-camera mode", want ? "ENTERING" : "leaving");
    if (!want)
        g_rebasePending = true;   // adopt whatever facing the script left
}

void VRWeapon_AddBodyYawDeg(float degrees)
{
    if (g_bodyYawValid)
        g_bodyYawDeg = VRM_WrapDeg(g_bodyYawDeg + degrees);
}

void VRWeapon_RebaseToGameAim()
{
    g_rebasePending = true;
}

void VRWeapon_OnNeutralRelatched(float deltaYawRad)
{
    // The room's "forward" rotated by delta; turning the body with it keeps
    // the view AND the weapon target exactly where they were on screen.
    if (g_bodyYawValid)
        g_bodyYawDeg = VRM_WrapDeg(g_bodyYawDeg + deltaYawRad * 57.2957795f);
}

void VRWeapon_NoteGameAim(const float gameAxis[9], const float gameOrigin[3], double now)
{
    memcpy(g_gameAimRows, gameAxis, sizeof(g_gameAimRows));
    memcpy(g_gameVieworg, gameOrigin, sizeof(g_gameVieworg));
    VRM_AxisYawPitchDeg(gameAxis, &g_gameAimYawDeg, &g_gameAimPitchDeg);
    g_gameAimTime = now;
    g_gameAimValid = true;
}

// ------------------------------------------------------------ pose update

// Convert one hand's XR-space poses into the raw room frame (Quake axes,
// metres, positions relative to the latched neutral head midpoint).
static void HandToRoom(const VRHandState& hs, int hand, const float headRefM[3], int haveRef)
{
    g_gripRoomValid[hand] = false;
    g_aimRoomValid[hand] = false;
    if (hs.gripValid && haveRef) {
        float q[3];
        VRM_XrToQuakeVec(hs.gripPos, q);
        for (int i = 0; i < 3; ++i)
            g_gripRoomM[hand][i] = q[i] - headRefM[i];
        g_gripRoomValid[hand] = true;
    }
    if (hs.aimValid) {
        VRM_QuatToQuakeAxis(hs.aimQuat, g_aimRoomRows[hand]);
        g_aimRoomValid[hand] = true;
    }
}

// Two-hand attach/release machine, raw room space. The rifle is a physical
// object: its line runs from the dominant (rear) grip along the one-hand
// weapon forward. ATTACH is a deliberate grab -- off-hand grip held, hand in
// FRONT of the rear grip along that line, within reach, and within
// g_twoHandGrabM of the line itself (you grab the rifle, not the air beside
// it). RELEASE only on letting go of the grip, leaving the reach envelope,
// or the hands collapsing together -- never on lateral motion, because
// steering the barrel by moving the support hand off the ONE-HAND line is
// exactly the feature. The attach/release envelopes differ by the
// grab/release radius margin, so no boundary can chatter the state.
static void TwoHandAdvance(const VRInputState* in, const float oneHandFwdRoom[3])
{
    const int off = 1 - g_dominant;
    const VRHandState& oh = in->hand[off];
    const bool poses = g_gripRoomValid[off] && g_gripRoomValid[g_dominant];

    g_twoHandCandidate = 0;
    g_twoHandPerpM = g_twoHandAlongM = g_twoHandHandDistM = 0.0f;

    float v[3] = { 0, 0, 0 };
    if (poses) {
        for (int i = 0; i < 3; ++i)
            v[i] = g_gripRoomM[off][i] - g_gripRoomM[g_dominant][i];
        g_twoHandHandDistM = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
        g_twoHandAlongM = v[0]*oneHandFwdRoom[0] + v[1]*oneHandFwdRoom[1]
                        + v[2]*oneHandFwdRoom[2];
        float perp[3];
        for (int i = 0; i < 3; ++i)
            perp[i] = v[i] - g_twoHandAlongM * oneHandFwdRoom[i];
        g_twoHandPerpM = sqrtf(perp[0]*perp[0] + perp[1]*perp[1] + perp[2]*perp[2]);
    }

    const float margin = (g_twoHandReleaseM > g_twoHandGrabM)
                       ? g_twoHandReleaseM - g_twoHandGrabM : 0.10f;
    bool want = g_twoHandAttached;
    if (!g_twoHandEnabled || !poses || !oh.squeezeDown) {
        want = false;
    } else if (!g_twoHandAttached) {
        g_twoHandCandidate = g_twoHandAlongM > 0.07f
                          && g_twoHandAlongM < g_twoHandMaxDistM
                          && g_twoHandPerpM <= g_twoHandGrabM
                          && g_twoHandHandDistM > 0.06f;
        want = g_twoHandCandidate != 0;
    } else {
        g_twoHandCandidate = 1;
        // Wider envelope while held: only a real disengagement releases.
        if (g_twoHandAlongM < 0.02f
            || g_twoHandAlongM > g_twoHandMaxDistM + margin
            || g_twoHandHandDistM < 0.05f)
            want = false;
    }

    if (want != g_twoHandAttached) {
        g_twoHandAttached = want;
        g_twoHandChangedEdge = 1;
    }
}

void VRWeapon_Update(const VRInputState* in, double now, int menuMode)
{
    g_menuMode = menuMode != 0;
    g_matchedLastFrame = g_matchedThisFrame;
    g_matchedThisFrame = 0;
    g_adsEnterEdge = g_adsExitEdge = 0;
    g_twoHandChangedEdge = 0;

    static double prevNow = -1.0;
    float dt = (prevNow > 0.0 && now > prevNow) ? (float)(now - prevNow) : (1.0f / 240.0f);
    if (dt > 0.1f) dt = 0.1f;
    prevNow = now;

    float yawRef = 0.0f, headRefM[3] = { 0, 0, 0 };
    const int refs = in ? VR_GetNeutralRefs(&yawRef, headRefM) : 0;
    const bool haveYawRef = (refs & 1) != 0;
    const bool haveMidRef = (refs & 2) != 0;

    const VRHandState* dom = in ? &in->hand[g_dominant] : nullptr;
    const bool inputLive = in && in->valid && in->focused && dom && dom->aimValid && haveYawRef;

    if (in) {
        HandToRoom(in->hand[VRI_LEFT], VRI_LEFT, headRefM, haveMidRef);
        HandToRoom(in->hand[VRI_RIGHT], VRI_RIGHT, headRefM, haveMidRef);
    }

    // Grace: a short tracking dropout keeps the last pose; a long one
    // disengages so the baseline behavior takes over cleanly.
    if (inputLive)
        g_lastPoseTime = now;
    const bool fresh = g_lastPoseTime > 0.0 && (now - g_lastPoseTime) < 1.0;

    // ENGAGE only from STABLE tracking (vr_input's acquire hysteresis): a
    // single transient isActive frame must never flip the camera basis. Once
    // engaged, the 1 s freshness grace above carries the mode through brief
    // pose misses, so the basis cannot alternate frame to frame in either
    // direction.
    const bool engageOk = g_decoupledLive || (dom && VRI_HandTrackingUsable(*dom));
    const bool wantDecoupled = g_enabled && !g_menuMode && !g_scripted
                             && fresh && g_gameAimValid && engageOk;
    if (wantDecoupled && !g_decoupledLive) {
        g_rebasePending = true;      // engage view-stable
        Log("VR weapon: head/weapon decoupling ENGAGED (dominant hand %s)",
            g_dominant == VRI_RIGHT ? "right" : "left");
    } else if (!wantDecoupled && g_decoupledLive) {
        Log("VR weapon: decoupling disengaged (%s)",
            g_menuMode ? "menu" : g_scripted ? "scripted" : !fresh ? "controller lost" : "off");
    }
    g_decoupledLive = wantDecoupled;

    if (g_rebasePending && g_gameAimValid && g_decoupledLive) {
        g_rebasePending = false;
        g_bodyYawDeg = g_gameAimYawDeg;
        g_bodyYawValid = true;
        ++g_rebaseCount;
        Log("VR weapon: body yaw rebased to game aim %.1f deg", g_bodyYawDeg);
    }

    // Serial for the entity-rewrite dedupe: everything cgame adds between two
    // updates belongs to one frame and may transform the persistent view-
    // weapon struct exactly once.
    ++g_updateSerial;

    // ---- weapon pose (raw room first, then world) ----
    g_weaponValid = false;
    if (inputLive && g_bodyYawValid) {
        // One-handed: calibration applied within the dominant aim frame --
        // unless the native weapon is driving, in which case the basis IS
        // the rendered barrel (VRWeapon_SetBarrelOverride) and needs none.
        float oneHandRoom[9];
        if (g_barrelValid
            && VRM_BasisFromForwardUp(g_barrelFwdRoom, g_barrelUpRoom, oneHandRoom)) {
            // rendered bore, as drawn
        } else {
            VRM_MulRows(g_calRows, g_aimRoomRows[g_dominant], oneHandRoom);
        }
        memcpy(g_oneHandFwdRoom, &oneHandRoom[0], sizeof(g_oneHandFwdRoom));

        // Two-hand machine: deliberate grab with attach/release hysteresis;
        // orientation change is carried by the blend ramp (no snap either way).
        TwoHandAdvance(in, &oneHandRoom[0]);
        const float step = (g_twoHandBlendSec > 0.001f) ? dt / g_twoHandBlendSec : 1.0f;
        g_twoHandBlend += g_twoHandAttached ? step : -step;
        if (g_twoHandBlend < 0.0f) g_twoHandBlend = 0.0f;
        if (g_twoHandBlend > 1.0f) g_twoHandBlend = 1.0f;

        float roomRows[9];
        memcpy(roomRows, oneHandRoom, sizeof(roomRows));
        if (g_twoHandBlend > 0.0f && g_gripRoomValid[g_dominant] && g_gripRoomValid[1 - g_dominant]) {
            float v[3];
            for (int i = 0; i < 3; ++i)
                v[i] = g_gripRoomM[1 - g_dominant][i] - g_gripRoomM[g_dominant][i];
            const float vl = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
            if (vl > 0.05f) {
                for (int i = 0; i < 3; ++i)
                    g_twoHandFwdRoom[i] = v[i] / vl;
                // Blend the forward from the one-hand axis toward the
                // hand-to-hand vector; roll stays with the dominant hand's up.
                float f[3];
                for (int i = 0; i < 3; ++i)
                    f[i] = oneHandRoom[i] * (1.0f - g_twoHandBlend)
                         + g_twoHandFwdRoom[i] * g_twoHandBlend;
                float shaped[9];
                if (VRM_BasisFromForwardUp(f, &oneHandRoom[6], shaped))
                    memcpy(roomRows, shaped, sizeof(roomRows));
            }
        }
        memcpy(g_weaponRoomRows, roomRows, sizeof(g_weaponRoomRows));

        // Room -> world: cancel the neutral yaw, then apply the body facing.
        float cancelled[9], bodyRows[9];
        VRM_RotateRowsZ(-yawRef, roomRows, cancelled);
        VRM_YawRows(g_bodyYawDeg * 0.01745329252f, bodyRows);
        VRM_MulRows(cancelled, bodyRows, g_weaponWorldRows);

        // Origin: the grip anchors the weapon. Its offset from the latched
        // neutral head midpoint rotates by (bodyYaw - neutralYaw) -- exactly
        // the transform the head-eye positional path uses -- so hand and head
        // stay rigidly consistent in world space.
        if (g_gripRoomValid[g_dominant] && g_gameAimValid) {
            const float rot = g_bodyYawDeg * 0.01745329252f - yawRef;
            float w[3];
            VRM_RotateVecZ(rot, g_gripRoomM[g_dominant], w);
            const float scale = VR_WorldScale() * VR_PositionalGain();
            for (int i = 0; i < 3; ++i)
                g_weaponWorldOrigin[i] = g_gameVieworg[i] + w[i] * scale;
            g_weaponValid = true;
        }

        VRM_AxisYawPitchDeg(g_weaponWorldRows, &g_targetYawDeg, &g_targetPitchDeg);
        // The game clamps pitch near +-85; aiming past that only winds up the
        // steering loop for nothing.
        if (g_targetPitchDeg >  85.0f) g_targetPitchDeg =  85.0f;
        if (g_targetPitchDeg < -85.0f) g_targetPitchDeg = -85.0f;
    } else {
        g_twoHandBlend = 0.0f;
        g_twoHandCandidate = 0;
        if (g_twoHandAttached) { g_twoHandAttached = false; g_twoHandChangedEdge = 1; }
    }

    // ---- physical ADS detection (dwell + hysteresis, never flickers) ----
    bool adsGeometry = false;
    if (g_physicalAds && g_decoupledLive && g_gripRoomValid[g_dominant]) {
        float headM[3];
        if (VR_GetHeadRoomMid(headM) && haveMidRef) {
            float headRoom[3];
            for (int i = 0; i < 3; ++i)
                headRoom[i] = headM[i] - headRefM[i];
            float e2g[3];
            for (int i = 0; i < 3; ++i)
                e2g[i] = g_gripRoomM[g_dominant][i] - headRoom[i];
            const float d = sqrtf(e2g[0]*e2g[0] + e2g[1]*e2g[1] + e2g[2]*e2g[2]);
            g_adsLastDistM = d;
            float ang = 180.0f;
            if (d > 0.02f) {
                const float* f = &g_weaponRoomRows[0];
                float dot = (e2g[0]*f[0] + e2g[1]*f[1] + e2g[2]*f[2]) / d;
                if (dot >  1.0f) dot =  1.0f;
                if (dot < -1.0f) dot = -1.0f;
                ang = acosf(dot) * 57.2957795f;
            }
            g_adsLastAngDeg = ang;
            const float distThr = g_adsActive ? g_adsExitDistM  : g_adsEnterDistM;
            const float angThr  = g_adsActive ? g_adsExitAngDeg : g_adsEnterAngDeg;
            adsGeometry = d <= distThr && ang <= angThr;
        }
    }
    if (!g_adsActive) {
        if (adsGeometry) {
            if (g_adsCandidateSince < 0.0) g_adsCandidateSince = now;
            if (now - g_adsCandidateSince >= 0.12) {
                g_adsActive = true;
                g_adsEnterEdge = 1;
                g_adsLostSince = -1.0;
            }
        } else {
            g_adsCandidateSince = -1.0;
        }
    } else {
        if (!adsGeometry || !g_decoupledLive) {
            if (g_adsLostSince < 0.0) g_adsLostSince = now;
            if (now - g_adsLostSince >= 0.20 || !g_decoupledLive) {
                g_adsActive = false;
                g_adsExitEdge = 1;
                g_adsCandidateSince = -1.0;
            }
        } else {
            g_adsLostSince = -1.0;
        }
    }
}

int VRWeapon_Decoupled() { return g_decoupledLive ? 1 : 0; }

int VRWeapon_GetViewBasisRows(float out[9])
{
    if (!g_decoupledLive || !g_bodyYawValid)
        return 0;
    VRM_YawRows(g_bodyYawDeg * 0.01745329252f, out);
    return 1;
}

int VRWeapon_GetWeaponWorld(float origin[3], float axis[9])
{
    if (!g_weaponValid)
        return 0;
    if (origin) memcpy(origin, g_weaponWorldOrigin, sizeof(g_weaponWorldOrigin));
    if (axis)   memcpy(axis, g_weaponWorldRows, sizeof(g_weaponWorldRows));
    return 1;
}

int VRWeapon_GetAimTargetDeg(float* yawDeg, float* pitchDeg)
{
    if (!g_decoupledLive || !g_weaponValid)
        return 0;
    if (yawDeg)   *yawDeg   = g_targetYawDeg;
    if (pitchDeg) *pitchDeg = g_targetPitchDeg;
    return 1;
}

int VRWeapon_AdsJustEntered()    { return g_adsEnterEdge; }
int VRWeapon_AdsJustExited()     { return g_adsExitEdge; }
int VRWeapon_TwoHandJustChanged(){ return g_twoHandChangedEdge; }

int VRWeapon_GetGameAimDeg(float* yawDeg, float* pitchDeg, double* when)
{
    if (!g_gameAimValid)
        return 0;
    if (yawDeg)   *yawDeg   = g_gameAimYawDeg;
    if (pitchDeg) *pitchDeg = g_gameAimPitchDeg;
    if (when)     *when     = g_gameAimTime;
    return 1;
}

int VRWeapon_GetMoveRefYawDeg(int useOffhand, float* yawDeg)
{
    if (!yawDeg || !g_bodyYawValid)
        return 0;
    if (useOffhand) {
        const int off = 1 - g_dominant;
        if (g_aimRoomValid[off]) {
            float yawRef = 0.0f, refM[3];
            if (VR_GetNeutralRefs(&yawRef, refM) & 1) {
                float cancelled[9];
                VRM_RotateRowsZ(-yawRef, g_aimRoomRows[off], cancelled);
                float y = 0, p = 0;
                VRM_AxisYawPitchDeg(cancelled, &y, &p);
                *yawDeg = VRM_WrapDeg(g_bodyYawDeg + y);
                return 1;
            }
        }
        // fall through to HMD when the off hand is not tracking
    }
    float headYawOff = 0, headPitchOff = 0;
    VR_GetHeadAimOffset(&headYawOff, &headPitchOff);
    *yawDeg = VRM_WrapDeg(g_bodyYawDeg + headYawOff);
    return 1;
}

// ------------------------------------------------------------ weapon ents

// Entity census: cumulative buckets over every entity the classifier saw, so
// the next hardware log can prove or disprove the classifier against the real
// entity population. The 2026-08-20 run reported "0 entities transformed" and
// could not distinguish "classifier wrong" from "gate off upstream" (it was
// the gate: no neutral midpoint -> no grip -> no weapon pose).
static int   g_censusTotal, g_censusNear, g_censusMidAligned, g_censusMidOther;
static int   g_censusFar, g_censusGateBlocked, g_censusRewrittenTotal;
static int   g_censusDeduped;
static float g_censusNearestU = 1.0e9f;

// Is this entity the local first-person view weapon (or one of its attached
// pieces -- flash, hands)? Judged geometrically against the last REAL refdef:
// the viewmodel is the only entity family that rides the camera. NPC and
// world weapons live at their owners' positions and never match. Returns the
// census bucket: 0 far/none, 1 near (viewmodel), 2 mid-range aligned, and
// -1 for mid-range misaligned (counted, never matched).
static int ClassifyViewWeaponEntity(const float entOrigin[3], const float entAxis[9])
{
    if (!g_gameAimValid)
        return 0;
    float d[3];
    for (int i = 0; i < 3; ++i)
        d[i] = entOrigin[i] - g_gameVieworg[i];
    const float dist = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    if (dist < g_censusNearestU)
        g_censusNearestU = dist;
    if (dist < 14.0f)
        return 1;                           // riding the camera: viewmodel
    if (dist > 40.0f)
        return 0;
    // Between 14 and 40 units (muzzle flash, attached pieces): require the
    // entity's forward to agree with the aim, so a soldier brushing past the
    // camera can never be grabbed.
    float ey = 0, ep = 0;
    VRM_AxisYawPitchDeg(entAxis, &ey, &ep);
    const bool aligned = fabsf(VRM_WrapDeg(ey - g_gameAimYawDeg)) < 20.0f
                      && fabsf(ep - g_gameAimPitchDeg) < 15.0f;
    return aligned ? 2 : -1;
}

int VRWeapon_MaybeRewriteEntity(float entOrigin[3], float entAxis[9],
                                float* entAnglesPYR, unsigned key)
{
    // Dedupe FIRST: cgame offers the same persistent struct several times per
    // frame (two base adds + one pass per attachment). After the first
    // transform the struct already holds the weapon pose, so a second pass
    // would (a) re-apply the delta on top of itself and (b) mis-classify the
    // moved origin. Repeats pass through untouched -- which is exactly what
    // the engine should see, since every offer aliases the same memory.
    if (key) {
        for (int i = 0; i < REWRITE_MEMO; ++i) {
            if (g_rewriteKey[i] == key && g_rewriteSerial[i] == g_updateSerial) {
                ++g_censusDeduped;
                return 0;
            }
        }
    }

    // Census BEFORE the gate: the classifier's answer is recorded whether or
    // not the transform is allowed to run this frame.
    const int cls = ClassifyViewWeaponEntity(entOrigin, entAxis);
    ++g_censusTotal;
    if      (cls == 1)  ++g_censusNear;
    else if (cls == 2)  ++g_censusMidAligned;
    else if (cls == -1) ++g_censusMidOther;
    else                ++g_censusFar;

    const bool isViewWeapon = cls == 1 || cls == 2;
    if (!g_transformEnabled || !g_decoupledLive || !g_weaponValid || g_menuMode || g_scripted) {
        if (isViewWeapon)
            ++g_censusGateBlocked;
        return 0;
    }
    if (!isViewWeapon)
        return 0;

    // The game's aim frame carries no roll; rebuild it from yaw/pitch so the
    // delta transform is exact even while cgame cants the model.
    float aimRows[9];
    VRM_AnglesToRows(g_gameAimYawDeg, g_gameAimPitchDeg, 0.0f, aimRows);
    const float* aF = &aimRows[0];
    const float* aL = &aimRows[3];
    const float* aU = &aimRows[6];
    const float* wF = &g_weaponWorldRows[0];
    const float* wL = &g_weaponWorldRows[3];
    const float* wU = &g_weaponWorldRows[6];

    // Rotate a world vector from the aim frame into the weapon frame.
    #define ROT(v, out) do {                                              \
        const float c0 = (v)[0]*aF[0] + (v)[1]*aF[1] + (v)[2]*aF[2];      \
        const float c1 = (v)[0]*aL[0] + (v)[1]*aL[1] + (v)[2]*aL[2];      \
        const float c2 = (v)[0]*aU[0] + (v)[1]*aU[1] + (v)[2]*aU[2];      \
        (out)[0] = c0*wF[0] + c1*wL[0] + c2*wU[0];                        \
        (out)[1] = c0*wF[1] + c1*wL[1] + c2*wU[1];                        \
        (out)[2] = c0*wF[2] + c1*wL[2] + c2*wU[2];                        \
    } while (0)

    float local[3];
    for (int i = 0; i < 3; ++i)
        local[i] = entOrigin[i] - g_gameVieworg[i];

    // The measured "baked" offset (how far the model anchor sits from the
    // camera, in the aim frame) is the number the next calibration pass
    // needs; record it from the entity nearest the camera.
    const float dist2 = local[0]*local[0] + local[1]*local[1] + local[2]*local[2];
    if (dist2 < 14.0f * 14.0f) {
        g_bakedOffsetU[0] = local[0]*aF[0] + local[1]*aF[1] + local[2]*aF[2];
        g_bakedOffsetU[1] = local[0]*aL[0] + local[1]*aL[1] + local[2]*aL[2];
        g_bakedOffsetU[2] = local[0]*aU[0] + local[1]*aU[1] + local[2]*aU[2];
        g_bakedOffsetValid = true;
        if (!g_bakedOffsetLogged) {
            g_bakedOffsetLogged = true;
            Log("VR weapon: viewmodel baked offset %.1f fwd, %.1f left, %.1f up units",
                g_bakedOffsetU[0], g_bakedOffsetU[1], g_bakedOffsetU[2]);
        }
        if (!g_anchorKeepModel) {
            // Anchor mode "hand": collapse the baked offset so the model
            // anchor lands exactly on the grip.
            float baked[3];
            for (int i = 0; i < 3; ++i)
                baked[i] = g_bakedOffsetU[0]*aF[i] + g_bakedOffsetU[1]*aL[i] + g_bakedOffsetU[2]*aU[i];
            for (int i = 0; i < 3; ++i)
                local[i] -= baked[i];
        }
    }

    float rotated[3];
    ROT(local, rotated);
    float calWorld[3];
    for (int i = 0; i < 3; ++i)
        calWorld[i] = g_calPosU[0]*wF[i] + g_calPosU[1]*wL[i] + g_calPosU[2]*wU[i];
    for (int i = 0; i < 3; ++i)
        entOrigin[i] = g_weaponWorldOrigin[i] + rotated[i] + calWorld[i];

    float newAxis[9];
    ROT(&entAxis[0], &newAxis[0]);
    ROT(&entAxis[3], &newAxis[3]);
    ROT(&entAxis[6], &newAxis[6]);
    memcpy(entAxis, newAxis, sizeof(newAxis));
    #undef ROT

    // The struct carries the SAME orientation twice: the axis at +12 and
    // Quake-order euler angles (pitch-down positive) at +56. The engine may
    // consume either, so both must describe the new pose. Extracted from the
    // rotated axis (not the raw weapon rows) so cgame's own procedural
    // carry/sway offsets survive relative to the hand.
    if (entAnglesPYR) {
        float y = 0, p = 0, r = 0;
        VRM_AxisToAnglesDeg(newAxis, &y, &p, &r);
        entAnglesPYR[0] = -p;      // quake pitch: positive looks down
        entAnglesPYR[1] = y;
        entAnglesPYR[2] = r;
    }

    if (key) {
        static int slot;
        slot = (slot + 1) % REWRITE_MEMO;
        g_rewriteKey[slot] = key;
        g_rewriteSerial[slot] = g_updateSerial;
    }

    ++g_matchedThisFrame;
    ++g_censusRewrittenTotal;
    return 1;
}

// ------------------------------------------------------------ diagnostics

void VRWeapon_GetDebug(VRWeaponDebug* out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->decoupled = g_decoupledLive ? 1 : 0;
    out->scripted = g_scripted ? 1 : 0;
    out->menu = g_menuMode ? 1 : 0;
    out->bodyYawDeg = g_bodyYawDeg;
    out->targetYawDeg = g_targetYawDeg;
    out->targetPitchDeg = g_targetPitchDeg;
    out->gameYawDeg = g_gameAimYawDeg;
    out->gamePitchDeg = g_gameAimPitchDeg;
    memcpy(out->weaponWorldFwd, &g_weaponWorldRows[0], sizeof(out->weaponWorldFwd));
    memcpy(out->weaponWorldOrigin, g_weaponWorldOrigin, sizeof(out->weaponWorldOrigin));
    out->weaponPoseValid = g_weaponValid ? 1 : 0;
    out->twoHand = g_twoHandAttached ? 1 : 0;
    out->ads = g_adsActive ? 1 : 0;
    out->barrelOverride = (g_barrelValid && g_decoupledLive) ? 1 : 0;
    out->adsEyeDistM = g_adsLastDistM;
    out->adsAlignDeg = g_adsLastAngDeg;
    for (int h = 0; h < VRI_HANDS; ++h) {
        memcpy(out->gripRoomM[h], g_gripRoomM[h], sizeof(out->gripRoomM[h]));
        out->gripRoomValid[h] = g_gripRoomValid[h] ? 1 : 0;
    }
    out->matchedEntities = g_matchedLastFrame;
    memcpy(out->bakedOffsetU, g_bakedOffsetU, sizeof(out->bakedOffsetU));
    out->bakedOffsetValid = g_bakedOffsetValid ? 1 : 0;

    // One-hand chain proof: recompute the origin from the raw pieces and
    // compare with what the solver shipped. Nonzero means a term was dropped
    // or something mutated the pose after the solve.
    out->solverPosErrU = -1.0f;
    memcpy(out->gameVieworg, g_gameVieworg, sizeof(out->gameVieworg));
    if (g_weaponValid && g_gripRoomValid[g_dominant] && g_bodyYawValid) {
        float yawRef = 0.0f, refM[3];
        if (VR_GetNeutralRefs(&yawRef, refM) & 1) {
            float w[3];
            VRM_RotateVecZ(g_bodyYawDeg * 0.01745329252f - yawRef,
                           g_gripRoomM[g_dominant], w);
            const float scale = VR_WorldScale() * VR_PositionalGain();
            float err = 0.0f;
            for (int i = 0; i < 3; ++i) {
                const float d = g_gameVieworg[i] + w[i] * scale - g_weaponWorldOrigin[i];
                err += d * d;
            }
            out->solverPosErrU = sqrtf(err);
        }
    }

    out->twoHandCandidate = g_twoHandCandidate;
    out->twoHandPerpM     = g_twoHandPerpM;
    out->twoHandAlongM    = g_twoHandAlongM;
    out->twoHandHandDistM = g_twoHandHandDistM;
    out->twoHandBlend     = g_twoHandBlend;
    memcpy(out->twoHandFwdRoom,   g_twoHandFwdRoom,   sizeof(out->twoHandFwdRoom));
    memcpy(out->oneHandFwdRoom,   g_oneHandFwdRoom,   sizeof(out->oneHandFwdRoom));
    memcpy(out->weaponRoomFwd,    &g_weaponRoomRows[0], sizeof(out->weaponRoomFwd));
    memcpy(out->rearAnchorRoomM,  g_gripRoomM[g_dominant], sizeof(out->rearAnchorRoomM));
    memcpy(out->frontAnchorRoomM, g_gripRoomM[1 - g_dominant], sizeof(out->frontAnchorRoomM));

    out->censusTotal      = g_censusTotal;
    out->censusNear       = g_censusNear;
    out->censusMidAligned = g_censusMidAligned;
    out->censusMidOther   = g_censusMidOther;
    out->censusFar        = g_censusFar;
    out->censusRewritten  = g_censusRewrittenTotal;
    out->censusGateBlocked = g_censusGateBlocked;
    out->censusDeduped    = g_censusDeduped;
    out->censusNearestU   = g_censusTotal ? g_censusNearestU : 0.0f;
}

void VRWeapon_TestReset()
{
    g_enabled = 1;
    g_dominant = VRI_RIGHT;
    g_transformEnabled = 1;
    g_anchorKeepModel = 1;
    g_calPosU[0] = g_calPosU[1] = g_calPosU[2] = 0.0f;
    memset(g_calRows, 0, sizeof(g_calRows));
    g_calRows[0] = g_calRows[4] = g_calRows[8] = 1.0f;
    g_physicalAds = 1;
    g_adsEnterDistM = 0.32f; g_adsExitDistM = 0.42f;
    g_adsEnterAngDeg = 15.0f; g_adsExitAngDeg = 25.0f;
    g_twoHandEnabled = 1; g_twoHandMaxDistM = 0.75f; g_twoHandBlendSec = 0.12f;
    g_twoHandGrabM = 0.22f; g_twoHandReleaseM = 0.32f;

    g_bodyYawDeg = 0.0f;
    g_bodyYawValid = false;
    g_scripted = false;
    g_rebasePending = true;
    g_barrelValid = 0;
    g_gameAimYawDeg = g_gameAimPitchDeg = 0.0f;
    memset(g_gameVieworg, 0, sizeof(g_gameVieworg));
    memset(g_gameAimRows, 0, sizeof(g_gameAimRows));
    g_gameAimRows[0] = g_gameAimRows[4] = g_gameAimRows[8] = 1.0f;
    g_gameAimTime = -1.0;
    g_gameAimValid = false;
    g_weaponValid = false;
    g_lastPoseTime = -1.0;
    g_decoupledLive = false;
    g_menuMode = true;
    g_twoHandAttached = false;
    g_twoHandBlend = 0.0f;
    g_twoHandChangedEdge = 0;
    g_twoHandCandidate = 0;
    g_twoHandPerpM = g_twoHandAlongM = g_twoHandHandDistM = 0.0f;
    memset(g_twoHandFwdRoom, 0, sizeof(g_twoHandFwdRoom));
    memset(g_oneHandFwdRoom, 0, sizeof(g_oneHandFwdRoom));
    g_rebaseCount = 0;
    g_updateSerial = 0;
    memset(g_rewriteKey, 0, sizeof(g_rewriteKey));
    memset(g_rewriteSerial, 0, sizeof(g_rewriteSerial));
    g_adsActive = false;
    g_adsEnterEdge = g_adsExitEdge = 0;
    g_adsCandidateSince = g_adsLostSince = -1.0;
    g_adsLastDistM = 0.0f;
    g_adsLastAngDeg = 0.0f;
    g_matchedThisFrame = g_matchedLastFrame = 0;
    g_censusTotal = g_censusNear = g_censusMidAligned = g_censusMidOther = 0;
    g_censusFar = g_censusGateBlocked = g_censusRewrittenTotal = 0;
    g_censusDeduped = 0;
    g_censusNearestU = 1.0e9f;
    memset(g_bakedOffsetU, 0, sizeof(g_bakedOffsetU));
    g_bakedOffsetValid = false;
    g_bakedOffsetLogged = false;
    memset(g_gripRoomM, 0, sizeof(g_gripRoomM));
    memset(g_gripRoomValid, 0, sizeof(g_gripRoomValid));
    memset(g_aimRoomRows, 0, sizeof(g_aimRoomRows));
    memset(g_aimRoomValid, 0, sizeof(g_aimRoomValid));
}

void VRWeapon_TestForcePose(const float origin[3], const float axis[9])
{
    ++g_updateSerial;      // a forced pose opens a fresh rewrite frame
    memcpy(g_weaponWorldOrigin, origin, sizeof(g_weaponWorldOrigin));
    memcpy(g_weaponWorldRows, axis, sizeof(g_weaponWorldRows));
    memcpy(g_weaponRoomRows, axis, sizeof(g_weaponRoomRows));
    g_weaponValid = true;
    g_decoupledLive = true;
    g_menuMode = false;
    g_scripted = false;
    g_bodyYawValid = true;
    VRM_AxisYawPitchDeg(g_weaponWorldRows, &g_targetYawDeg, &g_targetPitchDeg);
}
