// The weapon/aim model: explicit body / head / weapon spaces.
//
// Spaces (all conversions live in vr_weapon.cpp and are offline-tested):
//   1. XR LOCAL space          - what OpenXR reports (metres, +Y up, -Z fwd)
//   2. room space              - LOCAL mapped onto Quake axes (VRM_XrToQuakeVec /
//                                VRM_QuatToQuakeAxis), positions still metres,
//                                relative to the latched neutral head midpoint
//   3. body space              - room rotated so the latched neutral yaw is
//                                forward; the body's facing in the game world
//                                is g_bodyYaw (game degrees)
//   4. game world              - Quake units; the game's own vieworg/viewaxis
//
// The decoupling contract:
//   * The HMD owns the rendered camera: view basis = yaw(bodyYaw) with the
//     head's full local orientation composed on top by vr.cpp's existing
//     roll-stable pipeline (this module only supplies the yaw-only base).
//   * The DOMINANT controller owns the weapon: rendered weapon pose AND the
//     gameplay aim target both derive from ONE calibrated weapon axis, so
//     rendered muzzle forward and gameplay shot forward cannot disagree by
//     construction. The remaining error is the steering loop's convergence,
//     which is measured and reported, never hidden.
//   * The BODY only rotates on stick turn, snap turn, or an explicit rebase
//     (spawn/scripted jump). Head yaw never aims the gun; gun yaw never turns
//     the camera.
#pragma once

#include "vr_input.h"

// ------------------------------------------------------------- per frame

// Real game aim (before any render-time substitution), from the render hook.
// gameAxis is the refdef's 3x3 row basis, gameOrigin the vieworg.
void VRWeapon_NoteGameAim(const float gameAxis[9], const float gameOrigin[3], double now);

// Advance the model once per game frame (swap hook, after VRInput_Sync).
// menuMode = no 3D scene was rendered recently.
void VRWeapon_Update(const VRInputState* in, double now, int menuMode);

// ---------------------------------------------------------------- outputs

// True while the head/weapon decoupling is live (controllers healthy, not
// scripted, not menu, feature enabled). When false the render path falls back
// to the shipped head-on-game-aim composition, byte-identical to the
// baseline build.
int VRWeapon_Decoupled();

// The yaw-only body basis the render hook substitutes for the game's aim
// axis while decoupled. Returns 0 when not decoupled (caller keeps gameAxis).
int VRWeapon_GetViewBasisRows(float out[9]);

// World-space weapon pose (origin in game units, axis rows forward/left/up),
// including grip anchoring, calibration and two-hand shaping. Returns 0 when
// no valid pose exists this frame.
int VRWeapon_GetWeaponWorld(float origin[3], float axis[9]);

// The gameplay aim the steering loop drives the game toward, degrees.
int VRWeapon_GetAimTargetDeg(float* yawDeg, float* pitchDeg);

// Body rotation from stick / snap turn.
void VRWeapon_AddBodyYawDeg(float degrees);
float VRWeapon_BodyYawDeg();

// Scripted-camera mode (set by the steering loop's effectiveness monitor):
// decoupling and weapon transforms suspend; the baseline composition runs.
void VRWeapon_SetScripted(int on);
int  VRWeapon_Scripted();

// Adopt the game's current aim yaw as the body facing (engagement, spawn,
// scripted jump). View-stable by construction.
void VRWeapon_RebaseToGameAim();

// vr.cpp calls this when the neutral yaw relatches (recenter): the body turns
// WITH the redefinition so nothing on screen moves.
void VRWeapon_OnNeutralRelatched(float deltaYawRad);

// The last REAL game aim (yaw/pitch degrees) and when it was observed --
// the steering loop's feedback signal. Returns 0 before any 3D scene.
int VRWeapon_GetGameAimDeg(float* yawDeg, float* pitchDeg, double* when);

// World yaw of the movement reference: HMD gaze (useOffhand=0) or the
// off-hand aim (useOffhand=1, falling back to HMD when it is not tracking).
int VRWeapon_GetMoveRefYawDeg(int useOffhand, float* yawDeg);

// ------------------------------------------------------------ weapon ents

// Offered every first-person-candidate entity add (float origin[3] at +0,
// 3x3 axis at +12, euler angles pitch/yaw/roll at +56, in game world space).
// If it is classified as the local view weapon and a weapon pose exists,
// origin/axis/angles are rewritten in place and 1 is returned. WRITE-THROUGH
// semantics: the caller forwards the modified struct and does NOT restore it
// -- the view-weapon entity is a persistent cgame global (VA base+0x12573C)
// that the engine reads AFTER the add syscall returns (all of a frame's GL
// runs after the render syscall), so a restore-after-return silently undoes
// the transform. That restore was the 2026-08-20 "gun pinned to a point
// below me": rotation still tracked (mouse steering drives the game's own
// viewangles, which cgame writes into the same struct) while our origin
// rewrite never survived to the engine's read.
//
// cgame refills the struct every frame, and the same struct is passed once
// per attachment add (syscall 38) after the base add (37) plus a second base
// add per frame -- so `key` (the struct pointer) dedupes: only the FIRST
// offer of a given key per VRWeapon_Update interval is transformed; repeats
// pass through already-transformed. anglesPYR may be null (tests).
int VRWeapon_MaybeRewriteEntity(float entOrigin[3], float entAxis[9],
                                float* entAnglesPYR = nullptr, unsigned key = 0);

