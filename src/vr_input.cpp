// OpenXR action-system implementation. See vr_input.h for the contract.
//
// Design constraints that shaped this file:
//  * Action sets/actions are INSTANCE children; action spaces and the attach
//    are SESSION children. VirtualDesktopXR reconnects destroy and recreate
//    the session while the instance survives, so the two lifetimes are kept
//    rigorously apart: session teardown only destroys spaces and forgets the
//    attach, instance teardown destroys everything.
//  * Bindings are suggested at INSTANCE creation, immediately after the
//    actions exist -- the exact order hello_xr and q3vr use. The 2026-08-20
//    hardware run suggested them after xrCreateSession (spec-legal, but a
//    known runtime-bug surface) and the controllers never bound; nothing in
//    that run depended on the late order, so it was retired.
//  * xrAttachSessionActionSets is once-per-session and irrevocable, so it is
//    driven from VRInput_OnSessionCreated, never from the frame loop.
//  * Everything is created exactly once; the frame path performs no OpenXR
//    object creation and no heap allocation.
//  * Every XrResult is checked AND RECORDED. The same run proved that silent
//    success-path telemetry is worthless on hardware: attach logged PASS and
//    then nothing distinguished "sync failing", "sync unfocused", "runtime
//    never bound a profile" and "controllers asleep". The bring-up ladder
//    (VRI_BU_*) and its one-shot report exist to make those four cases
//    unambiguous in a single log block.

#include "vr_input.h"
#include "codvr.h"
#include "vr_math.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

extern void Log(const char* fmt, ...);

// ------------------------------------------------------------------ state

static XrInstance  g_inst;
static XrSession   g_sess;
static XrActionSet g_set;

enum {
    ACT_AIM = 0, ACT_GRIP, ACT_TRIGGER, ACT_SQUEEZE, ACT_STICK,
    ACT_STICK_CLICK, ACT_BTN_PRIMARY, ACT_BTN_SECONDARY, ACT_MENU,
    ACT_HAPTIC, ACT_COUNT
};
static XrAction g_act[ACT_COUNT];
static XrPath   g_handPath[VRI_HANDS];
static XrSpace  g_aimSpace[VRI_HANDS];
static XrSpace  g_gripSpace[VRI_HANDS];
static bool     g_actionsLive;
static bool     g_attached;
static bool     g_bindingsSuggested;   // per instance, like the actions
static VRInputState g_state;

static unsigned g_syncFailLogged;
static unsigned g_locateFailLogged;

// Edge-tracking state across syncs (file scope, not function statics, so the
// session/test resets can clear it -- a stale "was pressed" bit across a
// session rebuild would eat the first real press).
// -1 means "no live sync seen yet"; see BoolEdge.
static int  g_prevBtnP[VRI_HANDS] = { -1, -1 };
static int  g_prevBtnS[VRI_HANDS] = { -1, -1 };
static int  g_prevStick[VRI_HANDS] = { -1, -1 };
static int  g_prevMenu = -1;
static bool g_profileLogged;

// ---- tracking state machine (per hand) ----
static VRITrack  g_track[VRI_HANDS];            // the tracking state machine
static int       g_trackStreak[VRI_HANDS];      // consecutive active syncs
static long long g_trackLastActiveNs[VRI_HANDS];
static unsigned  g_trackLogs;                   // transition log budget
static const long long TRACK_LOSS_NS = 1000000000LL;   // 1.0 s of displayTime

// ---- bring-up telemetry ----
static int      g_buStage;                 // highest VRI_BU_* reached
static char     g_runtimeName[64];
enum { BU_PROFILES = 3 };
struct BuProfile {
    const char* path;      // static string
    int         attempted; // SuggestProfile ran for it
    XrResult    result;    // XR_SUCCESS or the rejection
    int         controls;  // bindings the runtime was offered
};
static BuProfile g_buProf[BU_PROFILES];
static XrResult g_buAttachRes;
static bool     g_buAttachTried;
static bool     g_buSyncedOnce;
static XrResult g_buFirstSyncRes, g_buLastSyncRes;
static unsigned g_buSyncCount;
static unsigned g_buSyncChangeLogs;
static int      g_buPoseSeen[VRI_HANDS];   // pose action ever active per hand
static bool     g_buReported;
static unsigned g_focusLogs;
static int      g_prevFocused = -1;
static char     g_polledProfile[VRI_HANDS][80];  // last POLLED profile logged

int VRInput_ActionsLive() { return g_actionsLive ? 1 : 0; }
int VRInput_Attached()    { return g_attached ? 1 : 0; }
int VRInput_BringupStage(){ return g_buStage; }
VRITrack VRInput_HandTracking(int hand)
{
    return (hand >= 0 && hand < VRI_HANDS) ? g_track[hand] : VRI_TRACK_UNAVAILABLE;
}
const VRInputState* VRInput_State() { return &g_state; }
const char* VRInput_ProfileText(int hand)
{
    if (hand < 0 || hand >= VRI_HANDS) return "";
    return g_state.profile[hand][0] ? g_state.profile[hand] : "(none)";
}

void VRInput_SetRuntimeName(const char* name)
{
    if (name && name[0])
        strcpy_s(g_runtimeName, name);
}

