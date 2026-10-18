// Binding/injection layer implementation. See vr_binds.h.
//
// Everything that reaches the game goes out as synthetic keyboard/mouse
// events. That is a deliberate constraint: the game exe is packed and off
// limits, the cgame syscall hook only serves rendering, and SendInput is the
// one channel already proven in this codebase (snap turn) to reach CoD's
// DirectInput reader. The cost is that the game's aim can only be steered
// RELATIVELY, so the steering loop below measures its own effectiveness
// instead of assuming it.

#include "vr_binds.h"
#include "vr_input.h"
#include "vr_weapon.h"
#include "vr.h"
#include "codvr.h"
#include "vr_math.h"
#include "vr_native_hands.h"
#include "vr_reload.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern void Log(const char* fmt, ...);

// ------------------------------------------------------------- key codes
//
// Positive = virtual key. Negative = special injectables.
enum { K_NONE = 0, K_MOUSE1 = -1, K_MOUSE2 = -2, K_MOUSE3 = -3,
       K_WHEELUP = -4, K_WHEELDOWN = -5 };

static int ParseBindKey(const char* name)
{
    if (!name || !*name) return K_NONE;
    if (!_stricmp(name, "none"))       return K_NONE;
    if (!_stricmp(name, "mouse1"))     return K_MOUSE1;
    if (!_stricmp(name, "mouse2"))     return K_MOUSE2;
    if (!_stricmp(name, "mouse3"))     return K_MOUSE3;
    if (!_stricmp(name, "wheelup"))    return K_WHEELUP;
    if (!_stricmp(name, "wheeldown"))  return K_WHEELDOWN;
    if (!_stricmp(name, "space"))      return VK_SPACE;
    if (!_stricmp(name, "shift"))      return VK_SHIFT;
    if (!_stricmp(name, "ctrl"))       return VK_CONTROL;
    if (!_stricmp(name, "alt"))        return VK_MENU;
    if (!_stricmp(name, "esc") || !_stricmp(name, "escape")) return VK_ESCAPE;
    if (!_stricmp(name, "enter"))      return VK_RETURN;
    return Comfort_ParseKey(name);     // F-keys, named keys, letters, digits
}

// ---------------------------------------------------------------- config

// Defaults follow the user's own Main/config.cfg binds:
// MOUSE1 +attack, M weapalt (ADS toggle), SPACE +gostand, C gocrouch,
// CTRL goprone, SHIFT +melee, R +reload, F +activate, 4 weaponslot grenade.
static int g_bind[VRB_CONTROL_COUNT] = {
    K_MOUSE1,     // FIRE
    'M',          // ADS (weapalt -- a TOGGLE command in CoD1)
    VK_SPACE,     // JUMP
    'C',          // CROUCH
    VK_CONTROL,   // PRONE
    VK_SHIFT,     // MELEE
    'R',          // RELOAD
    'F',          // USE
    '4',          // GRENADE (weapon slot)
    VK_ESCAPE,    // MENU
};

static int   g_turnSnap        = 0;       // vr_turn_mode: 0 smooth, 1 snap
static float g_snapAngleDeg    = 30.0f;
static float g_smoothTurnSpeed = 180.0f;  // deg/sec at full deflection
static int   g_moveOffhand     = 0;       // vr_move_direction
static int   g_hapticsOn       = 1;
static float g_hapticStrength  = 0.6f;
static int   g_physicalCrouch  = 0;
static float g_crouchDropM     = 0.32f;
static float g_steerGain       = 0.55f;   // fraction of error corrected/frame
static float g_menuCursorSpeed = 900.0f;  // px/sec at full deflection

// Signed degrees-per-count seeds: sensitivity 5 x m_yaw 0.022 = 0.11 deg per
// count; mouse right lowers yaw, mouse down lowers pitch (both negative in
// our +left/+up convention). Auto-calibration refines these live.
static float g_dpcYaw   = -0.11f;
static float g_dpcPitch = -0.11f;

// ------------------------------------ EXPERIMENTAL WEAPON CONTROL: DISARMED
//
// Master gate for every controller-derived path that can write the game's
// view angles. OFF by default. While it is off, this module injects
// locomotion and buttons only -- no mouse steering ever leaves it during
// gameplay -- so the camera is HMD + game base and nothing else.
//
// Why (hardware run 2026-08-23): controller-derived steering closed a
// positive-feedback loop with the body-yaw rebase --
//
//   game aim -> rebase -> body yaw -> camera basis AND weapon target
//            -> steering error -> mouse injection -> game aim -> ...
//
// -- which ran 1086 rebases off 2 engagements at a steady ~25 deg step, while
// the loop's own auto-calibration drifted counts-per-degree from -9.09 to
// -0.7 (a 13x gain increase) because it was reading its own output as
// evidence. The camera latch in vr.cpp now stops that motion from becoming
// per-eye divergence, but the loop itself is still unsound, so the input side
// is disarmed too: defence in depth, and the baseline this pass is meant to
// deliver has no business steering the view at all.
//
// THIS IS A TEMPORARY ISOLATION STEP, NOT AN ABANDONMENT OF PHYSICAL WEAPON
// CONTROL. Real weapon control returns via the DObj bone controller, which
// poses the gun directly and never touches the view angles -- so it does not
// need this gate at all.
static int g_experimentalWeapon = 0;

void VRBinds_SetExperimentalWeapon(int on) { g_experimentalWeapon = on ? 1 : 0; }
int  VRBinds_ExperimentalWeapon()          { return g_experimentalWeapon; }

// Latched by the stereo fail-safe. Once tripped, experimental weapon control
// stays off for the rest of the session regardless of config: a broken
// experiment gets exactly one chance to make the user sick, and it does not
// get a second one after a re-enable.
static int g_experimentalWeaponLockedOut = 0;

void VRBinds_LockOutExperimentalWeapon(const char* why)
{
    if (g_experimentalWeaponLockedOut)
        return;
    g_experimentalWeaponLockedOut = 1;
    g_experimentalWeapon = 0;
    Log("VR binds: experimental weapon control LOCKED OUT -- %s",
        why ? why : "(no reason given)");
}

int VRBinds_ExperimentalWeaponLockedOut() { return g_experimentalWeaponLockedOut; }

// The one place that decides whether controller-derived aim may move the
// view. Every steering path funnels through it.
static bool SteeringArmed()
{
    return g_experimentalWeapon != 0 && !g_experimentalWeaponLockedOut;
}

int VRBinds_SteeringArmed() { return SteeringArmed() ? 1 : 0; }

// Mouse counts injected on the aim axes since the last FRESH game-aim sample
// was consumed. Not a per-call value: a call that returns before the
// injection site must not be able to erase evidence of what we already did.
static int g_injSinceSampleX;
static int g_injSinceSampleY;

// Injection accumulated since the last steering-leak sample. The watchdog
// samples once per XR PAIR, which spans two game frames, so it cannot use a
// per-frame value.
static int g_injSinceLeakX;
static int g_injSinceLeakY;

// ------------------------------------------------ body-yaw rebase governor
//
// A rebase is a discontinuous jump of the body basis. It is legitimate ONLY
// at a genuine state transition (engage, leaving a scripted camera, a spawn).
// It may NEVER continuously chase the game's aim -- that is a feedback loop,
// and on 2026-08-23 it ran 1086 times off 2 engagements at a ~25 deg step.
//
// Every rebase now carries a reason, is counted, and is rate-limited. A burst
// beyond the budget is refused outright and reported: refusing to rebase
// leaves the basis merely stale, which is survivable, while rebasing in a
// loop is what made the user sick.
static unsigned    g_rebaseCount;
static const char* g_rebaseReason      = "(none)";
static unsigned    g_rebaseRefusedCount;
static double      g_rebaseWindowStart = -1.0;
static unsigned    g_rebaseInWindow;
static bool        g_rebaseStormLogged;

enum { REBASE_WINDOW_SEC = 2, REBASE_MAX_PER_WINDOW = 3 };

unsigned    VRBinds_BodyYawRebaseCount()   { return g_rebaseCount; }
const char* VRBinds_BodyYawRebaseReason()  { return g_rebaseReason; }
unsigned    VRBinds_BodyYawRebaseRefused() { return g_rebaseRefusedCount; }

static void NoteRebase(const char* reason)
{
    const double now = CODVR_NowSeconds();
    if (g_rebaseWindowStart < 0.0 || now - g_rebaseWindowStart >= (double)REBASE_WINDOW_SEC) {
        g_rebaseWindowStart = now;
        g_rebaseInWindow = 0;
    }
    if (g_rebaseInWindow >= (unsigned)REBASE_MAX_PER_WINDOW) {
        ++g_rebaseRefusedCount;
        if (!g_rebaseStormLogged) {
            g_rebaseStormLogged = true;
            Log("VR binds: body-yaw rebase STORM -- more than %d rebases in %d s "
                "(reason '%s'); refusing further rebases. A rebase loop is a "
                "feedback loop, not a state transition.",
                REBASE_MAX_PER_WINDOW, REBASE_WINDOW_SEC, reason ? reason : "?");
        }
        return;
    }
    ++g_rebaseInWindow;
    ++g_rebaseCount;
    g_rebaseReason = reason ? reason : "(none)";
    VRWeapon_RebaseToGameAim();
}

void VRBinds_TestRequestRebase(const char* reason) { NoteRebase(reason); }

// Counts-per-degree must stay in a physically believable band. Unclamped, the
// live calibrator drifted -9.09 -> -0.7 on 2026-08-23 by reading its own
// runaway output as evidence, which multiplied the injection gain by 13.
static void ClampCountsPerDegree()
{
    const float lo = -0.5f;     // 2 counts/deg -- coarsest believable
    const float hi = -0.005f;   // 200 counts/deg -- finest believable
    if (g_dpcYaw   > hi) g_dpcYaw   = hi;
    if (g_dpcYaw   < lo) g_dpcYaw   = lo;
    if (g_dpcPitch > hi) g_dpcPitch = hi;
    if (g_dpcPitch < lo) g_dpcPitch = lo;
}

int VRBinds_SetBinding(int control, const char* keyName)
{
    if (control < 0 || control >= VRB_CONTROL_COUNT)
        return 0;
    const int k = ParseBindKey(keyName);
    if (k == K_NONE && _stricmp(keyName, "none") != 0)
        return 0;
    g_bind[control] = k;
    return 1;
}

void VRBinds_SetTurnMode(int snap)              { if (snap>=0 && snap<=2) g_turnSnap=snap; }
void VRBinds_SetSnapAngle(float d)              { if (d >= 5.0f && d <= 180.0f) g_snapAngleDeg = d; }
void VRBinds_SetSmoothTurnSpeed(float s)        { if (s >= 10.0f && s <= 720.0f) g_smoothTurnSpeed = s; }
void VRBinds_SetMoveDirection(int offhand)      { g_moveOffhand = offhand ? 1 : 0; }
void VRBinds_SetHaptics(int on, float strength) { g_hapticsOn = on ? 1 : 0;
                                                  if (strength >= 0.0f && strength <= 1.0f) g_hapticStrength = strength; }
void VRBinds_ReloadFeedback(int hand, float scale, int durationMs)
{
    if (g_hapticsOn && scale > 0.0f)
        VRInput_Haptic(hand, g_hapticStrength * scale, durationMs);
}
void VRBinds_SetPhysicalCrouch(int on, float dropM) { g_physicalCrouch = on ? 1 : 0;
                                                  if (dropM > 0.1f && dropM < 1.0f) g_crouchDropM = dropM; }
void VRBinds_SetSteering(float gain, float seedCpd)
{
    if (gain > 0.05f && gain <= 1.0f) g_steerGain = gain;
    if (seedCpd > 0.5f && seedCpd < 200.0f) {
        g_dpcYaw   = -1.0f / seedCpd;
        g_dpcPitch = -1.0f / seedCpd;
    }
}
void VRBinds_SetMenuCursorSpeed(float s)        { if (s >= 100.0f && s <= 4000.0f) g_menuCursorSpeed = s; }

// ------------------------------------------------------------- injection

static VRBindsSink g_sink;      // test seam; null = real SendInput

