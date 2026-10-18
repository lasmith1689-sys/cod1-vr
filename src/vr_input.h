// OpenXR controller input: action set, actions, suggested bindings, action
// spaces, per-XR-frame sync/locate, and haptic output.
//
// This header is deliberately OpenXR-free so vr_weapon/vr_binds (pure logic
// and Windows-injection layers) can consume controller state without pulling
// in OpenXR types. The lifecycle entry points take opaque handles; vr.cpp --
// the only file that owns real Xr handles -- casts them across.
//
// Cadence contract: VRInput_Sync runs ONCE per located XR frame, at the pair
// boundary, right where vr.cpp locates the views. Both eyes of an F1 pair
// therefore render against ONE input generation -- controller and head data
// always belong to the same predicted display time, so a pair can never be
// stereo-inconsistent about where a hand is.
#pragma once

enum { VRI_LEFT = 0, VRI_RIGHT = 1, VRI_HANDS = 2 };

// Per-hand tracking state machine. `active` (below) is the runtime's RAW
// isActive answer for one sync and may flicker; consumers that switch camera
// composition must use the hysteresis outputs instead:
//   UNAVAILABLE  never/no longer tracking
//   ACQUIRING    active, but not yet for VRI_ACQUIRE_SYNCS consecutive syncs
//   ACTIVE       stably tracking
//   LOST         was ACTIVE, missing for < 1 s (poses stale but mode holds)
// A one-sync dropout is ACTIVE -> LOST -> ACTIVE and the hand stays USABLE
// throughout; only a full second of silence demotes to UNAVAILABLE.
//
// THIS IS A DISTINCT TYPE, NOT AN int, ON PURPOSE. See the encoding-trap note
// under VRHandState: an `enum class` cannot be compared to a plain int without
// an explicit cast, so the bug that made the controllers unreachable on
// 2026-08-28 is now a COMPILE error instead of an expression that reads like
// English and is always false.
enum class VRITrack : int {
    Unavailable = 0,
    Acquiring   = 1,
    Active      = 2,
    Lost        = 3
};
// Readable names for the comparison sites. These are VRITrack VALUES, so an
// `someInt == VRI_TRACK_ACTIVE` no longer compiles anywhere in the tree.
constexpr VRITrack VRI_TRACK_UNAVAILABLE = VRITrack::Unavailable;
constexpr VRITrack VRI_TRACK_ACQUIRING   = VRITrack::Acquiring;
constexpr VRITrack VRI_TRACK_ACTIVE      = VRITrack::Active;
constexpr VRITrack VRI_TRACK_LOST        = VRITrack::Lost;

enum { VRI_ACQUIRE_SYNCS = 8 };   // consecutive active syncs before ACTIVE

struct VRHandState {
    int      active;       // RAW: pose action bound and reporting this sync
    // THE SINGLE SOURCE OF TRUTH for tracking. There is deliberately no
    // second boolean beside it: the old `stable` field was exactly
    // (trackState == ACTIVE || trackState == LOST), so it carried no
    // information and existed only to be confused with this one.
    VRITrack trackState;
    int   aimValid;        // aim space located with valid orientation
    int   gripValid;
    int   aimPosTracked;   // locate reported POSITION_VALID for the aim space
    float aimPos[3];       // XR LOCAL space, metres
    float aimQuat[4];      // x,y,z,w
    float gripPos[3];
    float gripQuat[4];
    float trigger;         // 0..1
    float squeeze;         // 0..1
    float stickX, stickY;  // -1..1
    int   triggerActive;   // isActive of the float/vector actions this sync
    int   squeezeActive;
    int   stickActive;
    int   btnPrimary;      // A (right) / X (left)
    int   btnSecondary;    // B (right) / Y (left)
    int   stickClick;
    // Edges computed across syncs. Trigger uses hysteresis (>=0.60 press,
    // <=0.40 release) so an analog value resting on a single threshold can
    // never machine-gun press events.
    int   triggerDown;         // hysteresis state, level
    int   triggerPressed;      // edge this sync
    int   triggerReleased;     // edge this sync
    int   squeezeDown;         // same hysteresis, for the support/ADS grip
    int   squeezePressed;
    int   squeezeReleased;
    int   btnPrimaryPressed, btnPrimaryReleased;
    int   btnSecondaryPressed, btnSecondaryReleased;
    int   stickClickPressed;
};