// Symbolic names for the XrResults this layer actually meets, so hardware
// logs read as spec language instead of bare integers.
static const char* ResName(XrResult r)
{
    switch ((int)r) {
    case 0:   return "XR_SUCCESS";
    case 1:   return "XR_TIMEOUT_EXPIRED";
    case 3:   return "XR_SESSION_LOSS_PENDING";
    case 8:   return "XR_SESSION_NOT_FOCUSED";
    case -1:  return "XR_ERROR_VALIDATION_FAILURE";
    case -2:  return "XR_ERROR_RUNTIME_FAILURE";
    case -9:  return "XR_ERROR_SESSION_LOST";
    case -12: return "XR_ERROR_HANDLE_INVALID";
    case -13: return "XR_ERROR_INSTANCE_LOST";
    case -16: return "XR_ERROR_SESSION_NOT_RUNNING";
    case -19: return "XR_ERROR_PATH_INVALID";
    case -22: return "XR_ERROR_PATH_UNSUPPORTED";
    case -46: return "XR_ERROR_ACTIONSET_NOT_ATTACHED";
    case -47: return "XR_ERROR_ACTIONSETS_ALREADY_ATTACHED";
    default:  return "(unlisted)";
    }
}

static const char* BuStageName(int s)
{
    switch (s) {
    case VRI_BU_NONE:               return "NONE";
    case VRI_BU_INSTANCE_READY:     return "INPUT_INSTANCE_READY";
    case VRI_BU_BINDINGS_SUGGESTED: return "BINDINGS_SUGGESTED";
    case VRI_BU_ATTACHED:           return "SESSION_ACTIONS_ATTACHED";
    case VRI_BU_SYNCING:            return "ACTIONS_SYNCING";
    case VRI_BU_POSE_SEEN:          return "POSE_ACTIVE";
    case VRI_BU_LIVE:               return "CONTROLLERS_LIVE";
    default:                        return "?";
    }
}

static void BuAdvance(int stage)
{
    if (stage > g_buStage) {
        g_buStage = stage;
        Log("VR input: bring-up stage -> %s", BuStageName(stage));
    }
}

// -------------------------------------------------------------- creation

static XrPath PathOf(const char* s)
{
    XrPath p = XR_NULL_PATH;
    if (g_inst != XR_NULL_HANDLE)
        xrStringToPath(g_inst, s, &p);
    return p;
}

static bool MakeAction(int slot, XrActionType type, const char* name, const char* localized)
{
    XrActionCreateInfo aci = {};
    aci.type = XR_TYPE_ACTION_CREATE_INFO;
    aci.actionType = type;
    strcpy_s(aci.actionName, name);
    strcpy_s(aci.localizedActionName, localized);
    aci.countSubactionPaths = VRI_HANDS;
    aci.subactionPaths = g_handPath;
    XrResult r = xrCreateAction(g_set, &aci, &g_act[slot]);
    if (XR_FAILED(r)) {
        Log("VR input: xrCreateAction(%s) failed -> %d %s", name, (int)r, ResName(r));
        g_act[slot] = XR_NULL_HANDLE;
        return false;
    }
    return true;
}

// One suggested binding row.
struct Bind { int act; const char* path; };

static int SuggestProfile(int slot, const char* profilePath, const Bind* binds, int count)
{
    BuProfile& rec = g_buProf[slot];
    rec.path = profilePath;
    rec.attempted = 1;
    rec.result = XR_ERROR_PATH_UNSUPPORTED;
    rec.controls = 0;

    XrPath prof = PathOf(profilePath);
    if (prof == XR_NULL_PATH)
        return 0;
    XrActionSuggestedBinding sb[32];
    int n = 0;
    for (int i = 0; i < count && n < 32; ++i) {
        XrPath p = PathOf(binds[i].path);
        if (p == XR_NULL_PATH)
            continue;
        sb[n].action = g_act[binds[i].act];
        sb[n].binding = p;
        ++n;
    }
    if (!n)
        return 0;
    rec.controls = n;
    XrInteractionProfileSuggestedBinding pb = {};
    pb.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
    pb.interactionProfile = prof;
    pb.countSuggestedBindings = (uint32_t)n;
    pb.suggestedBindings = sb;
    XrResult r = xrSuggestInteractionProfileBindings(g_inst, &pb);
    rec.result = r;
    if (XR_FAILED(r)) {
        Log("VR input: bindings REJECTED for %s -> %d %s", profilePath, (int)r, ResName(r));
        ++g_state.suggestedFail;
        return 0;
    }
    Log("VR input: bindings suggested for %s (%d controls)", profilePath, n);
    ++g_state.suggestedOk;
    return 1;
}