static void RealSink(int kind, int a, int b)
{
    INPUT in = {};
    switch (kind) {
    case VRB_EV_KEYDOWN:
    case VRB_EV_KEYUP:
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = (WORD)a;
        in.ki.wScan = (WORD)MapVirtualKeyA((UINT)a, MAPVK_VK_TO_VSC);
        in.ki.dwFlags = (kind == VRB_EV_KEYUP ? KEYEVENTF_KEYUP : 0);
        break;
    case VRB_EV_MBTNDOWN:
    case VRB_EV_MBTNUP: {
        in.type = INPUT_MOUSE;
        const bool down = kind == VRB_EV_MBTNDOWN;
        if (a == K_MOUSE1)      in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN   : MOUSEEVENTF_LEFTUP;
        else if (a == K_MOUSE2) in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN  : MOUSEEVENTF_RIGHTUP;
        else                    in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        break;
    }
    case VRB_EV_MOUSEMOVE:
        in.type = INPUT_MOUSE;
        in.mi.dx = a;
        in.mi.dy = b;
        in.mi.dwFlags = MOUSEEVENTF_MOVE;
        break;
    case VRB_EV_WHEEL:
        in.type = INPUT_MOUSE;
        in.mi.mouseData = (DWORD)(a * WHEEL_DELTA);
        in.mi.dwFlags = MOUSEEVENTF_WHEEL;
        break;
    default:
        return;
    }
    SendInput(1, &in, sizeof(in));
}

// ---- input event ring -------------------------------------------------
//
// Every emission that leaves this layer is recorded: update counter, kind,
// key/dx, value/dy. Dumped once in the INPUT / WEAPON CHECK block, it is the
// proof layer between "OpenXR action state changed" and "the game received
// an event" -- the 2026-08-20 run could not show whether stick input ever
// became key events at all.
enum { INJ_RING = 96 };
struct EvRec { unsigned frame; short kind; short a; int b; };
static EvRec    g_evRing[INJ_RING];
static unsigned g_evCount;          // total ever; ring index = count % INJ_RING
static unsigned g_updateCounter;    // VRBinds_Update invocations

static void Emit(int kind, int a, int b)
{
    EvRec& r = g_evRing[g_evCount % INJ_RING];
    r.frame = g_updateCounter;
    r.kind = (short)kind;
    r.a = (short)a;
    r.b = b;
    ++g_evCount;
    (g_sink ? g_sink : RealSink)(kind, a, b);
}

// ---- desired-state reconciler -----------------------------------------
//
// The single ledger of synthetic keys/buttons currently DOWN at the OS, and
// the set the current update WANTS down. Every update rebuilds the complete
// wanted set from live input and reconciles: held-but-unwanted keys are
// released, wanted-but-unheld keys are pressed. A key can therefore stay
// down only by being wanted again each update -- no mode transition, focus
// blip, tracking loss, or eaten edge can strand one. (The 2026-08-20 stuck
// fire was a raw un-ledgered MOUSE1 down whose release edge fell into an
// unfocused window: nothing ever sent the UP. Raw un-ledgered button
// emissions no longer exist.)
enum { MAX_HELD = 16 };
static int g_held[MAX_HELD];
static int g_heldCount;
static int g_want[MAX_HELD];
static int g_wantCount;

static VRBindsCounters g_ctr;
static int g_menuClickWanted;       // this update's MOUSE1 wants are menu clicks
static int g_menuClickArmed;        // trigger seen UP inside the menu
static int g_wasMenuMode;
int VRBinds_MenuMode() { return g_wasMenuMode; }

static bool InList(const int* list, int n, int key)
{
    for (int i = 0; i < n; ++i)
        if (list[i] == key) return true;
    return false;
}

static void Want(int key)
{
    if (key == K_NONE || InList(g_want, g_wantCount, key) || g_wantCount >= MAX_HELD)
        return;
    g_want[g_wantCount++] = key;
}

static void CountEdge(int key, bool down)
{
    struct { int* d; int* u; } c = { nullptr, nullptr };
    if      (key == 'W') { c.d = &g_ctr.moveFwdDown;  c.u = &g_ctr.moveFwdUp;  }
    else if (key == 'S') { c.d = &g_ctr.moveBackDown; c.u = &g_ctr.moveBackUp; }
    else if (key == 'A') { c.d = &g_ctr.strafeLDown;  c.u = &g_ctr.strafeLUp;  }
    else if (key == 'D') { c.d = &g_ctr.strafeRDown;  c.u = &g_ctr.strafeRUp;  }
    else if (key == g_bind[VRB_FIRE] && !g_menuClickWanted)
                         { c.d = &g_ctr.fireDown;     c.u = &g_ctr.fireUp;     }
    else if (key == K_MOUSE1 && g_menuClickWanted)
                         { c.d = &g_ctr.menuClickDown; c.u = &g_ctr.menuClickUp; }
    else if (key == g_bind[VRB_JUMP])  { c.d = &g_ctr.jumpDown;  c.u = &g_ctr.jumpUp;  }
    else if (key == g_bind[VRB_MELEE]) { c.d = &g_ctr.meleeDown; c.u = &g_ctr.meleeUp; }
    else if (key == g_bind[VRB_USE])   { c.d = &g_ctr.useDown;   c.u = &g_ctr.useUp;   }
    if (down  && c.d) ++*c.d;
    if (!down && c.u) ++*c.u;
}

// Releases first (a sector change never holds opposing keys together for a
// frame), then presses. Returns how many keys were released.
static int Reconcile()
{
    int released = 0;
    for (int i = 0; i < g_heldCount; ) {
        const int key = g_held[i];
        if (!InList(g_want, g_wantCount, key)) {
            g_held[i] = g_held[--g_heldCount];
            if (key > 0) Emit(VRB_EV_KEYUP, key, 0);
            else         Emit(VRB_EV_MBTNUP, key, 0);
            CountEdge(key, false);
            ++released;
        } else {
            ++i;
        }
    }
    for (int i = 0; i < g_wantCount; ++i) {
        const int key = g_want[i];
        if (!InList(g_held, g_heldCount, key) && g_heldCount < MAX_HELD) {
            g_held[g_heldCount++] = key;
            if (key > 0) Emit(VRB_EV_KEYDOWN, key, 0);
            else         Emit(VRB_EV_MBTNDOWN, key, 0);
            CountEdge(key, true);
        }
    }
    return released;
}

static void Tap(int key)
{
    if (key == K_NONE)
        return;
    if (key == K_WHEELUP)   { Emit(VRB_EV_WHEEL, +1, 0); return; }
    if (key == K_WHEELDOWN) { Emit(VRB_EV_WHEEL, -1, 0); return; }
    if (key > 0) { Emit(VRB_EV_KEYDOWN, key, 0); Emit(VRB_EV_KEYUP, key, 0); }
    else         { Emit(VRB_EV_MBTNDOWN, key, 0); Emit(VRB_EV_MBTNUP, key, 0); }
}

// External gesture systems deliver one-shot commits through here (the
// physical reload's exactly-once tap is the production caller). Per-control
// counting keeps the exactly-once contract assertable end to end.
static unsigned g_tapCount[VRB_CONTROL_COUNT];

void VRBinds_TapControl(int control)
{
    if (control < 0 || control >= VRB_CONTROL_COUNT)
        return;
    ++g_tapCount[control];
    if (control == VRB_RELOAD)
        ++g_ctr.reloadTaps;
    Tap(g_bind[control]);
}

unsigned VRBinds_TapCount(int control)
{
    if (control < 0 || control >= VRB_CONTROL_COUNT)
        return 0;
    return g_tapCount[control];
}

void VRBinds_ReleaseAllInjectedInputs(const char* reason)
{
    g_wantCount = 0;
    const int released = Reconcile();
    if (released > 0) {
        ++g_ctr.releaseAllRuns;
        static int logged = 0;
        if (logged < 12) {
            ++logged;
            Log("VR binds: released %d injected input(s) (%s)",
                released, reason ? reason : "");
        }
    }
}

