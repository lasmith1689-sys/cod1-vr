// The centralized binding layer: every VR control that reaches the game goes
// through here, as injected keyboard/mouse events (the same SendInput channel
// the shipped snap turn already uses -- the one input path proven to reach
// CoD's DirectInput reader).
//
// Also owns the AIM STEERING loop: the game's own view angles are the weapon,
// and each frame this injects relative mouse motion driving them toward the
// controller-derived target from vr_weapon. Counts-per-degree is measured
// live from injected-vs-observed motion (auto-calibration), so the loop works
// at any in-game sensitivity; an effectiveness monitor detects "input dead"
// (cutscene, death cam, menus mid-transition) and flips the weapon module
// into scripted mode, where the baseline rendering behavior takes over.
#pragma once

// One update per game frame, from the swap hook, after VRWeapon_Update.
// menuMode = no 3D scene rendered recently (menu / loading / briefing).
void VRBinds_Update(double now, int menuMode);

// Force-release every synthetic key and mouse button this layer is holding.
// Idempotent; safe from any thread state; logs when it actually released
// something. Called on every boundary where updates may stop or the desire
// behind a held key may vanish: input not live, menu entry, session
// STOPPING/teardown, DLL unload. Inside VRBinds_Update the same guarantee is
// structural -- every update rebuilds the complete desired key set and
// reconciles the held set against it, so a key can only stay down by being
// wanted again that same update.
void VRBinds_ReleaseAllInjectedInputs(const char* reason);

// ---------------------------------------------------------------- config

enum {
    VRB_FIRE = 0, VRB_ADS, VRB_JUMP, VRB_CROUCH, VRB_PRONE, VRB_MELEE,
    VRB_RELOAD, VRB_USE, VRB_GRENADE, VRB_MENU, VRB_CONTROL_COUNT
};

// keyName: a Comfort_ParseKey name, or MOUSE1/MOUSE2/MOUSE3, SPACE, SHIFT,
// CTRL, ESC, ENTER, WHEELUP, WHEELDOWN, NONE. Returns 0 if unparseable.
int  VRBinds_SetBinding(int control, const char* keyName);

// One-shot tap of a control's bound key (down+up), for gesture systems that
// live outside this module -- the physical reload's single commit is the
// production caller. Counted per control; VRBinds_TapCount reads it back so
// the reload's exactly-once contract stays assertable end to end.
void     VRBinds_TapControl(int control);
unsigned VRBinds_TapCount(int control);
// Reload-stage feedback uses the same global haptic toggle and strength.
void VRBinds_ReloadFeedback(int hand, float scale, int durationMs);
int  VRBinds_MenuMode();

void VRBinds_SetTurnMode(int snap);            // 0 smooth (default), 1 snap, 2 off
void VRBinds_SetSnapAngle(float degrees);      // default 30
void VRBinds_SetSmoothTurnSpeed(float degPerSec); // default 180
void VRBinds_SetMoveDirection(int offhand);    // 0 hmd (default), 1 off-hand
void VRBinds_SetHaptics(int on, float strength01);
void VRBinds_SetPhysicalCrouch(int on, float dropMetres);
void VRBinds_SetSteering(float gain, float seedCountsPerDegree);
void VRBinds_SetMenuCursorSpeed(float pxPerSec);

// ---- deliberate one-shot actions (grenade / weapon cycle) ----
//
// Both were accidental-fire sources on the 2026-08-23 hardware run. They are
// now the only readers of their controls and both need an observed clean
// release before they can fire again.
void VRBinds_SetGrenadeHold(float seconds);        // default 0.35, clamp 0..2
void VRBinds_SetWeaponCycleBinding(int binding);   // 0 off-hand X, 2 X tap reload/hold cycle,
                                                   // 1 legacy dominant stick Y
struct VRBindsGrenadeStats {
    int         requests;        // holds that began while armed
    int         accepted;        // grenades actually injected
    int         rejectedNoise;   // refused: no gameplay / no tracking / holdoff
    const char* source;          // what committed the last accepted grenade
    const char* lastReject;      // why the last refusal was refused
    int         armed;           // a clean release has been observed
    float       holdSeconds;
    int         cycleBinding;
};
void VRBinds_GetGrenadeStats(VRBindsGrenadeStats* out);

// ---- experimental weapon control (controller-derived view steering) ----
//
// OFF by default. While off, this layer injects locomotion and buttons only;
// no controller-derived mouse motion reaches the game's view angles, so the
// camera is the game's base pose plus the HMD and nothing else.
//
// Disarmed for the CAMERA-STABLE BASELINE after the 2026-08-23 hardware run,
// in which steering closed a positive-feedback loop with the body-yaw rebase.
// Physical weapon control returns via the DObj bone controller, which poses
// the gun directly and never writes the view angles.
void VRBinds_SetExperimentalWeapon(int on);
int  VRBinds_ExperimentalWeapon();
int  VRBinds_SteeringArmed();          // 1 only if enabled AND not locked out

// Latching kill switch, driven by the stereo fail-safe. Once tripped,
// experimental weapon control stays off for the rest of the session even if
// config re-enables it.
void VRBinds_LockOutExperimentalWeapon(const char* why);
int  VRBinds_ExperimentalWeaponLockedOut();