static void SuggestAllBindings()
{
    if (g_bindingsSuggested || !g_actionsLive)
        return;
    g_bindingsSuggested = true;      // suggest-once: illegal after attach

    // Oculus/Meta Touch: the profile VirtualDesktopXR, Link and Quest air
    // runtimes all expose. This is the primary target; controller operation
    // must never depend on any OTHER profile succeeding.
    static const Bind touch[] = {
        { ACT_AIM,          "/user/hand/left/input/aim/pose" },
        { ACT_AIM,          "/user/hand/right/input/aim/pose" },
        { ACT_GRIP,         "/user/hand/left/input/grip/pose" },
        { ACT_GRIP,         "/user/hand/right/input/grip/pose" },
        { ACT_TRIGGER,      "/user/hand/left/input/trigger/value" },
        { ACT_TRIGGER,      "/user/hand/right/input/trigger/value" },
        { ACT_SQUEEZE,      "/user/hand/left/input/squeeze/value" },
        { ACT_SQUEEZE,      "/user/hand/right/input/squeeze/value" },
        { ACT_STICK,        "/user/hand/left/input/thumbstick" },
        { ACT_STICK,        "/user/hand/right/input/thumbstick" },
        { ACT_STICK_CLICK,  "/user/hand/left/input/thumbstick/click" },
        { ACT_STICK_CLICK,  "/user/hand/right/input/thumbstick/click" },
        { ACT_BTN_PRIMARY,  "/user/hand/left/input/x/click" },
        { ACT_BTN_PRIMARY,  "/user/hand/right/input/a/click" },
        { ACT_BTN_SECONDARY,"/user/hand/left/input/y/click" },
        { ACT_BTN_SECONDARY,"/user/hand/right/input/b/click" },
        { ACT_MENU,         "/user/hand/left/input/menu/click" },
        { ACT_HAPTIC,       "/user/hand/left/output/haptic" },
        { ACT_HAPTIC,       "/user/hand/right/output/haptic" },
    };
    // Khronos simple controller: poses, select as trigger, menu, haptic.
    // A float action bound to a boolean source is legal (0.0/1.0).
    static const Bind simple[] = {
        { ACT_AIM,     "/user/hand/left/input/aim/pose" },
        { ACT_AIM,     "/user/hand/right/input/aim/pose" },
        { ACT_GRIP,    "/user/hand/left/input/grip/pose" },
        { ACT_GRIP,    "/user/hand/right/input/grip/pose" },
        { ACT_TRIGGER, "/user/hand/left/input/select/click" },
        { ACT_TRIGGER, "/user/hand/right/input/select/click" },
        { ACT_MENU,    "/user/hand/left/input/menu/click" },
        { ACT_HAPTIC,  "/user/hand/left/output/haptic" },
        { ACT_HAPTIC,  "/user/hand/right/output/haptic" },
    };

    int ok = 0;
    ok += SuggestProfile(0, "/interaction_profiles/oculus/touch_controller",
                         touch, (int)(sizeof(touch)/sizeof(touch[0])));
    // Touch Plus (Quest 3's native profile) shares the Touch paths. Only some
    // runtimes know the profile path at all; a rejection here is expected and
    // harmless because the compatibility profile above already covers it.
    ok += SuggestProfile(1, "/interaction_profiles/meta/touch_controller_plus",
                         touch, (int)(sizeof(touch)/sizeof(touch[0])));
    ok += SuggestProfile(2, "/interaction_profiles/khr/simple_controller",
                         simple, (int)(sizeof(simple)/sizeof(simple[0])));
    if (ok)
        BuAdvance(VRI_BU_BINDINGS_SUGGESTED);
    else
        Log("VR input: NO interaction profile accepted bindings -- controllers will be dead");
}