// ------------------------------------------------ reading a hand's tracking
//
// THE ENCODING TRAP THIS STRUCTURE NOW MAKES UNREPRESENTABLE.
//
// VRHandState used to carry BOTH a boolean `stable` (0 or 1) and an int
// `trackState` holding the VRI_TRACK_* enum (0..3). Both were plain ints, so
//
//     if (h.stable == VRI_TRACK_ACTIVE)          // 1 == 2
//
// compiled, read like English, and was ALWAYS FALSE. That exact expression
// was the engagement gate in vr_native_hands.cpp, and it made the
// controller-driven hands unreachable on hardware: the 2026-08-28 run logged
// "R{eng=0 TRACKING} L{eng=0 TRACKING}" on every one of ~7000 pairs while
// both controllers were tracking perfectly (trk 1, poses valid, live). The
// offline fixture hand-built its VRHandState and set stable =
// VRI_TRACK_ACTIVE, so 1400+ green tests agreed with the bug.
//
// TWO structural changes close it, rather than a wrapper around the hazard:
//
//   1. `stable` is GONE. It was exactly (ACTIVE || LOST) -- derivable, never
//      independently set, and its only unique contribution was the collision.
//      A fixture can no longer construct the illegal state because the field
//      it was stored in does not exist.
//   2. `trackState` is a VRITrack `enum class`, so comparing ANY int field of
//      this struct to a VRI_TRACK_* name is ill-formed, not merely wrong.
//
// Consumers ask these questions by name; nothing outside vr_input.cpp should
// touch trackState directly.
static inline int VRI_HandTrackingActive(const VRHandState& h)
{ return h.trackState == VRI_TRACK_ACTIVE ? 1 : 0; }
static inline int VRI_HandTrackingLost(const VRHandState& h)
{ return h.trackState == VRI_TRACK_LOST ? 1 : 0; }
// ACTIVE, or LOST inside the 1 s hysteresis grace: the "keep the mode" gate.
// This is what the deleted `stable` field meant, computed where it is asked
// for instead of stored next to the enum it was confused with.
static inline int VRI_HandTrackingUsable(const VRHandState& h)
{
    return (h.trackState == VRI_TRACK_ACTIVE ||
            h.trackState == VRI_TRACK_LOST) ? 1 : 0;
}
// Numeric code, for logging only. Named so that a %d in a log line is an
// explicit request for the code rather than an accidental int comparison.
static inline int VRI_TrackCode(const VRHandState& h) { return (int)h.trackState; }
static inline int VRI_TrackCode(VRITrack t)           { return (int)t; }

struct VRInputState {
    int         valid;         // actions attached and synced at least once
    int         focused;       // last xrSyncActions ran with session focus
    unsigned    generation;    // bumps once per successful focused sync
    long long   sampleTime;    // XrTime the poses were located for
    VRHandState hand[VRI_HANDS];
    int         menuDown, menuPressed;
    char        profile[VRI_HANDS][80];   // active interaction profile paths
    int         suggestedOk;   // interaction profiles that accepted bindings
    int         suggestedFail; // profiles the runtime rejected (logged)
};

// ------------------------------------------------------------- lifecycle
// All opaque-handle parameters are the live XrInstance/XrSession/XrSpace.
// Every call is idempotent and safe out of order; failures are logged and
// leave the input layer inert (the game keeps running, menus keep working).
//
// Binding suggestions happen at INSTANCE creation (the reference hello_xr /
// q3vr order: create actions, suggest, then sessions come and go), never
// after an attach.

void VRInput_OnInstanceCreated(unsigned long long xrInstance); // set+actions+suggestions once
void VRInput_OnSessionCreated(unsigned long long xrInstance,
                              unsigned long long xrSession); // spaces+attach
void VRInput_OnSessionTeardown();                   // action spaces die with the session
void VRInput_OnInstanceTeardown();                  // actions + set die with the instance
void VRInput_OnInteractionProfileChanged(unsigned long long xrSession);

// The runtime's name, for the bring-up report. Safe to call any time.
void VRInput_SetRuntimeName(const char* name);

// Once per located XR frame (pair boundary). baseSpace is the LOCAL space the
// views are located in, displayTime the frame's predicted display time.
void VRInput_Sync(unsigned long long xrSession, unsigned long long baseSpace,
                  long long displayTime);

// Fire-and-forget haptic pulse. Bounded by the caller; this only submits.
void VRInput_Haptic(int hand, float amplitude01, int durationMs);

const VRInputState* VRInput_State();

// ---------------------------------------------- bring-up state machine
// The canonical ladder. The highest stage reached is latched and every
// advance is logged, so a dead-controller hardware run pinpoints exactly
// where activation stopped instead of going silent.
enum {
    VRI_BU_NONE = 0,            // nothing built
    VRI_BU_INSTANCE_READY,      // action set + actions created
    VRI_BU_BINDINGS_SUGGESTED,  // >= 1 interaction profile accepted bindings
    VRI_BU_ATTACHED,            // xrAttachSessionActionSets succeeded
    VRI_BU_SYNCING,             // xrSyncActions returning success codes
    VRI_BU_POSE_SEEN,           // >= 1 hand pose action reported active
    VRI_BU_LIVE                 // both hands reached stable ACTIVE tracking
};
int  VRInput_BringupStage();               // highest VRI_BU_* reached
VRITrack VRInput_HandTracking(int hand);   // this hand's state right now
// Emit the one-shot "OPENXR CONTROLLER BRING-UP" block immediately (it
// otherwise emits on first CONTROLLERS_LIVE, or after ~900 syncs without).
void VRInput_LogBringupReport(const char* reason);

// Diagnostics for the one-shot VR INPUT / WEAPON CHECK block.
const char* VRInput_ProfileText(int hand);
int  VRInput_ActionsLive();       // set+actions created
int  VRInput_Attached();          // attached to the current session

// Testing seam: reset every static (handles, latched states, log latches) so
// scenario tests start cold. Safe to call with no instance.
void VRInput_TestReset();