// THE PHYSICAL BARREL (brief 2026-09-03, section 10). While the native
// weapon is driving, vr_native_hands hands over the rendered gun's bore
// direction and up vector in the room frame (Quake axes, the same frame the
// controller aim pose is read in). It replaces the raw controller aim pose
// as the source of the weapon basis, so the fire direction IS the rendered
// barrel by construction. Invalid = fall back to the aim pose.
void VRWeapon_SetBarrelOverride(int valid, const float fwdRoom[3], const float upRoom[3]);
int  VRWeapon_BarrelOverrideActive();

// Kill switch (config vr_weapon_transform).
void VRWeapon_SetTransformEnabled(int on);
int  VRWeapon_TransformEnabled();

// -------------------------------------------------------------- ADS / 2H

int VRWeapon_AdsActive();          // physical ADS detected (with hysteresis)
int VRWeapon_AdsJustEntered();     // edge, consumed by the binds layer
int VRWeapon_AdsJustExited();
int VRWeapon_TwoHandAttached();
int VRWeapon_TwoHandJustChanged(); // edge for the haptic tick

// ---------------------------------------------------------------- config

void VRWeapon_SetEnabled(int on);              // vr_aim_mode: 0 legacy, 1 controller
int  VRWeapon_Enabled();
void VRWeapon_SetDominantHand(int hand);       // VRI_LEFT / VRI_RIGHT
int  VRWeapon_DominantHand();
void VRWeapon_SetCalibration(float posX, float posY, float posZ,
                             float pitchDeg, float yawDeg, float rollDeg);
void VRWeapon_SetAnchorMode(int keepModelOffset);   // 1 = keep baked offset (default)
void VRWeapon_SetPhysicalAds(int on);
int  VRWeapon_PhysicalAds();
void VRWeapon_SetAdsThresholds(float enterDistM, float exitDistM,
                               float enterAngDeg, float exitAngDeg);
void VRWeapon_SetTwoHand(int enabled, float maxDistM, float blendSeconds);
// Attach gate: at attach time the support hand must lie within grabRadiusM of
// the one-hand rifle line (perpendicular distance). releaseRadiusM > grab;
// the difference doubles as the along-axis detach hysteresis margin, so a
// hand hovering at any boundary cannot chatter the attach state.
void VRWeapon_SetTwoHandRadii(float grabRadiusM, float releaseRadiusM);
void VRWeapon_GetTwoHandRadii(float* grabRadiusM, float* releaseRadiusM);

// Bumps every time the body yaw rebases to the game aim (engagement, spawn,
// scripted jump). The binds layer watches it to reset its assumed ADS-toggle
// state: a spawn/load always returns the game to hip-fire.
int VRWeapon_GetRebaseCount();

// ------------------------------------------------------------ diagnostics

struct VRWeaponDebug {
    int   decoupled, scripted, menu;
    float bodyYawDeg;
    float targetYawDeg, targetPitchDeg;
    float gameYawDeg, gamePitchDeg;      // last observed real aim
    float weaponWorldFwd[3];
    float weaponWorldOrigin[3];
    int   weaponPoseValid;
    int   twoHand;
    int   ads;
    int   barrelOverride;                // the basis came from the rendered barrel
    float adsEyeDistM, adsAlignDeg;
    float gripRoomM[2][3];               // per hand, raw room metres
    int   gripRoomValid[2];
    int   matchedEntities;               // rewrites last 3D frame
    float bakedOffsetU[3];               // measured viewmodel offset, aim frame
    int   bakedOffsetValid;

    // One-hand chain proof: the weapon origin recomputed independently from
    // gripRoom + body yaw + vieworg, against the shipped origin. > a few mm
    // of disagreement means a term was dropped somewhere in the transform.
    float solverPosErrU;
    float gameVieworg[3];                // last real vieworg (body anchor probe)

    // Two-hand machine, everything the acceptance tests need to read.
    int   twoHandCandidate;              // this frame's attach-gate answer
    float twoHandPerpM;                  // support hand to one-hand rifle line
    float twoHandAlongM;                 // support hand along that line
    float twoHandHandDistM;              // dominant grip to support grip
    float twoHandBlend;                  // 0..1 orientation blend
    float twoHandFwdRoom[3];             // hand-to-hand line, room frame
    float oneHandFwdRoom[3];             // dominant-only forward, room frame
    float weaponRoomFwd[3];              // final shaped forward, room frame
    float rearAnchorRoomM[3];            // dominant grip (room, metres)
    float frontAnchorRoomM[3];           // support grip (room, metres)

    // Entity census (cumulative): every syscall-37/38 entity that reached the
    // classifier, bucketed, so a hardware log proves the classifier against
    // the game's real entity population even when the transform gate is off.
    int   censusTotal;                   // entities offered
    int   censusNear;                    // < 14 u of the camera (viewmodel)
    int   censusMidAligned;              // 14..40 u, forward agrees with aim
    int   censusMidOther;                // 14..40 u, not aligned
    int   censusFar;                     // > 40 u (world/NPC)
    int   censusRewritten;               // actually transformed
    int   censusGateBlocked;             // classified view-weapon, gate off
    int   censusDeduped;                 // same struct re-offered same frame
    float censusNearestU;                // nearest entity ever offered
};
void VRWeapon_GetDebug(VRWeaponDebug* out);

// Testing seam: full cold reset.
void VRWeapon_TestReset();

// Testing seam: force a live decoupled state with an explicit weapon world
// pose, so the entity-rewrite path can be driven without the input stack.
void VRWeapon_TestForcePose(const float origin[3], const float axis[9]);