// ---- body-yaw rebase governor ------------------------------------------
// A rebase is legitimate only at a real state transition. These expose the
// count, the last reason, and how many were REFUSED by the rate limiter --
// a nonzero refusal count means something is trying to chase the game's aim.
unsigned    VRBinds_BodyYawRebaseCount();
const char* VRBinds_BodyYawRebaseReason();
unsigned    VRBinds_BodyYawRebaseRefused();

// Test seam: request a rebase through the governor, so the rate limiter can
// be exercised without driving the whole steering loop.
void VRBinds_TestRequestRebase(const char* reason);

// ---- ADS accuracy-state ownership -------------------------------------
//
// CoD1's ADS (weapalt) is a TOGGLE this layer can only tap, so the layer
// runs a desired-state reconciler over it: desired = ANY owner active --
//   manual   : dominant grip held (deliberate hold-to-aim)
//   physical : vr_weapon's bring-the-sights-to-the-eye detector
//   twohand  : support hand attached to the rifle (the ADS accuracy benefit
//              of a braced two-handed hold -- pure gameplay state; it never
//              re-poses the rendered weapon, rotates the camera, or touches
//              the projection)
// The tracked "assumed" game state resets to hip-fire on every boundary the
// game itself resets it (spawn/load rebase, weapon switch, scripted entry,
// input loss), and can optionally be corrected from cgame's real zoom
// fraction (the hint provider) when the correction is enabled.

// Provider of cgame's ADS/zoom float: returns 1 with the value (0.0 =
// hip-fire, nonzero = ADS engaged/transitioning) or 0 when unreadable.
// Production wires CODVR_GetCgameAdsHint; tests substitute their own.
typedef int (*VRBindsAdsHintFn)(float* value);
void VRBinds_SetAdsHintProvider(VRBindsAdsHintFn fn);
// Off (default): the hint is only logged in the check block. On: a sustained
// hint/assumed mismatch overwrites the assumed state (codvr.cfg "vr_ads_hint").
void VRBinds_SetAdsHintCorrection(int on);

// ------------------------------------------------------------ diagnostics

struct VRBindsDebug {
    int   steeringLive;          // injecting this frame
    float cpdYaw, cpdPitch;      // measured counts-per-degree (signed)
    float steerErrYawDeg, steerErrPitchDeg;
    int   effectiveness;         // 0..100, recent observed/expected %
    int   scriptedDetected;
    int   heldKeys;              // injected keys currently held
    int   movementSector;        // -1 idle, else 0..7
    int   steerLeakCount;        // frames where steering moved the RENDERED
                                 // camera (must stay 0 while decoupled -- the
                                 // Phase-4 head/gun invariant, enforced live)
    // ADS ownership machine.
    int   adsDesired;            // what this frame wants the game state to be
    int   adsAssumed;            // what the toggle bookkeeping believes it is
    int   adsOwnerMask;          // 1 manual grip, 2 physical sights, 4 two-hand
    int   adsTransitions;        // reconciler taps since start
    int   adsHintValid;          // cgame zoom float readable this frame
    float adsHintValue;          // its raw value (0.0 = hip-fire)
    int   adsHintCorrections;    // times the hint overrode the assumed state
};
void VRBinds_GetDebug(VRBindsDebug* out);

// Cumulative injection counters: the proof layer between "OpenXR action state
// changed" and "the game received an event". Every DOWN/UP the reconciler
// emits is classified here by what the key means.
struct VRBindsCounters {
    int moveFwdDown,  moveFwdUp;      // W
    int moveBackDown, moveBackUp;     // S
    int strafeLDown,  strafeLUp;      // A
    int strafeRDown,  strafeRUp;      // D
    int turnEvents;                   // smooth-turn frames + snap turns
    int fireDown,  fireUp;
    int jumpDown,  jumpUp;
    int meleeDown, meleeUp;
    int useDown,   useUp;
    int menuClickDown, menuClickUp;   // MOUSE1 as menu cursor click
    int crouchTaps, proneTaps, reloadTaps, grenadeTaps, adsTaps;
    int weaponCycleTaps, menuTaps, recenters;
    int releaseAllRuns;               // release-all calls that released > 0
};
void VRBinds_GetCounters(VRBindsCounters* out);

// The one-shot "VR INPUT / WEAPON CHECK" block for the next hardware run.
// Called every frame; emits once when the data is meaningful.
void VRBinds_MaybeLogInputCheck(double now);

// -------------------------------------------------------------- test seams

// kinds the sink receives
enum { VRB_EV_KEYDOWN = 1, VRB_EV_KEYUP, VRB_EV_MBTNDOWN, VRB_EV_MBTNUP,
       VRB_EV_MOUSEMOVE, VRB_EV_WHEEL };
typedef void (*VRBindsSink)(int kind, int a, int b);   // key/btn or dx,dy
void VRBinds_TestSetSink(VRBindsSink sink);            // null restores SendInput
void VRBinds_TestReset();                              // cold state, keep sink