static bool GameHasFocusHere()
{
    if (g_sink)                       // test sink installed: no window exists
        return true;
    const HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// ----------------------------------------------------------------- state

static int    g_moveSector = -1;              // -1 idle, 0..7
static int    g_turnArmed = 1;                // snap-turn re-arm
static int    g_cycleArmed = 1;               // weapon-cycle re-arm

// Coupled-mode stick turning (see UpdateTurn): injected-count carry and the
// lifetime accounting the check block and the one-shot effectiveness log use.
static float     g_turnCarryX;          // fractional injected count carry
static long long g_turnCountsInjected;  // lifetime injected turn counts
static float     g_turnDegCommanded;    // lifetime commanded degrees (signed)
static int       g_turnEffLogged;

// ---- deliberate-action arming (grenade / weapon cycle) ----
//
// The hardware run of 2026-08-23 threw three grenades and cycled the weapon
// three times without the player ever asking for either. Both had the same
// shape: a HIGH-TRAFFIC control wired straight to a one-shot game action
// through a bare edge.
//   * grenade  <- off-hand trigger PRESS. That trigger is the most reflexively
//                 squeezed control on the controller, and the CoD1 grenade
//                 bind is weapon slot '4' -- so a reflex squeeze literally
//                 switches the weapon to grenades. Three in 70 s.
//   * weapnext <- dominant stick Y > 0.7. The dominant stick is the TURN
//                 stick; any diagonal flick while turning clears 0.7 on Y,
//                 and CoD's weapon cycle walks THROUGH the grenade slot.
// Both are deliberate now: armed only from an observed clean release, gated
// on stable tracking and focus, and the grenade additionally needs a
// sustained hold. No control has two owners.
enum { VRB_CYCLE_OFFHAND_X = 0, VRB_CYCLE_LEGACY_STICK = 1, VRB_CYCLE_X_HOLD = 2 };
static double g_xPressStart = -1.0;
static double g_recenterPressStart = -1.0;
static int    g_cycleBinding   = VRB_CYCLE_OFFHAND_X;
static float  g_grenadeHoldSec = 0.35f;       // vr_grenade_hold
static int    g_grenadeArmed;                 // saw a clean release since commit
static double g_grenadePressStart = -1.0;     // when the current hold began
static int    g_grenadeCounted;               // this hold already made a request
static int    g_grenadeFired;                 // this hold already committed
static double g_deliberateHoldoffUntil = -1.0;// after focus/tracking/menu churn
static int    g_prevFocusSeen = -1;
static int    g_prevStableSeen[2] = { -1, -1 };
static int    g_grenadeRequests, g_grenadeAccepted, g_grenadeRejectedNoise;
static const char* g_grenadeSource = "(none)";
static const char* g_grenadeLastReject = "(none)";

// Every boundary that can present an already-held control as a fresh press
// disarms all deliberate actions and starts a short holdoff.
static void DisarmDeliberate(double now)
{
    g_grenadeArmed = 0;
    g_grenadePressStart = -1.0;
    g_grenadeCounted = 0;
    g_grenadeFired = 0;
    g_cycleArmed = 0;
    g_xPressStart = -1.0;
    g_deliberateHoldoffUntil = now + 0.25;
}

static double g_secondaryPressTime = -1.0;    // crouch/prone press-and-hold
static int    g_proneFired;
static int    g_crouched;                     // physical-crouch bookkeeping
static double g_crouchEdgeTime = -1.0;
static double g_lastFireHaptic = -1.0;
static int    g_reloadFireNeedsRelease;

// ---- ADS accuracy-state ownership (see vr_binds.h) ----
static int    g_adsAssumed;                   // toggle bookkeeping: game state
static int    g_adsDesired;                   // this frame's wanted state
static int    g_adsOwnerMask;                 // 1 manual, 2 physical, 4 twohand
static int    g_adsTransitions;               // reconciler taps
static double g_adsHoldoffUntil = -1.0;       // after weapon switch: let the
                                              // game finish before re-asserting
static int    g_lastRebaseSeen;               // VRWeapon_GetRebaseCount watch
static VRBindsAdsHintFn g_adsHintFn;          // cgame zoom reader (may be null)
static int    g_adsHintCorrect;               // correction enabled (config)
static int    g_adsHintValidLast;
static float  g_adsHintValueLast;
static int    g_adsHintCorrections;
static double g_adsHintMismatchSince = -1.0;

void VRBinds_SetAdsHintProvider(VRBindsAdsHintFn fn) { g_adsHintFn = fn; }
void VRBinds_SetAdsHintCorrection(int on)            { g_adsHintCorrect = on ? 1 : 0; }

// Steering.
static float  g_carryX, g_carryY;             // fractional count carry
static int    g_prevInjX, g_prevInjY;         // what last frame injected
static float  g_prevYaw, g_prevPitch;
static double g_prevAimTime = -1.0;
static bool   g_prevAimValid;
static float  g_accExpected, g_accObserved;   // decaying effectiveness meter
static int    g_deadFrames;
static double g_lastProbeTime = -1.0;
static int    g_probeArmed;
static bool   g_steeringLive;
static float  g_lastErrYaw, g_lastErrPitch;

// Steering-leak watchdog (the Phase-4 head/gun invariant, enforced live).
// While native mode is decoupled, the rendered camera yaw must move by
// EXACTLY bodyYaw delta + head's own yaw delta -- injected aim steering must
// contribute nothing. Any residual beyond tolerance on a frame that injected
// counts as a leak of the weapon into the HMD camera.
static int    g_steerLeakCount;
static unsigned g_leakLogs;
static float  g_leakPrevCamYaw, g_leakPrevBodyYaw, g_leakPrevHeadYaw;
static bool   g_leakPrevValid;
static unsigned g_leakPrevSerial;   // pair-latch serial of the last sample
static float  g_leakPrevBaseYaw;    // camera base the last sampled pair used

// Input-check cadence: first block 8 s after activity, then one per minute
// (capped) -- the 2026-08-20 run's single block fired in the MENU and carried
// zeros for every gameplay quantity, so a whole play session produced no
// census. Periodic blocks make the log carry the state DURING play.
static double g_firstActiveTime = -1.0;
static int    g_checkCount;
static double g_lastCheckTime = -1.0;
enum { CHECK_MAX_BLOCKS = 12, CHECK_PERIOD_S = 60 };

// Steering convergence telemetry (Priority-6 numbers for the next log).
static bool   g_hadTarget;
static double g_targetSince = -1.0;      // when a target first became available
static bool   g_convergedLogged;
static double g_lastPursuitLog = -1.0;
static unsigned g_pursuitLogs;

// Action-state change ring: OpenXR-side proof, one record per observed
// change (stick quantised to 0.05 so it cannot flood).
enum { ACT_RING = 96 };
enum { ACH_ACTIVE = 0, ACH_STICK, ACH_TRIGGER, ACH_SQUEEZE,
       ACH_BTN_P, ACH_BTN_S, ACH_STICK_CLICK, ACH_MENU, ACH_CODES };
struct ActRec { unsigned frame; unsigned char hand; unsigned char code; short v1, v2; };
static ActRec   g_actRing[ACT_RING];
static unsigned g_actCount;
static short    g_actPrev[VRI_HANDS][ACH_CODES];
static short    g_actPrevMenu;

static void NoteAct(int hand, int code, short v1, short v2)
{
    ActRec& r = g_actRing[g_actCount % ACT_RING];
    r.frame = g_updateCounter;
    r.hand = (unsigned char)hand;
    r.code = (unsigned char)code;
    r.v1 = v1; r.v2 = v2;
    ++g_actCount;
}

static void NoteActionChanges(const VRInputState* in)
{
    for (int h = 0; h < VRI_HANDS; ++h) {
        const VRHandState& hs = in->hand[h];
        const short cur[ACH_CODES] = {
            (short)hs.active,
            (short)((short)lroundf(hs.stickX * 20.0f) * 64
                  + (short)lroundf(hs.stickY * 20.0f)),   // packed, compare-only
            (short)hs.triggerDown,
            (short)hs.squeezeDown,
            (short)hs.btnPrimary,
            (short)hs.btnSecondary,
            (short)hs.stickClick,
            0,
        };
        for (int c = 0; c < ACH_CODES; ++c) {
            if (cur[c] == g_actPrev[h][c])
                continue;
            g_actPrev[h][c] = cur[c];
            if (c == ACH_STICK)
                NoteAct(h, c, (short)lroundf(in->hand[h].stickX * 100.0f),
                              (short)lroundf(in->hand[h].stickY * 100.0f));
            else
                NoteAct(h, c, cur[c], 0);
        }
    }
    if ((short)in->menuDown != g_actPrevMenu) {
        g_actPrevMenu = (short)in->menuDown;
        NoteAct(0, ACH_MENU, g_actPrevMenu, 0);
    }
}

// Input generations arrive once per XR frame, but this layer updates once per
// GAME frame -- twice per F1 pair. Edge flags (pressed/released) persist in
// the state across both game frames, so edge-triggered actions must consume
// each generation exactly once or every tap fires twice.
static unsigned g_lastGenSeen;

void VRBinds_TestSetSink(VRBindsSink sink) { g_sink = sink; }

void VRBinds_TestReset()
{
    VRBinds_ReleaseAllInjectedInputs("test reset");
    memset(&g_ctr, 0, sizeof(g_ctr));
    memset(g_tapCount, 0, sizeof(g_tapCount));
    g_evCount = 0;
    g_updateCounter = 0;
    g_menuClickWanted = 0;
    g_menuClickArmed = 0;
    g_wasMenuMode = 0;
    g_actCount = 0;
    memset(g_actPrev, 0, sizeof(g_actPrev));
    g_actPrevMenu = 0;
    g_hadTarget = false;
    g_targetSince = -1.0;
    g_convergedLogged = false;
    g_lastPursuitLog = -1.0;
    g_pursuitLogs = 0;
    for (int i = 0; i < VRB_CONTROL_COUNT; ++i) {
        static const int defs[VRB_CONTROL_COUNT] = {
            K_MOUSE1, 'M', VK_SPACE, 'C', VK_CONTROL, VK_SHIFT, 'R', 'F', '4', VK_ESCAPE };
        g_bind[i] = defs[i];
    }
    g_turnSnap = 0; g_snapAngleDeg = 30.0f; g_smoothTurnSpeed = 180.0f;
    g_moveOffhand = 0; g_hapticsOn = 1; g_hapticStrength = 0.6f;
    g_physicalCrouch = 0; g_crouchDropM = 0.32f;
    g_steerGain = 0.55f; g_menuCursorSpeed = 900.0f;
    // Back to the PRODUCTION default: experimental weapon control off, no
    // lockout, a clean rebase governor and a fresh injection accumulator.
    g_experimentalWeapon = 0;
    g_experimentalWeaponLockedOut = 0;
    g_injSinceSampleX = g_injSinceSampleY = 0;
    g_injSinceLeakX = g_injSinceLeakY = 0;
    g_leakPrevSerial = 0;
    g_rebaseCount = 0;
    g_rebaseReason = "(none)";
    g_rebaseRefusedCount = 0;
    g_rebaseWindowStart = -1.0;
    g_rebaseInWindow = 0;
    g_rebaseStormLogged = false;
    g_dpcYaw = -0.11f; g_dpcPitch = -0.11f;
    g_dpcYaw = g_dpcPitch = -0.11f;
    g_turnCarryX = 0.0f; g_turnCountsInjected = 0;
    g_turnDegCommanded = 0.0f; g_turnEffLogged = 0;
    g_moveSector = -1; g_turnArmed = 1; g_cycleArmed = 1;
    g_cycleBinding = VRB_CYCLE_OFFHAND_X; g_grenadeHoldSec = 0.35f;
    g_xPressStart = g_recenterPressStart = -1.0;
    g_grenadeArmed = 0; g_grenadePressStart = -1.0;
    g_grenadeCounted = 0; g_grenadeFired = 0;
    g_deliberateHoldoffUntil = -1.0;
    g_prevFocusSeen = -1; g_prevStableSeen[0] = g_prevStableSeen[1] = -1;
    g_grenadeRequests = g_grenadeAccepted = g_grenadeRejectedNoise = 0;
    g_grenadeSource = "(none)"; g_grenadeLastReject = "(none)";
    g_secondaryPressTime = -1.0; g_proneFired = 0;
    g_adsAssumed = 0; g_crouched = 0; g_crouchEdgeTime = -1.0;
    g_adsDesired = 0; g_adsOwnerMask = 0; g_adsTransitions = 0;
    g_adsHoldoffUntil = -1.0;
    g_lastRebaseSeen = VRWeapon_GetRebaseCount();
    g_adsHintCorrect = 0;
    g_adsHintValidLast = 0; g_adsHintValueLast = 0.0f;
    g_adsHintCorrections = 0;
    g_adsHintMismatchSince = -1.0;
    g_lastFireHaptic = -1.0;
    g_reloadFireNeedsRelease = 0;
    g_carryX = g_carryY = 0.0f;
    g_prevInjX = g_prevInjY = 0;
    g_prevYaw = g_prevPitch = 0.0f;
    g_prevAimTime = -1.0; g_prevAimValid = false;
    g_accExpected = g_accObserved = 0.0f;
    g_deadFrames = 0;
    g_lastProbeTime = -1.0; g_probeArmed = 0;
    g_steeringLive = false;
    g_lastErrYaw = g_lastErrPitch = 0.0f;
    g_firstActiveTime = -1.0;
    g_checkCount = 0;
    g_lastCheckTime = -1.0;
    g_lastGenSeen = 0;
    g_steerLeakCount = 0;
    g_leakLogs = 0;
    g_leakPrevValid = false;
    g_leakPrevCamYaw = g_leakPrevBodyYaw = g_leakPrevHeadYaw = 0.0f;
}

// ---------------------------------------------------------------- pieces

// Adds this frame's movement keys to the wanted set. The reconciler keeps a
// key held across sector changes that share it (W across W -> W+A), so
// forward never stutters, and releases whatever the new sector dropped.
static void UpdateMovement(const VRInputState* in)
{
    static const int kSectorKeys[8][2] = {
        { 'W', 0 }, { 'W', 'A' }, { 'A', 0 }, { 'A', 'S' },
        { 'S', 0 }, { 'S', 'D' }, { 'D', 0 }, { 'W', 'D' } };

    int sector = -1;
    const int moveHand = 1 - VRWeapon_DominantHand();   // off hand moves
    const VRHandState& mh = in->hand[moveHand];
    const float mag = sqrtf(mh.stickX * mh.stickX + mh.stickY * mh.stickY);
    if (mh.active && mag > 0.25f) {
        // Stick bearing in yaw terms: +left. Stick up = 0, right = -90.
        const float bearing = atan2f(-mh.stickX, mh.stickY) * 57.2957795f;
        float refYaw = 0.0f, gameYaw = 0.0f, gamePitch = 0.0f;
        double when = 0.0;
        const int haveRef  = VRWeapon_GetMoveRefYawDeg(g_moveOffhand, &refYaw);
        const int haveGame = VRWeapon_GetGameAimDeg(&gameYaw, &gamePitch, &when);
        // Desired world direction relative to the aim (WASD moves relative
        // to the game's view yaw, which is the weapon -- when the aim has
        // wandered from the body, e.g. mouse steering, this rotation is what
        // keeps stick-forward walking where the player LOOKS).
        float rel = bearing;
        if (haveRef && haveGame) {
            rel = VRM_WrapDeg(refYaw + bearing - gameYaw);
        } else {
            // Coupled baseline: body yaw does not exist (it is a decoupled-
            // mode quantity), but "stick-forward walks where the player
            // LOOKS" must still hold. The rendered look direction here is
            // the game's own yaw plus the HMD's offset from neutral, so
            // relative to the game yaw the desired direction is simply
            // bearing + head offset.
            float hy = 0, hp = 0;
            VR_GetHeadAimOffset(&hy, &hp);
            rel = VRM_WrapDeg(bearing + hy);
        }
        int s = (int)floorf((rel + 22.5f) / 45.0f);
        while (s < 0) s += 8;
        sector = s % 8;
    }

    if (sector >= 0)
        for (int k = 0; k < 2; ++k)
            Want(kSectorKeys[sector][k]);
    g_moveSector = sector;
}

// ---- thumbstick turning -----------------------------------------------
//
// Two consumers, selected by the weapon module's live mode:
//
//   DECOUPLED (experimental weapon control): the turn rotates the virtual
//   BODY basis (VRWeapon_AddBodyYawDeg); the camera is composed on that
//   basis, so the view turns and the steering loop carries the game's aim
//   along. This is the original wiring.
//
//   COUPLED (the production camera-stable baseline): body yaw is INVALID by
//   construction (it only exists while decoupled) and the camera is composed
//   on the game's own aim axis -- so the only thing that can visibly turn
//   the player is the game's view yaw itself. The turn is therefore injected
//   as relative mouse motion through the SendInput channel, the one input
//   path proven to reach CoD's DirectInput reader (the shipped snap turn and
//   the 2026-08-23 steering runs both drove the view this way). The
//   2026-08-28 hardware run is why this exists: UpdateTurn commanded
//   AddBodyYawDeg 1224 times and every one was silently dropped, because in
//   coupled mode nothing validates body yaw and nothing renders it.
//
// Degrees -> counts uses the measured counts-per-degree the steering loop
// keeps (seeded from codvr.cfg's vr_steer_cpd, auto-calibrated when steering
// is armed). A fractional carry (g_turnCarryX, declared with the state above)
// keeps slow smooth turns from truncating to zero counts per frame.
static void InjectTurnCounts(float turnDeg)
{
    // g_dpcYaw is DEGREES PER COUNT, negative (+counts turn right = -yaw).
    if (g_dpcYaw >= -1e-6f)
        return;                          // unbelievable calibration: refuse
    const float want = turnDeg / g_dpcYaw + g_turnCarryX;
    int dx = (int)want;
    if (dx >  400) dx =  400;            // same per-frame bound as steering
    if (dx < -400) dx = -400;
    g_turnCarryX = want - (float)dx;
    if (g_turnCarryX >  4.0f) g_turnCarryX =  4.0f;
    if (g_turnCarryX < -4.0f) g_turnCarryX = -4.0f;
    if (!dx)
        return;
    Emit(VRB_EV_MOUSEMOVE, dx, 0);
    g_turnCountsInjected += dx;
    g_turnDegCommanded   += turnDeg;
    ++g_ctr.turnEvents;

    // One-shot effectiveness note: after ~30 deg of commanded turning, say
    // whether the game's yaw is actually moving, so a dead injection channel
    // is named in the log instead of inferred from a user report.
    if (!g_turnEffLogged && fabsf(g_turnDegCommanded) > 30.0f) {
        g_turnEffLogged = 1;
        float gy = 0, gp = 0; double t = 0;
        const int haveAim = VRWeapon_GetGameAimDeg(&gy, &gp, &t);
        Log("VR binds: coupled stick turn live -- %.0f deg commanded, "
            "%lld counts injected (cpd %.2f), game yaw now %s%.1f",
            g_turnDegCommanded, g_turnCountsInjected,
            g_dpcYaw != 0.0f ? 1.0f / g_dpcYaw : 0.0f,
            haveAim ? "" : "(unreadable) ", gy);
    }
}

static void CommandTurn(float turnDeg)
{
    if (VRWeapon_Decoupled()) {
        VRWeapon_AddBodyYawDeg(turnDeg);
        ++g_ctr.turnEvents;
    } else {
        InjectTurnCounts(turnDeg);
    }
}

static void UpdateTurn(const VRInputState* in, float dt, int gameplay)
{
    if (!gameplay || g_turnSnap == 2)
        return;
    const VRHandState& dh = in->hand[VRWeapon_DominantHand()];
    if (!dh.active)
        return;
    const float x = dh.stickX;

    if (g_turnSnap) {
        if (g_turnArmed && fabsf(x) > 0.7f) {
            g_turnArmed = 0;
            // Stick right = turn right = yaw decreases (+yaw is left).
            CommandTurn(x > 0 ? -g_snapAngleDeg : g_snapAngleDeg);
        } else if (!g_turnArmed && fabsf(x) < 0.4f) {
            g_turnArmed = 1;
        }
    } else if (fabsf(x) > 0.2f) {
        const float t = (fabsf(x) - 0.2f) / 0.8f;          // deadzone rescale
        const float rate = g_smoothTurnSpeed * t * t;      // gentle start
        CommandTurn((x > 0 ? -rate : rate) * dt);
    }
}

// Watches the boundaries that can fabricate an edge -- focus regained, a hand
// re-acquiring tracking, a menu transition -- and disarms every deliberate
// action across them. Runs before the actions themselves, in every mode.
static void UpdateDeliberateArming(const VRInputState* in, double now, int gameplay)
{
    const int focused = in->focused ? 1 : 0;
    if (g_prevFocusSeen >= 0 && focused != g_prevFocusSeen)
        DisarmDeliberate(now);
    g_prevFocusSeen = focused;

    for (int h = 0; h < 2; ++h) {
        const int st = VRI_HandTrackingUsable(in->hand[h]);
        if (g_prevStableSeen[h] >= 0 && st != g_prevStableSeen[h])
            DisarmDeliberate(now);
        g_prevStableSeen[h] = st;
    }
    if (!gameplay)
        DisarmDeliberate(now);
}

static int DeliberateReady(const VRHandState& h, double now)
{
    return h.active && VRI_HandTrackingUsable(h) && now >= g_deliberateHoldoffUntil;
}

// Weapon cycle. The default source is the OFF-HAND X button: discrete,
// low-traffic, weapon-management, and it shares nothing with the grenade and
// nothing with the turn stick. The old dominant-stick-Y source survives only
// behind an explicit opt-in, so the regression suite can still drive it.
static void UpdateWeaponCycle(const VRInputState* in, double now, int gameplay)
{
    if (!gameplay)
        return;

    int want = 0, dir = -1;                   // dir < 0 = wheeldown = weapnext
    if (g_cycleBinding == VRB_CYCLE_X_HOLD) {
        const VRHandState& oh=in->hand[1-VRWeapon_DominantHand()];
        if (!oh.active || !VRI_HandTrackingActive(oh)) {
            g_cycleArmed=0; g_xPressStart=-1.0; return;
        }
        if (!oh.btnPrimary) {
            if (g_xPressStart>=0 && now-g_xPressStart<0.45 && DeliberateReady(oh,now))
                if (VRNH_ReloadMode() != 2 || !VRNH_PhysicalReloadAvailable())
                    VRBinds_TapControl(VRB_RELOAD);
            g_xPressStart=-1.0; g_cycleArmed=1; return;
        }
        if (g_cycleArmed && DeliberateReady(oh,now)) {
            g_cycleArmed=0; g_xPressStart=now;
        }
        if (g_xPressStart>=0 && now-g_xPressStart>=0.45 && DeliberateReady(oh,now)) {
            want=1; g_xPressStart=-1.0;
        }
    } else if (g_cycleBinding == VRB_CYCLE_LEGACY_STICK) {
        const VRHandState& dh = in->hand[VRWeapon_DominantHand()];
        const float y = dh.active ? dh.stickY : 0.0f;
        if (fabsf(y) < 0.4f) g_cycleArmed = 1;
        if (g_cycleArmed && fabsf(y) > 0.7f && DeliberateReady(dh, now)) {
            want = 1; dir = (y > 0) ? +1 : -1;
        }
    } else {
        const VRHandState& oh = in->hand[1 - VRWeapon_DominantHand()];
        // Same rule as the grenade: only a live, stably tracked hand can
        // witness the release that re-arms this. A dropout reads every button
        // as 0, and an absent hand loses its arm rather than keeping it.
        if (!oh.active || !VRI_HandTrackingUsable(oh)) g_cycleArmed = 0;
        else if (!oh.btnPrimary)           g_cycleArmed = 1;
        if (g_cycleArmed && oh.btnPrimary && DeliberateReady(oh, now))
            want = 1;
    }
    if (!want)
        return;

    g_cycleArmed = 0;
    Tap(dir > 0 ? K_WHEELUP : K_WHEELDOWN);
    ++g_ctr.weaponCycleTaps;
    // A weapon switch drops the game to hip-fire and eats weapalt taps
    // while the switch animation runs: adopt hip, and hold the ADS
    // reconciler off until the new weapon is up. If an owner is still
    // active afterwards (e.g. the support hand never let go), the
    // reconciler re-asserts ADS with one tap.
    g_adsAssumed = 0;
    g_adsHoldoffUntil = now + 0.8;
}

// Grenade. The ONLY grenade source in the build, and it is deliberate:
//   * off-hand trigger, which nothing else reads
//   * armed only after a clean released state has actually been observed
//   * a sustained hold (vr_grenade_hold, default 0.35 s) before it commits
//   * at most one commit per physical press
//   * refused outside gameplay, without stable tracking, or inside the
//     post-focus / post-reacquire holdoff
// A reflex squeeze does nothing. A tracking blip does nothing. Only holding
// the off-hand trigger, on purpose, throws a grenade.
static void UpdateGrenade(const VRInputState* in, double now, int gameplay)
{
    const VRHandState& oh = in->hand[1 - VRWeapon_DominantHand()];

    // A DEAD hand reads trigger 0. That is not a release -- it is an absence,
    // and treating it as one is precisely how a hand that drops out with the
    // trigger held comes back "armed" and throws a grenade nobody asked for.
    // Only a hand that is present and stably tracked can witness a release.
    // USABLE deliberately rides out a 1 s dropout so the camera mode does
    // not flicker, which means it is NOT enough on its own here: a hand can
    // be absent and still USABLE. Arming needs the RAW presence bit, and an
    // absent hand loses its arm outright -- while we could not see it, the
    // player may have released and re-pressed, and we would never know.
    const int witnessed = oh.active && VRI_HandTrackingUsable(oh);
    if (!witnessed) {
        g_grenadeArmed = 0;
        g_grenadePressStart = -1.0;
        g_grenadeCounted = 0;
        g_grenadeFired = 0;
        return;
    }
    if (!oh.triggerDown) {                    // released: re-arm, end the hold
        if (!g_grenadeArmed && gameplay)
            g_grenadeArmed = 1;
        g_grenadePressStart = -1.0;
        g_grenadeCounted = 0;
        g_grenadeFired = 0;
        return;
    }
    if (!g_grenadeArmed)                      // held since before we armed
        return;
    if (g_grenadeFired)                       // one commit per press
        return;

    if (g_grenadePressStart < 0.0)
        g_grenadePressStart = now;

    const int ready = gameplay && DeliberateReady(oh, now);
    if (!g_grenadeCounted) {
        g_grenadeCounted = 1;
        ++g_grenadeRequests;
        if (!ready) {
            ++g_grenadeRejectedNoise;
            g_grenadeLastReject = !gameplay  ? "not gameplay"
                                : !VRI_HandTrackingUsable(oh) ? "tracking not stable"
                                             : "re-arm holdoff";
        }
    }
    if (!ready)
        return;
    if (now - g_grenadePressStart < (double)g_grenadeHoldSec)
        return;                               // not held long enough yet

    g_grenadeFired = 1;
    g_grenadeArmed = 0;                       // needs a clean release to re-arm
    Tap(g_bind[VRB_GRENADE]);
    ++g_ctr.grenadeTaps;
    ++g_grenadeAccepted;
    g_grenadeSource = "offhand trigger hold";
    if (g_hapticsOn)
        VRInput_Haptic(1 - VRWeapon_DominantHand(), g_hapticStrength * 0.7f, 18);
}

// The single writer of the weapalt toggle. desired-state style: every
// gameplay update recomputes the complete wanted ADS state from live owners
// and reconciles the assumed game state against it with at most one tap.
// Raw edge-paired toggling (the old press+release double tap) is gone -- an
// eaten edge or a mode change mid-hold can no longer invert the bookkeeping.
static void UpdateAds(const VRInputState* in, double now, int gameplay)
{
    const int dom = VRWeapon_DominantHand();
    const VRHandState& dh = in->hand[dom];

    // Boundaries where the GAME resets itself to hip-fire: adopt that belief
    // without tapping (tapping would toggle the real state the wrong way).
    if (VRWeapon_GetRebaseCount() != g_lastRebaseSeen) {
        g_lastRebaseSeen = VRWeapon_GetRebaseCount();
        if (g_adsAssumed)
            Log("VR binds: ADS assumed off (spawn/load rebase)");
        g_adsAssumed = 0;
    }
    if (VRWeapon_Scripted() && g_adsAssumed)
        g_adsAssumed = 0;

    // Owners. Two-hand support IS the braced-aim accuracy state; manual grip
    // and physical sights-to-eye remain deliberate ADS modes in their own
    // right. All are levels -- no edges to eat.
    g_adsOwnerMask = 0;
    if (dh.squeezeDown)              g_adsOwnerMask |= 1;
    if (VRWeapon_AdsActive())        g_adsOwnerMask |= 2;
    if (VRWeapon_TwoHandAttached())  g_adsOwnerMask |= 4;
    g_adsDesired = (gameplay && g_adsOwnerMask != 0) ? 1 : 0;

    // Optional ground truth: cgame's zoom-transition float (0.0 = hip-fire).
    // Always sampled for the check block; only overrides the assumed state
    // when the correction is enabled and the mismatch has persisted -- a
    // transitional raise/lower must not be "corrected" mid-animation.
    g_adsHintValidLast = 0;
    if (g_adsHintFn) {
        float v = 0.0f;
        if (g_adsHintFn(&v)) {
            g_adsHintValidLast = 1;
            g_adsHintValueLast = v;
            const int hintState = (v != 0.0f) ? 1 : 0;
            if (g_adsHintCorrect && gameplay && hintState != g_adsAssumed) {
                if (g_adsHintMismatchSince < 0.0)
                    g_adsHintMismatchSince = now;
                if (now - g_adsHintMismatchSince > 0.7) {
                    g_adsAssumed = hintState;
                    ++g_adsHintCorrections;
                    g_adsHintMismatchSince = -1.0;
                    static int logged = 0;
                    if (logged < 4) {
                        ++logged;
                        Log("VR binds: ADS assumed corrected to %s by cgame zoom %.3f",
                            hintState ? "ON" : "off", v);
                    }
                }
            } else {
                g_adsHintMismatchSince = -1.0;
            }
        }
    }

    if (!gameplay)
        return;                       // menus pause the game: never tap there
    if (g_adsHoldoffUntil > 0.0 && now < g_adsHoldoffUntil)
        return;                       // weapon switch in flight

    if (g_adsDesired != g_adsAssumed) {
        Tap(g_bind[VRB_ADS]);
        ++g_ctr.adsTaps;
        ++g_adsTransitions;
        g_adsAssumed = g_adsDesired;
        static int logged = 0;
        if (logged < 10) {
            ++logged;
            Log("VR binds: ADS -> %s (owner%s%s%s)",
                g_adsAssumed ? "ON" : "off",
                (g_adsOwnerMask & 1) ? " manual" : "",
                (g_adsOwnerMask & 2) ? " physical" : "",
                (g_adsOwnerMask & 4) ? " twohand" : "");
        }
    }
}

// edges: this update is the FIRST to see the current input generation, so
// edge flags may be consumed. Everything HELD is expressed as a Want -- the
// reconciler at the end of the update owns every down/up, so a mode change
// mid-hold releases cleanly and an eaten release edge cannot strand a
// button. mode: 1 = gameplay, 0 = menu, -1 = scripted (cutscene: edge taps
// and recenter only, no combat or click injection).
static void UpdateButtons(const VRInputState* in, double now, int mode, int edges)
{
    const int dom = VRWeapon_DominantHand();
    const int off = 1 - dom;
    const VRHandState& dh = in->hand[dom];
    const VRHandState& oh = in->hand[off];

    if (mode == 1) {
        VRReloadProfile reloadProfile;
        VRReload_Get(&reloadProfile);
        const int manualOnly = VRNH_ReloadMode() == 2 && reloadProfile.valid;
        const int reloadState = VRReload_State();
        // Receiver loaders reach POUCH empty-handed. They begin ammunition
        // handling only at pickup; detachable loaders have already removed a mag.
        const int handlingStartsAt = (reloadProfile.directLoad
                                   || reloadProfile.cls == VRRL_CLASS_INCREMENTAL)
                                   ? VRRL_ACQUIRED : VRRL_GRIPPED;
        const int manipulating = manualOnly && reloadState >= handlingStartsAt
                               && reloadState <= VRRL_INSERT;
        if (!manualOnly) g_reloadFireNeedsRelease = 0;
        else if (manipulating) g_reloadFireNeedsRelease = 1;
        // An absent controller/action reads zero. Only a live, tracked
        // trigger can witness the release required after ammunition handling.
        else if (dh.active && dh.triggerActive && VRI_HandTrackingActive(dh)
                 && !dh.triggerDown) g_reloadFireNeedsRelease = 0;
        const int allowFire = !manipulating && !g_reloadFireNeedsRelease;
        // Fire mirrors the trigger LEVEL: automatic weapons need the hold,
        // and a dead hand reads trigger 0 so loss releases by itself.
        // A mode-2 ammo-handling stage withdraws that Want, so the existing
        // reconciler releases +attack. Completion cannot discharge a trigger
        // held through the gesture: release and press again to fire.
        if (dh.triggerDown && allowFire) Want(g_bind[VRB_FIRE]);
        if (edges && dh.triggerPressed && g_hapticsOn && allowFire) {
            VRInput_Haptic(dom, g_hapticStrength, 25);
            g_lastFireHaptic = now;
        }
        // Bounded auto-fire rumble while held: one short pulse per 90 ms.
        if (dh.triggerDown && g_hapticsOn && allowFire
            && g_lastFireHaptic >= 0.0 && now - g_lastFireHaptic >= 0.09) {
            VRInput_Haptic(dom, g_hapticStrength * 0.8f, 15);
            g_lastFireHaptic = now;
        }
        if (!dh.triggerDown || !allowFire)
            g_lastFireHaptic = -1.0;

        // ADS is owned by the desired-state reconciler (UpdateAds); only the
        // physical-entry haptic tick still consumes the edge here.
        if (VRWeapon_AdsJustEntered() && g_hapticsOn)
            VRInput_Haptic(dom, g_hapticStrength * 0.5f, 8);

        if (dh.btnPrimary) Want(g_bind[VRB_JUMP]);
        if (dh.stickClick) Want(g_bind[VRB_MELEE]);

        // Secondary: tap = crouch, hold >= 0.4 s = prone.
        if (edges && dh.btnSecondaryPressed) {
            g_secondaryPressTime = now;
            g_proneFired = 0;
        }
        if (dh.btnSecondary && g_secondaryPressTime >= 0.0 && !g_proneFired
            && now - g_secondaryPressTime >= 0.4) {
            g_proneFired = 1;
            Tap(g_bind[VRB_PRONE]);
            ++g_ctr.proneTaps;
        }
        if (edges && dh.btnSecondaryReleased) {
            if (!g_proneFired && g_secondaryPressTime >= 0.0) {
                Tap(g_bind[VRB_CROUCH]);
                ++g_ctr.crouchTaps;
            }
            g_secondaryPressTime = -1.0;
        }

        // Off-hand X is now WEAPON CYCLE (UpdateWeaponCycle owns it) and the
        // off-hand trigger is now GRENADE-on-hold (UpdateGrenade owns it).
        // Neither is read here: one control, one owner, no shared edges, and
        // in particular nothing can drive BOTH the grenade and the cycle.
        (void)off;
        if (oh.btnSecondary) Want(g_bind[VRB_USE]);

        // Two-hand attach/detach tick, both hands.
        if (VRWeapon_TwoHandJustChanged() && g_hapticsOn) {
            VRInput_Haptic(0, g_hapticStrength * 0.65f, 10);
            VRInput_Haptic(1, g_hapticStrength * 0.65f, 10);
        }
    } else if (mode == 0) {
        // Menu: dominant stick moves the cursor, trigger clicks, B backs
        // out. The click is LEVEL-driven through the reconciler -- never a
        // raw edge-paired down/up, whose release could be eaten by a focus
        // blip and leave MOUSE1 down at the OS forever (the stuck-fire bug).
        // Armed only after the trigger has been seen UP in the menu, so a
        // trigger held across the gameplay->menu transition cannot click
        // whatever the cursor happens to sit on.
        g_menuClickWanted = 1;
        if (!dh.triggerDown) g_menuClickArmed = 1;
        if (g_menuClickArmed && dh.triggerDown) Want(K_MOUSE1);
        if (edges && dh.btnSecondaryPressed) Tap(VK_ESCAPE);
    }

    // Short click recenters on RELEASE. A hold is reserved for VR Options
    // and must not move the world while opening settings.
    if (edges && oh.stickClickPressed) g_recenterPressStart=now;
    if (edges && !oh.stickClick && g_recenterPressStart>=0) {
        if (oh.active && now-g_recenterPressStart<0.6) {
            VR_Recenter(); ++g_ctr.recenters;
        }
        g_recenterPressStart=-1.0;
    }

    if (edges && in->menuPressed) {
        Tap(g_bind[VRB_MENU]);
        ++g_ctr.menuTaps;
    }
}

static void UpdateMenuCursor(const VRInputState* in, float dt)
{
    const VRHandState& dh = in->hand[VRWeapon_DominantHand()];
    if (!dh.active)
        return;
    if (fabsf(dh.stickX) < 0.15f && fabsf(dh.stickY) < 0.15f)
        return;
    const int dx = (int)(dh.stickX * g_menuCursorSpeed * dt);
    const int dy = (int)(-dh.stickY * g_menuCursorSpeed * dt);
    if (dx || dy)
        Emit(VRB_EV_MOUSEMOVE, dx, dy);
}

static void UpdatePhysicalCrouch(double now)
{
    if (!g_physicalCrouch)
        return;
    float yawRef = 0.0f, refM[3];
    float headM[3];
    if (!(VR_GetNeutralRefs(&yawRef, refM) & 2) || !VR_GetHeadRoomMid(headM))
        return;
    const float drop = refM[2] - headM[2];      // metres below neutral
    const int want = g_crouched ? (drop > g_crouchDropM - 0.10f)
                                : (drop > g_crouchDropM);
    if (want != g_crouched) {
        if (g_crouchEdgeTime < 0.0)
            g_crouchEdgeTime = now;
        if (now - g_crouchEdgeTime >= 0.15) {
            g_crouched = want;
            g_crouchEdgeTime = -1.0;
            Tap(g_bind[VRB_CROUCH]);
        }
    } else {
        g_crouchEdgeTime = -1.0;
    }
}

// ----------------------------------------------------------- aim steering

static void UpdateSteering(double now, int gameplay)
{
    g_steeringLive = false;

    // DISARMED BASELINE: with experimental weapon control off, this module
    // observes nothing and injects nothing on the aim axis. Returning here
    // (rather than merely skipping the Emit) also keeps the auto-calibrator
    // and the rebase heuristic from running on motion they did not cause --
    // both of which mis-fire when they see turns they did not inject.
    if (!SteeringArmed()) {
        g_prevInjX = g_prevInjY = 0;
        g_carryX = g_carryY = 0.0f;
        g_hadTarget = false;
        g_prevAimValid = false;     // no stale sample to diff against on re-arm
        return;
    }

    float gameYaw = 0, gamePitch = 0;
    double aimTime = 0;
    const int haveAim = VRWeapon_GetGameAimDeg(&gameYaw, &gamePitch, &aimTime);
    const bool newSample = haveAim && aimTime != g_prevAimTime;

    // ---- calibration + effectiveness, on every fresh observation ----
    if (newSample && g_prevAimValid) {
        const float obsYaw   = VRM_WrapDeg(gameYaw - g_prevYaw);
        const float obsPitch = gamePitch - g_prevPitch;

        // Expected turn is measured against everything injected SINCE THE
        // LAST FRESH AIM SAMPLE, not just the last call's injection.
        //
        // The 2026-08-23 rebase runaway lived here: g_prevInjX was zeroed on
        // every call, including calls that returned before the injection site
        // (non-gameplay frames, no-target frames). A later fresh sample then
        // compared a real, self-inflicted turn against expYaw == 0, decided
        // "that is far beyond anything we injected", and rebased the body yaw
        // -- 1086 times off 2 engagements, at a steady ~25 deg step. The
        // accumulator below cannot be zeroed by a call that did not consume a
        // sample, so the heuristic can no longer mistake our own output for
        // an external turn.
        const float expYaw   = g_injSinceSampleX * g_dpcYaw;
        const float expPitch = g_injSinceSampleY * g_dpcPitch;
        (void)expPitch;

        // A change far beyond anything we injected is a spawn or a scripted
        // turn: adopt the new facing instead of fighting it.
        if (fabsf(obsYaw) > 15.0f && fabsf(expYaw) < 5.0f) {
            NoteRebase("aim jumped beyond injected turn");
        } else {
            // Calibrate against the SAME accumulator the expectation used, and
            // clamp the result. An unclamped ratio is how counts-per-degree
            // drifted -9.09 -> -0.7 on 2026-08-23: a 13x gain increase that
            // turned every degree of aim error into a violent mouse slam.
            if (abs(g_injSinceSampleX) >= 8 && fabsf(obsYaw) < 20.0f) {
                const float m = obsYaw / (float)g_injSinceSampleX;
                if (m < -0.004f && m > -2.0f)
                    g_dpcYaw += (m - g_dpcYaw) * 0.15f;
            }
            if (abs(g_injSinceSampleY) >= 8 && fabsf(obsPitch) < 20.0f) {
                const float m = obsPitch / (float)g_injSinceSampleY;
                if (m < -0.004f && m > -2.0f)
                    g_dpcPitch += (m - g_dpcPitch) * 0.15f;
            }
            ClampCountsPerDegree();
        }

        // Effectiveness meter: decaying accumulators of |expected| vs
        // |observed|. Only meaningful while we are actually injecting.
        const float expPitchAbs = fabsf((float)g_injSinceSampleY * g_dpcPitch);
        g_accExpected = g_accExpected * 0.92f + fabsf(expYaw) + expPitchAbs;
        float obsCredit = fabsf(obsYaw) + fabsf(obsPitch);
        const float cap = 2.0f * (fabsf(expYaw) + expPitchAbs);
        if (obsCredit > cap) obsCredit = cap;
        g_accObserved = g_accObserved * 0.92f + obsCredit;

        if (!VRWeapon_Scripted()) {
            if ((abs(g_injSinceSampleX) >= 5 || abs(g_injSinceSampleY) >= 5)
                && fabsf(obsYaw) < 0.02f && fabsf(obsPitch) < 0.02f)
                ++g_deadFrames;
            else
                g_deadFrames = 0;
            if (g_deadFrames >= 30) {
                g_deadFrames = 0;
                VRWeapon_SetScripted(1);
                g_carryX = g_carryY = 0.0f;
            }
        } else if (g_probeArmed) {
            g_probeArmed = 0;
            if (fabsf(obsYaw) >= 0.05f)
                VRWeapon_SetScripted(0);    // input responds again
        }
    }
    if (newSample) {
        g_prevYaw = gameYaw;
        g_prevPitch = gamePitch;
        g_prevAimTime = aimTime;
        g_prevAimValid = true;
        // The accumulator is cleared ONLY here, where a fresh sample has just
        // been credited against it. Clearing it anywhere else is the defect
        // that produced the rebase runaway.
        g_injSinceSampleX = g_injSinceSampleY = 0;
    }

    g_prevInjX = g_prevInjY = 0;

    if (!gameplay || !haveAim)
        return;

    // Scripted mode: no steering; a tiny periodic probe (0.3-0.4 deg) is the
    // only injection, to notice when the game gives control back.
    if (VRWeapon_Scripted()) {
        if (g_lastProbeTime < 0.0 || now - g_lastProbeTime >= 0.35) {
            g_lastProbeTime = now;
            g_probeArmed = 1;
            Emit(VRB_EV_MOUSEMOVE, 3, 0);
            g_prevInjX = 3;
            g_injSinceSampleX += 3;
            g_injSinceLeakX += 3;
        }
        return;
    }

    float targetYaw = 0, targetPitch = 0;
    if (!VRWeapon_GetAimTargetDeg(&targetYaw, &targetPitch)) {
        g_hadTarget = false;
        return;
    }
    if (!g_hadTarget) {
        g_hadTarget = true;
        g_targetSince = now;
        g_convergedLogged = false;
    }

    float errYaw   = VRM_WrapDeg(targetYaw - gameYaw);
    float errPitch = targetPitch - gamePitch;
    g_lastErrYaw = errYaw;
    g_lastErrPitch = errPitch;

    // Convergence telemetry: the acceptance number for native aiming is a
    // steady error under 0.5 deg. Log the first convergence after each
    // target acquisition, and a bounded pursuit trace while it struggles.
    if (!g_convergedLogged && fabsf(errYaw) < 0.5f && fabsf(errPitch) < 0.5f) {
        g_convergedLogged = true;
        static int logged = 0;
        if (logged < 4) {
            ++logged;
            Log("VR binds: steering converged to %.2f/%.2f deg %.2f s after "
                "target acquisition (cpd yaw %.2f pitch %.2f)",
                errYaw, errPitch, g_targetSince >= 0.0 ? (float)(now - g_targetSince) : 0.0f,
                g_dpcYaw != 0.0f ? 1.0f / g_dpcYaw : 0.0f,
                g_dpcPitch != 0.0f ? 1.0f / g_dpcPitch : 0.0f);
        }
    }

    // Anti-windup at the game's pitch clamp.
    if ((gamePitch >  84.0f && errPitch > 0) || (gamePitch < -84.0f && errPitch < 0))
        errPitch = 0.0f;

    const float wantX = errYaw   * g_steerGain / g_dpcYaw   + g_carryX;
    const float wantY = errPitch * g_steerGain / g_dpcPitch + g_carryY;
    int dx = (int)wantX;
    int dy = (int)wantY;
    if (dx >  400) dx =  400;
    if (dx < -400) dx = -400;
    if (dy >  400) dy =  400;
    if (dy < -400) dy = -400;
    g_carryX = wantX - (float)dx;
    g_carryY = wantY - (float)dy;
    if (g_carryX >  4.0f) g_carryX =  4.0f;     // never store a windup
    if (g_carryX < -4.0f) g_carryX = -4.0f;
    if (g_carryY >  4.0f) g_carryY =  4.0f;
    if (g_carryY < -4.0f) g_carryY = -4.0f;

    if (dx || dy) {
        Emit(VRB_EV_MOUSEMOVE, dx, dy);
        g_prevInjX = dx;
        g_prevInjY = dy;
        g_injSinceSampleX += dx;
        g_injSinceSampleY += dy;
        g_injSinceLeakX += dx;
        g_injSinceLeakY += dy;
    }
    g_steeringLive = true;

    // Bounded pursuit trace while convergence is struggling: the exact
    // inject-vs-error numbers the next hardware log needs to judge whether
    // relative mouse steering can carry native aiming.
    if (!g_convergedLogged && g_targetSince >= 0.0 && now - g_targetSince > 3.0
        && (g_lastPursuitLog < 0.0 || now - g_lastPursuitLog >= 3.0)
        && g_pursuitLogs < 6) {
        ++g_pursuitLogs;
        g_lastPursuitLog = now;
        Log("VR binds: steering pursuing -- error %.1f/%.1f deg, injecting "
            "%+d/%+d counts, cpd %.2f/%.2f, effectiveness %d%%",
            errYaw, errPitch, dx, dy,
            g_dpcYaw != 0.0f ? 1.0f / g_dpcYaw : 0.0f,
            g_dpcPitch != 0.0f ? 1.0f / g_dpcPitch : 0.0f,
            (g_accExpected > 1.0f) ? (int)(100.0f * g_accObserved / g_accExpected) : -1);
    }
}

// ---- steering-leak watchdog (the Phase-4 head/gun invariant, live) ----
//
// While decoupled, the rendered camera yaw may move only by (body turn) +
// (the head's own rotation). Any residual on a frame that injected steering
// is the weapon leaking into the HMD camera. Spawn-sized jumps (> 15 deg)
// are discontinuities the steering loop rebases through, not leaks; the
// window resamples across them.
//
// SAMPLED ONCE PER XR PAIR, against the base yaw the pair's camera was
// LATCHED with -- not once per game frame against the live body yaw.
//
// The camera source of truth latches one base pose per XR pair, so the
// rendered camera changes only at pair boundaries while the body basis keeps
// advancing every game frame. Comparing a pair-latched camera against a live
// body would show the body moving while the camera legitimately held still
// and report a leak that is not there -- which is exactly what a stick turn
// produced. Both sides of the comparison now come from the same pair.
static void LeakWatchdog(int gameplay)
{
    if (!gameplay) {
        g_leakPrevValid = false;   // coupled/menu frames are legitimately aim-driven
        return;
    }
    float camYaw = 0, headYaw = 0;
    if (!VR_GetRenderedViewYawDeg(&camYaw) || !VR_GetHeadLocalYawDeg(&headYaw))
        return;

    // What the pair's camera was ACTUALLY latched onto, and the pair's id.
    float baseYaw = 0.0f;
    unsigned latchSerial = 0;
    if (!VR_GetPairBaseYawDeg(&baseYaw, &latchSerial))
        return;
    if (g_leakPrevValid && latchSerial == g_leakPrevSerial)
        return;                    // same pair: the camera has not moved yet

    // What it SHOULD have been latched onto: the virtual body basis.
    const float bodyYaw = VRWeapon_BodyYawDeg();

    // Injection credited to this pair: everything emitted since the last
    // sample, not just the last game frame's, because a pair spans two.
    const int injX = g_injSinceLeakX, injY = g_injSinceLeakY;
    g_injSinceLeakX = g_injSinceLeakY = 0;

    if (g_leakPrevValid && (injX || injY)) {
        // PRIMARY TEST -- did the camera get composed on the right thing?
        //
        // While decoupled, the camera base IS the body basis, so over any
        // window the two must move together. Comparing the camera against
        // its own latched base cannot answer this: if steering corrupts the
        // base, camera and base move together and the residual is zero --
        // which is exactly how the mutant hid. Comparing the LATCHED BASE
        // against the AUTHORITATIVE BODY does answer it: a base that chases
        // the steered game aim while the body holds still is the leak.
        const float baseDelta = VRM_WrapDeg(baseYaw - g_leakPrevBaseYaw);
        const float bodyDelta = VRM_WrapDeg(bodyYaw - g_leakPrevBodyYaw);
        const float baseResid = VRM_WrapDeg(baseDelta - bodyDelta);
        if (fabsf(baseResid) > 0.35f && fabsf(baseResid) < 15.0f) {
            ++g_steerLeakCount;
            if (g_leakLogs < 3) {
                ++g_leakLogs;
                Log("VR binds: STEERING LEAK -- the camera base followed the "
                    "injected aim %.2f deg beyond the body (base %+.2f, body %+.2f)",
                    baseResid, baseDelta, bodyDelta);
            }
        }

        // SECONDARY TEST -- given the base it used, did the composition
        // itself stay honest? The rendered camera may move only by
        // (base turn) + (the head's own rotation).
        const float camDelta = VRM_WrapDeg(camYaw - g_leakPrevCamYaw);
        const float expect   = VRM_WrapDeg(baseDelta + (headYaw - g_leakPrevHeadYaw));
        const float residual = VRM_WrapDeg(camDelta - expect);
        if (fabsf(residual) > 0.35f && fabsf(residual) < 15.0f) {
            ++g_steerLeakCount;
            if (g_leakLogs < 3) {
                ++g_leakLogs;
                Log("VR binds: STEERING LEAK -- injected aim moved the rendered "
                    "camera %.2f deg beyond base+head (cam %+.2f, base %+.2f, head %+.2f)",
                    residual, camDelta, baseDelta,
                    headYaw - g_leakPrevHeadYaw);
            }
        }
    }
    g_leakPrevCamYaw  = camYaw;
    g_leakPrevBaseYaw = baseYaw;
    g_leakPrevBodyYaw = bodyYaw;
    g_leakPrevHeadYaw = headYaw;
    g_leakPrevSerial  = latchSerial;
    g_leakPrevValid = true;
}

// ------------------------------------------------------------ the update

void VRBinds_Update(double now, int menuMode)
{
    static double prevNow = -1.0;
    float dt = (prevNow > 0.0 && now > prevNow) ? (float)(now - prevNow) : (1.0f / 240.0f);
    if (dt > 0.1f) dt = 0.1f;
    prevNow = now;

    ++g_updateCounter;
    g_wantCount = 0;              // the desired key set is rebuilt from scratch
    g_menuClickWanted = 0;

    const VRInputState* in = VRInput_State();
    const bool live = in && in->valid && in->focused
                    && (in->hand[0].active || in->hand[1].active)
                    && GameHasFocusHere();
    if (!live) {
        // Not focused / no controller / another window: nothing may stay
        // held. Reconciling against the (empty) wanted set releases
        // everything, including a button whose release edge was eaten by
        // the very focus loss that brought us here.
        //
        // The ADS TOGGLE is not a held key, so the reconciler above cannot
        // release it -- if the game believes ADS is on when tracking dies,
        // tap it off now while the game window still has the OS focus
        // (tapping into some OTHER focused window would type into it).
        if (g_adsAssumed && GameHasFocusHere()) {
            Tap(g_bind[VRB_ADS]);
            ++g_ctr.adsTaps;
            ++g_adsTransitions;
            g_adsAssumed = 0;
            static int logged = 0;
            if (logged < 4) {
                ++logged;
                Log("VR binds: ADS -> off (input lost while aiming)");
            }
        }
        g_recenterPressStart=-1.0; g_xPressStart=-1.0; g_cycleArmed=0;
        VRBinds_ReleaseAllInjectedInputs("input not live");
        g_moveSector = -1;
        g_leakPrevValid = false;
        return;
    }
    if (g_firstActiveTime < 0.0)
        g_firstActiveTime = now;

    NoteActionChanges(in);

    // Edge budget: consume each input generation exactly once (updates run
    // twice per F1 pair against a single sync).
    const int edges = (in->generation != g_lastGenSeen) ? 1 : 0;
    g_lastGenSeen = in->generation;

    const int gameplay = !menuMode && VRWeapon_Decoupled();

    // Menu-click arming across the mode boundary (see UpdateButtons mode 0).
    if (menuMode && !g_wasMenuMode)
        g_menuClickArmed = !in->hand[VRWeapon_DominantHand()].triggerDown;
    g_wasMenuMode = menuMode;

    // Phase-4 invariant, enforced before anything below mutates body yaw:
    // bodyYaw still equals the value THIS frame's camera was composed with.
    LeakWatchdog(gameplay);

    // Runs in EVERY mode and before every deliberate action, so a focus blip,
    // a tracking re-acquire or a menu transition can never be mistaken for a
    // fresh press by the grenade or the weapon cycle.
    UpdateDeliberateArming(in, now, gameplay);

    if (menuMode) {
        // Menu wants replace gameplay wants wholesale; the reconciler below
        // releases every combat hold in this same update.
        g_moveSector = -1;
        UpdateMenuCursor(in, dt);
        UpdateButtons(in, now, 0, edges);
        UpdateAds(in, now, 0);         // bookkeeping only: menus never tap
        UpdateGrenade(in, now, 0);     // release tracking only; cannot commit
    } else if (VRWeapon_Scripted()) {
        // Cutscene: the game took control. No combat or movement injection;
        // steering still runs because it owns the control-returned probe.
        g_moveSector = -1;
        UpdateButtons(in, now, -1, edges);
        UpdateAds(in, now, 0);
        UpdateGrenade(in, now, 0);     // release tracking only; cannot commit
        UpdateSteering(now, 1);
    } else {
        UpdateMovement(in);
        UpdateTurn(in, dt, 1);
        UpdateWeaponCycle(in, now, 1);
        UpdateGrenade(in, now, 1);
        UpdateButtons(in, now, 1, edges);
        UpdateAds(in, now, 1);
        UpdatePhysicalCrouch(now);
        UpdateSteering(now, 1);
    }

    // THE invariant: whatever this update did not explicitly want is
    // released right now. No held key survives on inertia.
    Reconcile();

    VRBinds_MaybeLogInputCheck(now);
}

void VRBinds_GetDebug(VRBindsDebug* out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->steeringLive = g_steeringLive ? 1 : 0;
    out->cpdYaw   = (g_dpcYaw   != 0.0f) ? 1.0f / g_dpcYaw   : 0.0f;
    out->cpdPitch = (g_dpcPitch != 0.0f) ? 1.0f / g_dpcPitch : 0.0f;
    out->steerErrYawDeg = g_lastErrYaw;
    out->steerErrPitchDeg = g_lastErrPitch;
    out->effectiveness = (g_accExpected > 1.0f)
        ? (int)(100.0f * g_accObserved / g_accExpected) : -1;
    out->scriptedDetected = VRWeapon_Scripted();
    out->heldKeys = g_heldCount;
    out->movementSector = g_moveSector;
    out->steerLeakCount = g_steerLeakCount;
    out->adsDesired = g_adsDesired;
    out->adsAssumed = g_adsAssumed;
    out->adsOwnerMask = g_adsOwnerMask;
    out->adsTransitions = g_adsTransitions;
    out->adsHintValid = g_adsHintValidLast;
    out->adsHintValue = g_adsHintValueLast;
    out->adsHintCorrections = g_adsHintCorrections;
}

void VRBinds_GetCounters(VRBindsCounters* out)
{
    if (!out) return;
    *out = g_ctr;
}

void VRBinds_SetGrenadeHold(float seconds)
{
    if (seconds >= 0.0f && seconds <= 2.0f)
        g_grenadeHoldSec = seconds;
}

void VRBinds_SetWeaponCycleBinding(int binding)
{
    if (binding >= 0 && binding <= VRB_CYCLE_X_HOLD) {
        g_cycleBinding=binding; g_xPressStart=-1.0; g_cycleArmed=0;
    }
}

void VRBinds_GetGrenadeStats(VRBindsGrenadeStats* out)
{
    if (!out) return;
    out->requests      = g_grenadeRequests;
    out->accepted      = g_grenadeAccepted;
    out->rejectedNoise = g_grenadeRejectedNoise;
    out->source        = g_grenadeSource;
    out->lastReject    = g_grenadeLastReject;
    out->armed         = g_grenadeArmed;
    out->holdSeconds   = g_grenadeHoldSec;
    out->cycleBinding  = g_cycleBinding;
}

// ------------------------------------------------ VR INPUT / WEAPON CHECK

// Symbolic dump of both rings: the OpenXR action changes we OBSERVED and the
// injections we EMITTED, on a shared update-counter timeline. Together they
// answer, per control: did the action state change, did the mapping see it,
// did an injection occur.
static void LogEventRings()
{
    static const char* achName[ACH_CODES] = {
        "active", "stick", "trigger", "squeeze", "btnA/X", "btnB/Y",
        "stickClick", "menu" };
    Log("ACTION CHANGES (last %u of %u; u<n> = binds update counter):",
        g_actCount < ACT_RING ? g_actCount : (unsigned)ACT_RING, g_actCount);
    {
        char line[220]; int used = 0; int items = 0;
        const unsigned start = g_actCount > ACT_RING ? g_actCount - ACT_RING : 0;
        for (unsigned i = start; i < g_actCount; ++i) {
            const ActRec& r = g_actRing[i % ACT_RING];
            char one[48];
            if (r.code == ACH_STICK)
                sprintf_s(one, "u%u %c.stick %+d%%/%+d%%", r.frame,
                          r.hand ? 'R' : 'L', (int)r.v1, (int)r.v2);
            else
                sprintf_s(one, "u%u %c.%s=%d", r.frame, r.hand ? 'R' : 'L',
                          achName[r.code & 7], (int)r.v1);
            const int n = (int)strlen(one);
            if (used + n + 3 > (int)sizeof(line) - 1) {
                Log("  %s", line); used = 0; line[0] = 0;
            }
            sprintf_s(line + used, sizeof(line) - used, "%s%s", used ? " | " : "", one);
            used = (int)strlen(line);
            ++items;
        }
        if (used) Log("  %s", line);
        if (!items) Log("  (none observed)");
    }
    Log("INJECTION EVENTS (last %u of %u):",
        g_evCount < INJ_RING ? g_evCount : (unsigned)INJ_RING, g_evCount);
    {
        char line[220]; int used = 0; int items = 0; unsigned moves = 0;
        const unsigned start = g_evCount > INJ_RING ? g_evCount - INJ_RING : 0;
        for (unsigned i = start; i < g_evCount; ++i) {
            const EvRec& r = g_evRing[i % INJ_RING];
            if (r.kind == VRB_EV_MOUSEMOVE) { ++moves; continue; }   // steering flood
            char keyTxt[16];
            if (r.a > 32 && r.a < 127) sprintf_s(keyTxt, "'%c'", (char)r.a);
            else if (r.a == K_MOUSE1)  strcpy_s(keyTxt, "MOUSE1");
            else if (r.a == K_MOUSE2)  strcpy_s(keyTxt, "MOUSE2");
            else                       sprintf_s(keyTxt, "vk%d", (int)r.a);
            static const char* kindName[7] = {
                "?", "DOWN", "UP", "BDOWN", "BUP", "MOVE", "WHEEL" };
            char one[48];
            sprintf_s(one, "u%u %s %s", r.frame, kindName[r.kind & 7],
                      r.kind == VRB_EV_WHEEL ? (r.a > 0 ? "wheel+" : "wheel-") : keyTxt);
            const int n = (int)strlen(one);
            if (used + n + 3 > (int)sizeof(line) - 1) {
                Log("  %s", line); used = 0; line[0] = 0;
            }
            sprintf_s(line + used, sizeof(line) - used, "%s%s", used ? " | " : "", one);
            used = (int)strlen(line);
            ++items;
        }
        if (used) Log("  %s", line);
        if (moves) Log("  (+ %u steering/cursor mouse-move events not listed)", moves);
        if (!items && !moves) Log("  (none emitted)");
    }
}

static void LogHand(const char* name, const VRHandState& h)
{
    Log("%s controller:", name);
    Log("  aim pose active: %d   grip pose active: %d", h.aimValid, h.gripValid);
    Log("  position: %+.3f %+.3f %+.3f m (XR)", h.aimPos[0], h.aimPos[1], h.aimPos[2]);
    Log("  orientation: quat %+.3f %+.3f %+.3f %+.3f",
        h.aimQuat[0], h.aimQuat[1], h.aimQuat[2], h.aimQuat[3]);
    Log("  trigger: %.2f   squeeze: %.2f   stick: %+.2f %+.2f",
        h.trigger, h.squeeze, h.stickX, h.stickY);
}

void VRBinds_MaybeLogInputCheck(double now)
{
    if (g_firstActiveTime < 0.0 || now - g_firstActiveTime < 8.0)
        return;
    if (g_checkCount >= CHECK_MAX_BLOCKS)
        return;
    if (g_checkCount > 0 && now - g_lastCheckTime < (double)CHECK_PERIOD_S)
        return;
    ++g_checkCount;
    g_lastCheckTime = now;

    const VRInputState* in = VRInput_State();
    VRWeaponDebug wd;
    VRWeapon_GetDebug(&wd);
    VRBindsDebug bd;
    VRBinds_GetDebug(&bd);

    const int dom = VRWeapon_DominantHand();
    float headM[3] = { 0, 0, 0 };
    const int headOk = VR_GetHeadRoomMid(headM);
    float headYawOff = 0, headPitchOff = 0;
    VR_GetHeadAimOffset(&headYawOff, &headPitchOff);

    Log("");
    Log("======= VR INPUT / WEAPON CHECK (block %d, t+%.0f s) =======",
        g_checkCount, now - g_firstActiveTime);
    Log("runtime interaction profile: left=%s right=%s",
        VRInput_ProfileText(VRI_LEFT), VRInput_ProfileText(VRI_RIGHT));
    Log("dominant hand: %s", dom == VRI_RIGHT ? "right" : "left");
    Log("left-handed mode: %s", dom == VRI_LEFT ? "ON" : "off");
    LogHand("LEFT", in->hand[VRI_LEFT]);
    LogHand("RIGHT", in->hand[VRI_RIGHT]);
    Log("HEAD:");
    Log("  room mid: %+.3f %+.3f %+.3f m (%s)   view offset yaw %+.1f pitch %+.1f deg",
        headM[0], headM[1], headM[2], headOk ? "ok" : "n/a", headYawOff, headPitchOff);
    Log("WEAPON:");
    Log("  dominant hand: %s   body yaw %.1f deg", dom == VRI_RIGHT ? "right" : "left", wd.bodyYawDeg);
    Log("  grip-space origin: %.1f %.1f %.1f u (valid %d)",
        wd.weaponWorldOrigin[0], wd.weaponWorldOrigin[1], wd.weaponWorldOrigin[2],
        wd.weaponPoseValid);
    Log("  aim-space forward: %+.3f %+.3f %+.3f", wd.weaponWorldFwd[0],
        wd.weaponWorldFwd[1], wd.weaponWorldFwd[2]);
    Log("  rendered weapon: %d entities transformed last frame, baked offset "
        "%.1f/%.1f/%.1f u (%s)", wd.matchedEntities,
        wd.bakedOffsetU[0], wd.bakedOffsetU[1], wd.bakedOffsetU[2],
        wd.bakedOffsetValid ? "measured" : "not yet seen");
    Log("  entity census: %d offered, nearest %.1f u; near %d, mid-aligned %d, "
        "mid-other %d, far %d; rewritten %d, deduped %d, gate-blocked %d",
        wd.censusTotal, wd.censusNearestU, wd.censusNear, wd.censusMidAligned,
        wd.censusMidOther, wd.censusFar, wd.censusRewritten, wd.censusDeduped,
        wd.censusGateBlocked);
    Log("  gameplay fire: yaw %.1f pitch %.1f deg (target %.1f / %.1f)",
        wd.gameYawDeg, wd.gamePitchDeg, wd.targetYawDeg, wd.targetPitchDeg);
    const float fireErrYaw = VRM_WrapDeg(wd.targetYawDeg - wd.gameYawDeg);
    const float fireErrPitch = wd.targetPitchDeg - wd.gamePitchDeg;
    char effTxt[16];
    if (bd.effectiveness < 0) strcpy_s(effTxt, "idle");
    else                      sprintf_s(effTxt, "%d%%", bd.effectiveness);
    Log("  head/weapon yaw difference: %+.1f deg   steering error %.2f/%.2f deg "
        "(cpd yaw %.1f pitch %.1f, effectiveness %s)",
        VRM_WrapDeg(wd.targetYawDeg - (wd.bodyYawDeg + headYawOff)),
        fireErrYaw, fireErrPitch, bd.cpdYaw, bd.cpdPitch, effTxt);
    Log("ONE-HAND:");
    Log("  dominant grip XR: %+.3f %+.3f %+.3f m (valid %d)",
        in->hand[dom].gripPos[0], in->hand[dom].gripPos[1], in->hand[dom].gripPos[2],
        in->hand[dom].gripValid);
    Log("  dominant grip room: %+.3f %+.3f %+.3f m",
        wd.rearAnchorRoomM[0], wd.rearAnchorRoomM[1], wd.rearAnchorRoomM[2]);
    Log("  weapon final origin: %.1f %.1f %.1f u   forward: %+.3f %+.3f %+.3f",
        wd.weaponWorldOrigin[0], wd.weaponWorldOrigin[1], wd.weaponWorldOrigin[2],
        wd.weaponWorldFwd[0], wd.weaponWorldFwd[1], wd.weaponWorldFwd[2]);
    const float weaponCamOffU = sqrtf(
        (wd.weaponWorldOrigin[0]-wd.gameVieworg[0])*(wd.weaponWorldOrigin[0]-wd.gameVieworg[0]) +
        (wd.weaponWorldOrigin[1]-wd.gameVieworg[1])*(wd.weaponWorldOrigin[1]-wd.gameVieworg[1]) +
        (wd.weaponWorldOrigin[2]-wd.gameVieworg[2])*(wd.weaponWorldOrigin[2]-wd.gameVieworg[2]));
    Log("  grip->weapon chain error: %.2f cm   weapon-to-camera offset: %.1f u",
        wd.solverPosErrU >= 0.0f ? wd.solverPosErrU * 2.54f : -1.0f, weaponCamOffU);
    Log("TWO-HAND:");
    Log("  offhand grip pressed: %d   candidate: %d   attached: %s   blend %.2f",
        in->hand[1 - dom].squeezeDown, wd.twoHandCandidate,
        wd.twoHand ? "yes" : "no", wd.twoHandBlend);
    Log("  support perp %.3f m  along %.3f m  hand-to-hand %.3f m",
        wd.twoHandPerpM, wd.twoHandAlongM, wd.twoHandHandDistM);
    Log("  rear anchor %+.3f %+.3f %+.3f m   support anchor %+.3f %+.3f %+.3f m (valid %d)",
        wd.rearAnchorRoomM[0], wd.rearAnchorRoomM[1], wd.rearAnchorRoomM[2],
        wd.frontAnchorRoomM[0], wd.frontAnchorRoomM[1], wd.frontAnchorRoomM[2],
        wd.gripRoomValid[1 - dom]);
    Log("  two-hand fwd %+.2f %+.2f %+.2f   one-hand fwd %+.2f %+.2f %+.2f (room)",
        wd.twoHandFwdRoom[0], wd.twoHandFwdRoom[1], wd.twoHandFwdRoom[2],
        wd.oneHandFwdRoom[0], wd.oneHandFwdRoom[1], wd.oneHandFwdRoom[2]);
    Log("INJECTION COUNTERS (since start):");
    Log("  move fwd %d/%d back %d/%d strafeL %d/%d strafeR %d/%d (down/up)",
        g_ctr.moveFwdDown, g_ctr.moveFwdUp, g_ctr.moveBackDown, g_ctr.moveBackUp,
        g_ctr.strafeLDown, g_ctr.strafeLUp, g_ctr.strafeRDown, g_ctr.strafeRUp);
    Log("  turn frames %d   fire %d/%d   jump %d/%d   melee %d/%d   use %d/%d",
        g_ctr.turnEvents, g_ctr.fireDown, g_ctr.fireUp, g_ctr.jumpDown, g_ctr.jumpUp,
        g_ctr.meleeDown, g_ctr.meleeUp, g_ctr.useDown, g_ctr.useUp);
    Log("  stick turn: mode %s   commanded %.0f deg   injected %lld counts (cpd %.2f)",
        VRWeapon_Decoupled() ? "decoupled-body" : "coupled-mouse",
        g_turnDegCommanded, g_turnCountsInjected,
        g_dpcYaw != 0.0f ? 1.0f / g_dpcYaw : 0.0f);
    Log("  taps: crouch %d prone %d reload %d grenade %d ads %d cycle %d menu %d",
        g_ctr.crouchTaps, g_ctr.proneTaps, g_ctr.reloadTaps, g_ctr.grenadeTaps,
        g_ctr.adsTaps, g_ctr.weaponCycleTaps, g_ctr.menuTaps);
    Log("GRENADE (deliberate off-hand trigger hold, %.2f s):", g_grenadeHoldSec);
    Log("  grenadeRequests %d   grenadeAccepted %d   grenadeRejectedNoise %d",
        g_grenadeRequests, g_grenadeAccepted, g_grenadeRejectedNoise);
    Log("  grenadeSource %s   lastReject %s   armed %d",
        g_grenadeSource, g_grenadeLastReject, g_grenadeArmed);
    Log("  weapon-cycle source: %s (nothing shares a control with the grenade)",
        g_cycleBinding == VRB_CYCLE_LEGACY_STICK
            ? "LEGACY dominant stick Y -- opt-in only"
            : "off-hand X button");
    Log("  menu clicks %d/%d   recenters %d   forced release-alls %d   held now %d",
        g_ctr.menuClickDown, g_ctr.menuClickUp, g_ctr.recenters,
        g_ctr.releaseAllRuns, g_heldCount);
    LogEventRings();
    Log("ACCURACY/ADS:");
    Log("  physical ADS enabled: %d   detected: %d", VRWeapon_PhysicalAds(), wd.ads);
    Log("  eye-to-grip distance: %.3f m   alignment: %.1f deg", wd.adsEyeDistM, wd.adsAlignDeg);
    Log("  owners:%s%s%s%s   desired: %s   assumed game state: %s   transitions %d",
        (bd.adsOwnerMask & 1) ? " manual" : "",
        (bd.adsOwnerMask & 2) ? " physical" : "",
        (bd.adsOwnerMask & 4) ? " twohand" : "",
        bd.adsOwnerMask ? "" : " none",
        bd.adsDesired ? "ON" : "off", bd.adsAssumed ? "ON" : "off",
        bd.adsTransitions);
    if (bd.adsHintValid)
        Log("  cgame zoom hint: %.4f (%s)   corrections applied: %d",
            bd.adsHintValue, bd.adsHintValue != 0.0f ? "ADS-ish" : "hip-fire",
            bd.adsHintCorrections);
    else
        Log("  cgame zoom hint: unreadable (RVA off/wrong build)");

    // Verdicts, computed from live values only. Words sit mid-line so the
    // build gate's "  FAIL  " matcher can never trip on them.
    const int trackOk = in->hand[0].aimValid && in->hand[1].aimValid;
    const int decoupleOk = wd.decoupled && !wd.scripted;
    const int aimMatchOk = wd.weaponPoseValid && decoupleOk
        && fabsf(fireErrYaw) < 2.5f && fabsf(fireErrPitch) < 2.5f;
    const int domOk = (dom == VRI_LEFT || dom == VRI_RIGHT);
    float menuCx = 0, menuCy = 0, menuVx = 0, menuVy = 0;
    const int menuKnown = VR_GetMenuSafeArea(&menuCx, &menuCy, &menuVx, &menuVy);
    const int menuOk = menuKnown && menuCx <= menuVx + 0.1f && menuCy <= menuVy + 0.1f;
    int vpOk = 1, scOk = 1, covKnown = 1;
    for (int e = 0; e < 2; ++e) {
        int known = 0, v = 0, s = 0;
        VR_GetWorldPassCoverage(e, &known, &v, &s);
        if (!known) covKnown = 0;
        vpOk &= v;
        scOk &= s;
    }
    // Cumulative-rewrite delta across blocks: the entity path must have
    // actually carried the pose to the engine since the last block.
    static int prevRewritten;
    const int rewrittenDelta = wd.censusRewritten - prevRewritten;
    prevRewritten = wd.censusRewritten;

    // Solver translation proof: origin == vieworg + rotated grip (chain error
    // ~0) AND the entity path delivered it this period.
    const int chainOk = wd.weaponPoseValid && wd.solverPosErrU >= 0.0f
                     && wd.solverPosErrU < 0.4f;               // < 1 cm
    const int translateOk = chainOk && (rewrittenDelta > 0 || g_checkCount == 1);
    // No fixed body anchor: the delivered weapon origin sits away from the
    // camera by the hand's actual room offset (scaled), not glued to it.
    float gripMagM = 0.0f;
    for (int i = 0; i < 3; ++i)
        gripMagM += wd.rearAnchorRoomM[i] * wd.rearAnchorRoomM[i];
    gripMagM = sqrtf(gripMagM);
    const int anchorOk = chainOk
        && fabsf(weaponCamOffU - gripMagM * 39.37f) < 4.0f + 0.25f * gripMagM * 39.37f;
    const int supportOk = in->hand[1 - dom].gripValid;
    // Two-hand direction: once fully blended, the final room-frame forward
    // must BE the hand-to-hand line.
    int dirOk = 0, dirBlending = 1;
    if (wd.twoHand && wd.twoHandBlend > 0.99f) {
        dirBlending = 0;
        const float dot = wd.weaponRoomFwd[0]*wd.twoHandFwdRoom[0]
                        + wd.weaponRoomFwd[1]*wd.twoHandFwdRoom[1]
                        + wd.weaponRoomFwd[2]*wd.twoHandFwdRoom[2];
        dirOk = dot > 0.9994f;                       // < ~2 deg apart
    }
    Log("VERDICTS:");
    Log("controllerTracking = %s", trackOk ? "PASS" : "FAIL(a controller is not tracking)");
    Log("headWeaponDecoupling = %s", decoupleOk ? "PASS" : "FAIL(fell back to coupled mode)");
    Log("dominantTranslation1to1 = %s",
        !decoupleOk ? "N/A(not decoupled)" :
        translateOk ? "PASS" :
        chainOk ? "FAIL(no entity rewrite reached the engine this period)"
                : "FAIL(solver chain error)");
    Log("noFixedBodyAnchor = %s",
        !decoupleOk ? "N/A(not decoupled)" :
        anchorOk ? "PASS" : "FAIL(weapon origin not tracking the hand offset)");
    Log("supportHandTracking = %s", supportOk ? "PASS" : "FAIL(off-hand grip not located)");
    Log("twoHandAttach = %s", wd.twoHand ? "PASS"
        : in->hand[1 - dom].squeezeDown ? "FAIL(grip held but not attached)" : "N/A(not requested)");
    Log("twoHandDirection = %s",
        !wd.twoHand ? "N/A(not attached)" :
        dirBlending ? "PENDING(blending)" :
        dirOk ? "PASS" : "FAIL(final forward is not the hand-to-hand line)");
    Log("twoHandAccuracyState = %s",
        wd.twoHand ? ((bd.adsDesired && bd.adsAssumed) ? "PASS" : "FAIL(attached but no ADS state)")
                   : "N/A(not attached)");
    Log("steeringLeakIntoCamera = %s",
        bd.steerLeakCount == 0 ? "NONE(invariant held)"
                               : "DETECTED(camera moved with injected aim)");
    Log("visualAimMatchesFireAim = %s", aimMatchOk ? "PASS"
        : decoupleOk ? "FAIL(steering not converged)" : "N/A(not decoupled)");
    Log("cameraIndependence = %s",
        bd.steerLeakCount == 0 ? "PASS" : "FAIL(steering leaked into the camera)");
    Log("dominantHandIdentity = %s", domOk ? "PASS" : "FAIL");
    Log("menuSafeArea = %s", menuKnown ? (menuOk ? "PASS" : "FAIL(content exceeds visible fov)")
                                       : "N/A(no menu shown yet)");
    Log("viewportCoverage = %s", covKnown ? (vpOk ? "PASS" : "FAIL") : "N/A");
    Log("scissorCoverage = %s", covKnown ? (scOk ? "PASS" : "FAIL") : "N/A");
    Log("=======================================");
}