void VRInput_OnInstanceCreated(unsigned long long xrInstance)
{
    XrInstance inst = (XrInstance)xrInstance;
    if (inst == XR_NULL_HANDLE)
        return;
    if (g_actionsLive && inst == g_inst)
        return;                        // already built for this instance
    g_inst = inst;
    g_actionsLive = false;
    g_bindingsSuggested = false;

    g_handPath[VRI_LEFT]  = PathOf("/user/hand/left");
    g_handPath[VRI_RIGHT] = PathOf("/user/hand/right");
    if (g_handPath[VRI_LEFT] == XR_NULL_PATH || g_handPath[VRI_RIGHT] == XR_NULL_PATH) {
        Log("VR input: hand paths unavailable; controller input disabled");
        return;
    }

    XrActionSetCreateInfo si = {};
    si.type = XR_TYPE_ACTION_SET_CREATE_INFO;
    strcpy_s(si.actionSetName, "codvr_gameplay");
    strcpy_s(si.localizedActionSetName, "CoD VR gameplay");
    si.priority = 0;
    XrResult r = xrCreateActionSet(g_inst, &si, &g_set);
    if (XR_FAILED(r)) {
        Log("VR input: xrCreateActionSet failed -> %d %s; controller input disabled",
            (int)r, ResName(r));
        g_set = XR_NULL_HANDLE;
        return;
    }

    bool ok = true;
    ok &= MakeAction(ACT_AIM,           XR_ACTION_TYPE_POSE_INPUT,     "aim_pose",       "Aim pose");
    ok &= MakeAction(ACT_GRIP,          XR_ACTION_TYPE_POSE_INPUT,     "grip_pose",      "Grip pose");
    ok &= MakeAction(ACT_TRIGGER,       XR_ACTION_TYPE_FLOAT_INPUT,    "trigger_value",  "Trigger");
    ok &= MakeAction(ACT_SQUEEZE,       XR_ACTION_TYPE_FLOAT_INPUT,    "squeeze_value",  "Grip squeeze");
    ok &= MakeAction(ACT_STICK,         XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick",     "Thumbstick");
    ok &= MakeAction(ACT_STICK_CLICK,   XR_ACTION_TYPE_BOOLEAN_INPUT,  "thumbstick_click","Thumbstick click");
    ok &= MakeAction(ACT_BTN_PRIMARY,   XR_ACTION_TYPE_BOOLEAN_INPUT,  "btn_primary",    "A / X button");
    ok &= MakeAction(ACT_BTN_SECONDARY, XR_ACTION_TYPE_BOOLEAN_INPUT,  "btn_secondary",  "B / Y button");
    ok &= MakeAction(ACT_MENU,          XR_ACTION_TYPE_BOOLEAN_INPUT,  "menu_click",     "Menu");
    ok &= MakeAction(ACT_HAPTIC,        XR_ACTION_TYPE_VIBRATION_OUTPUT,"haptic",        "Haptic");
    if (!ok) {
        Log("VR input: action creation incomplete; controller input disabled");
        return;
    }
    g_actionsLive = true;
    Log("VR input: action set + %d actions created", ACT_COUNT);
    BuAdvance(VRI_BU_INSTANCE_READY);

    // Reference order (hello_xr/q3vr): suggest bindings the moment the
    // actions exist, before any session -- never after an attach.
    SuggestAllBindings();
}

void VRInput_OnSessionCreated(unsigned long long xrInstance, unsigned long long xrSession)
{
    XrInstance inst = (XrInstance)xrInstance;
    XrSession  sess = (XrSession)xrSession;
    if (sess == XR_NULL_HANDLE)
        return;

    VRInput_OnInstanceCreated(inst);   // idempotent (also suggests bindings)
    if (!g_actionsLive)
        return;
    SuggestAllBindings();              // no-op unless the instance path was skipped

    g_sess = sess;

    // Action spaces are children of THIS session.
    for (int h = 0; h < VRI_HANDS; ++h) {
        XrActionSpaceCreateInfo sci = {};
        sci.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
        sci.action = g_act[ACT_AIM];
        sci.subactionPath = g_handPath[h];
        sci.poseInActionSpace.orientation.w = 1.0f;
        XrResult ra = xrCreateActionSpace(g_sess, &sci, &g_aimSpace[h]);
        if (XR_FAILED(ra)) {
            g_aimSpace[h] = XR_NULL_HANDLE;
            Log("VR input: xrCreateActionSpace(aim %s) failed -> %d %s",
                h == VRI_LEFT ? "left" : "right", (int)ra, ResName(ra));
        }
        sci.action = g_act[ACT_GRIP];
        XrResult rg = xrCreateActionSpace(g_sess, &sci, &g_gripSpace[h]);
        if (XR_FAILED(rg)) {
            g_gripSpace[h] = XR_NULL_HANDLE;
            Log("VR input: xrCreateActionSpace(grip %s) failed -> %d %s",
                h == VRI_LEFT ? "left" : "right", (int)rg, ResName(rg));
        }
    }
    // Proof line for the reference-space audit: no game-world value ever
    // enters an XrPosef on the input side either.
    Log("VR input: aim/grip action spaces created (poseInActionSpace = identity)");

    XrSessionActionSetsAttachInfo ai = {};
    ai.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
    ai.countActionSets = 1;
    ai.actionSets = &g_set;
    XrResult r = xrAttachSessionActionSets(g_sess, &ai);
    g_buAttachTried = true;
    g_buAttachRes = r;
    if (XR_FAILED(r)) {
        Log("VR input: xrAttachSessionActionSets failed -> %d %s; controllers dead this session",
            (int)r, ResName(r));
        g_attached = false;
        return;
    }
    g_attached = true;
    Log("VR input: action set attached to session");
    BuAdvance(VRI_BU_ATTACHED);
}

static void DestroyActionSpaces()
{
    for (int h = 0; h < VRI_HANDS; ++h) {
        if (g_aimSpace[h] != XR_NULL_HANDLE)  { xrDestroySpace(g_aimSpace[h]);  g_aimSpace[h]  = XR_NULL_HANDLE; }
        if (g_gripSpace[h] != XR_NULL_HANDLE) { xrDestroySpace(g_gripSpace[h]); g_gripSpace[h] = XR_NULL_HANDLE; }
    }
}

void VRInput_OnSessionTeardown()
{
    DestroyActionSpaces();
    g_attached = false;
    g_sess = XR_NULL_HANDLE;
    g_state.valid = 0;
    g_state.focused = 0;
    memset(g_state.hand, 0, sizeof(g_state.hand));
    g_state.menuDown = g_state.menuPressed = 0;
    g_state.profile[0][0] = g_state.profile[1][0] = 0;
    // -1, not 0: "unknown until one live sync has been observed". Zero would
    // let a button already held at session start read as a fresh press.
    for (int h = 0; h < VRI_HANDS; ++h)
        g_prevBtnP[h] = g_prevBtnS[h] = g_prevStick[h] = -1;
    g_prevMenu = -1;
    for (int h = 0; h < VRI_HANDS; ++h) {
        g_track[h] = VRI_TRACK_UNAVAILABLE;
        g_trackStreak[h] = 0;
        g_trackLastActiveNs[h] = 0;
    }
    g_prevFocused = -1;
}

void VRInput_OnInstanceTeardown()
{
    VRInput_OnSessionTeardown();
    for (int i = 0; i < ACT_COUNT; ++i) {
        if (g_act[i] != XR_NULL_HANDLE) { xrDestroyAction(g_act[i]); g_act[i] = XR_NULL_HANDLE; }
    }
    if (g_set != XR_NULL_HANDLE) { xrDestroyActionSet(g_set); g_set = XR_NULL_HANDLE; }
    g_actionsLive = false;
    g_bindingsSuggested = false;
    g_inst = XR_NULL_HANDLE;
}

void VRInput_TestReset()
{
    VRInput_OnInstanceTeardown();
    memset(&g_state, 0, sizeof(g_state));
    g_syncFailLogged = 0;
    g_locateFailLogged = 0;
    g_profileLogged = false;
    g_trackLogs = 0;
    g_buStage = VRI_BU_NONE;
    g_runtimeName[0] = 0;
    memset(g_buProf, 0, sizeof(g_buProf));
    g_buAttachRes = XR_SUCCESS;
    g_buAttachTried = false;
    g_buSyncedOnce = false;
    g_buFirstSyncRes = g_buLastSyncRes = XR_SUCCESS;
    g_buSyncCount = 0;
    g_buSyncChangeLogs = 0;
    g_buPoseSeen[0] = g_buPoseSeen[1] = 0;
    g_buReported = false;
    g_focusLogs = 0;
    g_prevFocused = -1;
    memset(g_polledProfile, 0, sizeof(g_polledProfile));
}

// -------------------------------------------------------- profile logging

static void QueryProfile(int hand)
{
    g_state.profile[hand][0] = 0;
    if (!g_attached || g_sess == XR_NULL_HANDLE)
        return;
    XrInteractionProfileState ps = {};
    ps.type = XR_TYPE_INTERACTION_PROFILE_STATE;
    if (XR_FAILED(xrGetCurrentInteractionProfile(g_sess, g_handPath[hand], &ps)))
        return;
    if (ps.interactionProfile == XR_NULL_PATH) {
        strcpy_s(g_state.profile[hand], "(none)");
        return;
    }
    uint32_t n = 0;
    char buf[80] = {};
    if (XR_SUCCEEDED(xrPathToString(g_inst, ps.interactionProfile,
                                    sizeof(buf), &n, buf)))
        strcpy_s(g_state.profile[hand], buf);
}

void VRInput_OnInteractionProfileChanged(unsigned long long xrSession)
{
    (void)xrSession;
    QueryProfile(VRI_LEFT);
    QueryProfile(VRI_RIGHT);
    Log("VR input: interaction profile now left=%s right=%s",
        VRInput_ProfileText(VRI_LEFT), VRInput_ProfileText(VRI_RIGHT));
}

// -------------------------------------------------------- bring-up report

static void LogBringupHand(const char* name, int hand)
{
    const VRHandState& h = g_state.hand[hand];
    static const char* trackName[4] =
        { "UNAVAILABLE", "ACQUIRING", "ACTIVE", "LOST" };
    Log("%s:", name);
    Log("  aim active %d   grip active %d", h.aimValid, h.gripValid);
    Log("  position valid %d   orientation valid %d", h.aimPosTracked, h.aimValid);
    Log("  pos %+.3f %+.3f %+.3f m   quat %+.3f %+.3f %+.3f %+.3f",
        h.aimPos[0], h.aimPos[1], h.aimPos[2],
        h.aimQuat[0], h.aimQuat[1], h.aimQuat[2], h.aimQuat[3]);
    Log("  trigger active %d value %.2f   squeeze active %d value %.2f",
        h.triggerActive, h.trigger, h.squeezeActive, h.squeeze);
    Log("  stick active %d value %+.2f %+.2f", h.stickActive, h.stickX, h.stickY);
    Log("  tracking state %s (raw active %d)", trackName[VRI_TrackCode(g_track[hand]) & 3], h.active);
}

void VRInput_LogBringupReport(const char* reason)
{
    if (g_buReported)
        return;
    g_buReported = true;

    // The report wants CURRENT profiles even if no event ever fired.
    if (g_attached) {
        QueryProfile(VRI_LEFT);
        QueryProfile(VRI_RIGHT);
    }

    const int live = (g_track[VRI_LEFT] == VRI_TRACK_ACTIVE
                   && g_track[VRI_RIGHT] == VRI_TRACK_ACTIVE);

    Log("");
    Log("======= OPENXR CONTROLLER BRING-UP =======");
    Log("(%s)", reason ? reason : "");
    Log("runtime: %s", g_runtimeName[0] ? g_runtimeName : "(unknown)");
    Log("left current profile: %s", VRInput_ProfileText(VRI_LEFT));
    Log("right current profile: %s", VRInput_ProfileText(VRI_RIGHT));
    Log("suggest bindings:");
    for (int i = 0; i < BU_PROFILES; ++i) {
        const BuProfile& p = g_buProf[i];
        if (!p.attempted || !p.path) {
            Log("  (profile %d not attempted)", i);
            continue;
        }
        if (XR_SUCCEEDED(p.result))
            Log("  %s = PASS (%d controls)", p.path, p.controls);
        else
            Log("  %s = REJECTED [%d %s]", p.path, (int)p.result, ResName(p.result));
    }
    if (g_buAttachTried)
        Log("attach action set: %s [%d %s]",
            XR_SUCCEEDED(g_buAttachRes) ? "PASS" : "REJECTED",
            (int)g_buAttachRes, ResName(g_buAttachRes));
    else
        Log("attach action set: never attempted");
    if (g_buSyncedOnce)
        Log("sync: first %d %s, last %d %s, %u syncs, focused now %d",
            (int)g_buFirstSyncRes, ResName(g_buFirstSyncRes),
            (int)g_buLastSyncRes, ResName(g_buLastSyncRes),
            g_buSyncCount, g_state.focused);
    else
        Log("sync: xrSyncActions never ran");
    LogBringupHand("LEFT", VRI_LEFT);
    LogBringupHand("RIGHT", VRI_RIGHT);
    Log("CONTROLLERS_LIVE = %s", live ? "YES" : "NO");

    if (live) {
        Log("FAILURE_STAGE = (none)");
    } else if (g_buStage < VRI_BU_INSTANCE_READY) {
        Log("FAILURE_STAGE = INPUT_INSTANCE_READY (action set/actions never created)");
    } else if (g_buStage < VRI_BU_BINDINGS_SUGGESTED) {
        Log("FAILURE_STAGE = BINDINGS_SUGGESTED (no interaction profile accepted bindings)");
    } else if (g_buStage < VRI_BU_ATTACHED) {
        Log("FAILURE_STAGE = SESSION_ACTIONS_ATTACHED (attach failed or never ran)");
    } else if (g_buStage < VRI_BU_SYNCING) {
        Log("FAILURE_STAGE = ACTIONS_SYNCING (xrSyncActions failing or never called)");
    } else if (g_buStage < VRI_BU_POSE_SEEN) {
        if (!g_state.focused)
            Log("FAILURE_STAGE = POSE_ACTIVE (sync healthy but session never FOCUSED "
                "-- runtime is holding input; check the runtime dashboard)");
        else
            Log("FAILURE_STAGE = POSE_ACTIVE (sync healthy and focused, but no pose "
                "action ever went active -- the runtime never bound our suggested "
                "profiles to the physical controllers, or the controllers are asleep)");
    } else {
        Log("FAILURE_STAGE = CONTROLLERS_LIVE (%s pose seen, %s never stabilised)",
            g_buPoseSeen[VRI_LEFT] && g_buPoseSeen[VRI_RIGHT] ? "both hands"
                : g_buPoseSeen[VRI_LEFT] ? "only the LEFT" : "only the RIGHT",
            g_track[VRI_LEFT] != VRI_TRACK_ACTIVE ? "LEFT" : "RIGHT");
    }
    Log("==========================================");
}

// ------------------------------------------------------------- per frame

static float GetFloat(int act, int hand, int* isActive)
{
    XrActionStateGetInfo gi = {};
    gi.type = XR_TYPE_ACTION_STATE_GET_INFO;
    gi.action = g_act[act];
    gi.subactionPath = g_handPath[hand];
    XrActionStateFloat st = {};
    st.type = XR_TYPE_ACTION_STATE_FLOAT;
    if (XR_FAILED(xrGetActionStateFloat(g_sess, &gi, &st)))
        return 0.0f;
    if (isActive) *isActive = st.isActive ? 1 : 0;
    return st.isActive ? st.currentState : 0.0f;
}

static int GetBool(int act, int hand, int* isActive)
{
    XrActionStateGetInfo gi = {};
    gi.type = XR_TYPE_ACTION_STATE_GET_INFO;
    gi.action = g_act[act];
    gi.subactionPath = g_handPath[hand];
    XrActionStateBoolean st = {};
    st.type = XR_TYPE_ACTION_STATE_BOOLEAN;
    if (XR_FAILED(xrGetActionStateBoolean(g_sess, &gi, &st)))
        return 0;
    if (isActive) *isActive = st.isActive ? 1 : 0;
    return (st.isActive && st.currentState) ? 1 : 0;
}

static void GetVec2(int act, int hand, float* x, float* y, int* isActive)
{
    *x = *y = 0.0f;
    XrActionStateGetInfo gi = {};
    gi.type = XR_TYPE_ACTION_STATE_GET_INFO;
    gi.action = g_act[act];
    gi.subactionPath = g_handPath[hand];
    XrActionStateVector2f st = {};
    st.type = XR_TYPE_ACTION_STATE_VECTOR2F;
    if (XR_FAILED(xrGetActionStateVector2f(g_sess, &gi, &st)))
        return;
    if (st.isActive) {
        if (isActive) *isActive = 1;
        *x = st.currentState.x;
        *y = st.currentState.y;
    }
}

static int LocateHandSpace(XrSpace space, XrSpace base, XrTime t,
                           float pos[3], float quat[4], int* posTracked)
{
    if (space == XR_NULL_HANDLE || base == XR_NULL_HANDLE)
        return 0;
    XrSpaceLocation loc = {};
    loc.type = XR_TYPE_SPACE_LOCATION;
    XrResult r = xrLocateSpace(space, base, t, &loc);
    if (XR_FAILED(r)) {
        if (g_locateFailLogged < 3) {
            ++g_locateFailLogged;
            Log("VR input: xrLocateSpace failed -> %d %s", (int)r, ResName(r));
        }
        return 0;
    }
    if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        return 0;
    quat[0] = loc.pose.orientation.x;
    quat[1] = loc.pose.orientation.y;
    quat[2] = loc.pose.orientation.z;
    quat[3] = loc.pose.orientation.w;
    if (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
        pos[0] = loc.pose.position.x;
        pos[1] = loc.pose.position.y;
        pos[2] = loc.pose.position.z;
        if (posTracked) *posTracked = 1;
    } else if (posTracked) {
        *posTracked = 0;
    }
    return 1;
}

// Trigger/squeeze hysteresis: press at >=0.60, release at <=0.40.
static void Hysteresis(float value, int* down, int* pressed, int* released)
{
    const int was = *down;
    if (!was && value >= 0.60f) *down = 1;
    if (was  && value <= 0.40f) *down = 0;
    *pressed  = (*down && !was) ? 1 : 0;
    *released = (!*down && was) ? 1 : 0;
}

// `gate` is the hand's raw activity. While a hand is inactive its buttons all
// read 0, so `prev` decays to 0 -- and a hand that RE-ACQUIRES with a button
// genuinely held would then present a manufactured rising edge on the first
// live sync. The analog paths are immune (Hysteresis has to climb from 0
// through 0.60), but the boolean ones were not. A hand must be seen holding
// the button DOWN for one sync while active before an edge can be believed,
// which costs one sync on a real press and kills the reacquire artefact.
static void BoolEdge(int value, int gate, int* prev, int* pressed, int* released)
{
    if (!gate) {                       // dead hand: no edges, and no memory
        *pressed = 0;
        if (released) *released = (*prev == 1) ? 1 : 0;
        *prev = -1;                    // -1 = "unknown, needs one live sync"
        return;
    }
    if (*prev < 0) {                   // first live sync after a dropout
        *pressed = 0;
        if (released) *released = 0;
        *prev = value;
        return;
    }
    *pressed  = (value && !*prev) ? 1 : 0;
    if (released)
        *released = (!value && *prev) ? 1 : 0;
    *prev = value;
}

// Advance one hand's tracking state machine from this sync's raw pose
// answer. `stable` (the camera-mode gate) is ACTIVE or LOST-within-grace:
// a one-sync miss keeps the mode; only a sustained silence demotes.
static void TrackAdvance(int hand, int raw, long long nowNs)
{
    static const char* handName[2] = { "LEFT", "RIGHT" };
    static const char* trackName[4] = { "UNAVAILABLE", "ACQUIRING", "ACTIVE", "LOST" };
    const VRITrack before = g_track[hand];
    switch (g_track[hand]) {
    case VRI_TRACK_UNAVAILABLE:
        if (raw) { g_track[hand] = VRI_TRACK_ACQUIRING; g_trackStreak[hand] = 1; }
        break;
    case VRI_TRACK_ACQUIRING:
        if (raw) {
            if (++g_trackStreak[hand] >= VRI_ACQUIRE_SYNCS) {
                g_track[hand] = VRI_TRACK_ACTIVE;
                g_trackLastActiveNs[hand] = nowNs;
            }
        } else {
            g_track[hand] = VRI_TRACK_UNAVAILABLE;
            g_trackStreak[hand] = 0;
        }
        break;
    case VRI_TRACK_ACTIVE:
        if (raw) g_trackLastActiveNs[hand] = nowNs;
        else     g_track[hand] = VRI_TRACK_LOST;
        break;
    case VRI_TRACK_LOST:
        if (raw) {
            g_track[hand] = VRI_TRACK_ACTIVE;
            g_trackLastActiveNs[hand] = nowNs;
        } else if (nowNs - g_trackLastActiveNs[hand] >= TRACK_LOSS_NS) {
            g_track[hand] = VRI_TRACK_UNAVAILABLE;
            g_trackStreak[hand] = 0;
        }
        break;
    }
    if (g_track[hand] != before && g_trackLogs < 24) {
        ++g_trackLogs;
        Log("VR input: %s tracking %s -> %s", handName[hand],
            trackName[VRI_TrackCode(before) & 3], trackName[VRI_TrackCode(g_track[hand]) & 3]);
    }
}

void VRInput_Sync(unsigned long long xrSession, unsigned long long baseSpace, long long displayTime)
{
    XrSession sess = (XrSession)xrSession;
    XrSpace   base = (XrSpace)baseSpace;
    if (!g_actionsLive || !g_attached || sess == XR_NULL_HANDLE || sess != g_sess)
        return;

    XrActiveActionSet active = {};
    active.actionSet = g_set;
    active.subactionPath = XR_NULL_PATH;
    XrActionsSyncInfo si = {};
    si.type = XR_TYPE_ACTIONS_SYNC_INFO;
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    XrResult r = xrSyncActions(g_sess, &si);
    ++g_buSyncCount;
    if (!g_buSyncedOnce) {
        g_buSyncedOnce = true;
        g_buFirstSyncRes = g_buLastSyncRes = r;
        Log("VR input: first xrSyncActions -> %d %s", (int)r, ResName(r));
    } else if (r != g_buLastSyncRes && g_buSyncChangeLogs < 8) {
        ++g_buSyncChangeLogs;
        Log("VR input: xrSyncActions result changed %d %s -> %d %s (sync %u)",
            (int)g_buLastSyncRes, ResName(g_buLastSyncRes), (int)r, ResName(r),
            g_buSyncCount);
    }
    g_buLastSyncRes = r;
    if (XR_FAILED(r)) {
        if (g_syncFailLogged < 3) {
            ++g_syncFailLogged;
            Log("VR input: xrSyncActions failed -> %d %s", (int)r, ResName(r));
        }
        g_state.focused = 0;
        for (int h = 0; h < VRI_HANDS; ++h) {
            g_state.hand[h].active = 0;
            TrackAdvance(h, 0, displayTime);
            g_state.hand[h].trackState = g_track[h];
        }
        return;
    }
    BuAdvance(VRI_BU_SYNCING);
    g_state.valid = 1;
    // XR_SESSION_NOT_FOCUSED is a success code: the loop is healthy but the
    // runtime keeps the input (dashboard open). Everything reads inactive.
    g_state.focused = (r == XR_SESSION_NOT_FOCUSED) ? 0 : 1;
    if (g_prevFocused != g_state.focused && g_focusLogs < 6) {
        ++g_focusLogs;
        Log("VR input: session focus -> %d (sync %u)", g_state.focused, g_buSyncCount);
    }
    g_prevFocused = g_state.focused;
    g_state.sampleTime = (long long)displayTime;

    for (int h = 0; h < VRI_HANDS; ++h) {
        VRHandState& hs = g_state.hand[h];
        int act = 0;

        // Pose activity: the canonical "is this controller alive" signal.
        {
            XrActionStateGetInfo gi = {};
            gi.type = XR_TYPE_ACTION_STATE_GET_INFO;
            gi.action = g_act[ACT_AIM];
            gi.subactionPath = g_handPath[h];
            XrActionStatePose ps = {};
            ps.type = XR_TYPE_ACTION_STATE_POSE;
            if (XR_SUCCEEDED(xrGetActionStatePose(g_sess, &gi, &ps)) && ps.isActive)
                act = 1;
        }
        hs.active = g_state.focused ? act : 0;
        if (hs.active) {
            if (!g_buPoseSeen[h]) {
                g_buPoseSeen[h] = 1;
                Log("VR input: %s pose action ACTIVE for the first time (sync %u)",
                    h == VRI_LEFT ? "LEFT" : "RIGHT", g_buSyncCount);
            }
            BuAdvance(VRI_BU_POSE_SEEN);
        }

        hs.aimValid  = hs.active && LocateHandSpace(g_aimSpace[h], base,
                                        (XrTime)displayTime, hs.aimPos, hs.aimQuat,
                                        &hs.aimPosTracked);
        if (!hs.aimValid)
            hs.aimPosTracked = 0;
        hs.gripValid = hs.active && LocateHandSpace(g_gripSpace[h], base,
                                        (XrTime)displayTime, hs.gripPos, hs.gripQuat,
                                        nullptr);

        hs.triggerActive = hs.squeezeActive = hs.stickActive = 0;
        hs.trigger = hs.active ? GetFloat(ACT_TRIGGER, h, &hs.triggerActive) : 0.0f;
        hs.squeeze = hs.active ? GetFloat(ACT_SQUEEZE, h, &hs.squeezeActive) : 0.0f;
        if (hs.active) GetVec2(ACT_STICK, h, &hs.stickX, &hs.stickY, &hs.stickActive);
        else           hs.stickX = hs.stickY = 0.0f;
        int dummy = 0;
        hs.btnPrimary   = hs.active ? GetBool(ACT_BTN_PRIMARY, h, &dummy) : 0;
        hs.btnSecondary = hs.active ? GetBool(ACT_BTN_SECONDARY, h, &dummy) : 0;
        hs.stickClick   = hs.active ? GetBool(ACT_STICK_CLICK, h, &dummy) : 0;

        Hysteresis(hs.trigger, &hs.triggerDown, &hs.triggerPressed, &hs.triggerReleased);
        Hysteresis(hs.squeeze, &hs.squeezeDown, &hs.squeezePressed, &hs.squeezeReleased);
        BoolEdge(hs.btnPrimary,   hs.active, &g_prevBtnP[h], &hs.btnPrimaryPressed,   &hs.btnPrimaryReleased);
        BoolEdge(hs.btnSecondary, hs.active, &g_prevBtnS[h], &hs.btnSecondaryPressed, &hs.btnSecondaryReleased);
        int rel = 0;
        BoolEdge(hs.stickClick, hs.active, &g_prevStick[h], &hs.stickClickPressed, &rel);

        // Tracking state: raw = pose active AND located this sync.
        TrackAdvance(h, hs.active && hs.aimValid, displayTime);
        hs.trackState = g_track[h];
    }

    g_state.menuDown = GetBool(ACT_MENU, VRI_LEFT, nullptr)
                     | GetBool(ACT_MENU, VRI_RIGHT, nullptr);
    int menuRel = 0;
    BoolEdge(g_state.menuDown, g_state.focused, &g_prevMenu, &g_state.menuPressed, &menuRel);

    if (g_state.focused)
        ++g_state.generation;

    // First good sync with a live controller: report the active profile.
    if (!g_profileLogged && (g_state.hand[0].active || g_state.hand[1].active)) {
        g_profileLogged = true;
        QueryProfile(VRI_LEFT);
        QueryProfile(VRI_RIGHT);
        Log("VR input: controllers live -- profile left=%s right=%s",
            VRInput_ProfileText(VRI_LEFT), VRInput_ProfileText(VRI_RIGHT));
    }

    // Some runtimes never deliver INTERACTION_PROFILE_CHANGED. Poll while the
    // controllers are not both stable, every ~2 s of syncs, and log changes.
    if ((g_track[VRI_LEFT] != VRI_TRACK_ACTIVE || g_track[VRI_RIGHT] != VRI_TRACK_ACTIVE)
        && (g_buSyncCount % 240) == 0) {
        QueryProfile(VRI_LEFT);
        QueryProfile(VRI_RIGHT);
        if (strcmp(g_polledProfile[0], g_state.profile[0]) != 0
            || strcmp(g_polledProfile[1], g_state.profile[1]) != 0) {
            strcpy_s(g_polledProfile[0], g_state.profile[0]);
            strcpy_s(g_polledProfile[1], g_state.profile[1]);
            Log("VR input: polled interaction profile left=%s right=%s (sync %u)",
                VRInput_ProfileText(VRI_LEFT), VRInput_ProfileText(VRI_RIGHT),
                g_buSyncCount);
        }
    }

    // The one-shot bring-up verdict: immediately on first CONTROLLERS_LIVE,
    // or after ~900 syncs (about 7.5 s at 120 Hz) still without one.
    if (g_track[VRI_LEFT] == VRI_TRACK_ACTIVE && g_track[VRI_RIGHT] == VRI_TRACK_ACTIVE) {
        BuAdvance(VRI_BU_LIVE);
        VRInput_LogBringupReport("both controllers reached stable tracking");
    } else if (g_buSyncCount == 900) {
        VRInput_LogBringupReport("900 syncs without stable controllers");
    }
}

void VRInput_Haptic(int hand, float amplitude01, int durationMs)
{
    if (hand < 0 || hand >= VRI_HANDS || !g_attached || g_sess == XR_NULL_HANDLE)
        return;
    if (amplitude01 <= 0.0f)
        return;
    if (amplitude01 > 1.0f) amplitude01 = 1.0f;
    if (durationMs < 1)   durationMs = 1;
    if (durationMs > 500) durationMs = 500;

    XrHapticVibration vib = {};
    vib.type = XR_TYPE_HAPTIC_VIBRATION;
    vib.amplitude = amplitude01;
    vib.duration = (XrDuration)durationMs * 1000000LL;
    vib.frequency = XR_FREQUENCY_UNSPECIFIED;

    XrHapticActionInfo hi = {};
    hi.type = XR_TYPE_HAPTIC_ACTION_INFO;
    hi.action = g_act[ACT_HAPTIC];
    hi.subactionPath = g_handPath[hand];
    xrApplyHapticFeedback(g_sess, &hi, (const XrHapticBaseHeader*)&vib);
}
