// ---------------------------------------------------------------------------
// PHYSICAL RELOAD
// ---------------------------------------------------------------------------
//
// The support hand performs a deliberate reload gesture at measured loading
// zones. Detachable magazines use grab/pull/pouch/insert; clips and receiver
// loaders use pouch/insert. Completion asks the stock engine to run its normal
// reload, including ammunition transfer and charging/bolt animation. This
// layer does not simulate a detached magazine or per-round ammunition.
//
// WHAT IS MEASURED AND WHAT IS CHOSEN
//
// Measured, from the weapon's own reload animation (tools/extract_reload.py):
//
//   which hand does the loading   -- the support hand fetches a magazine; the
//                                    trigger hand works a bolt and presses a
//                                    clip in. Getting this backwards would put
//                                    the gesture on the wrong controller, so it
//                                    is read off the animation rather than
//                                    assumed from the weapon's class.
//   the loading zone              -- where that hand travels to, in the
//                                    WEAPON's frame. A Bren's magwell is on
//                                    top; a Thompson's is underneath. A single
//                                    "below the gun" rule would be wrong for
//                                    both.
//   the firing grip               -- where the hand sits when not reloading.
//   how the motion is structured  -- one swap, or a per-round loop.
//
// Chosen, and marked as such because nothing in the assets answers it:
//
//   the ammo pouch zone           -- where a player reaches for a fresh
//                                    magazine. This is a fact about human
//                                    beings and a comfortable play space, not
//                                    about CoD1, so it is a tunable with a
//                                    stated default rather than a constant
//                                    pretending to be data.
//   the zone radii and the pull threshold.
//
// SAFETY POSTURE
//
// The machine issues AT MOST ONE reload command per completed gesture, and the
// command is edge-triggered off a state transition that happens once. Every
// way a gesture can be interrupted -- tracking loss, the menu, a weapon
// switch, taking too long -- routes to RECOVER, which issues nothing. A reload
// that fires twice would cost the player a magazine; one that fires when they
// did not ask would cost them a firefight.

#pragma once

// The four motion shapes the extractor distinguishes. Kept numerically in step
// with tools/reload_classify.py.
enum {
    VRRL_CLASS_NONE = 0,
    VRRL_CLASS_MAGAZINE,      // magazine or clip swapped whole, one motion
    VRRL_CLASS_INCREMENTAL,   // one round at a time, start/loop/end
    VRRL_CLASS_SINGLE_SHOT,   // one projectile; the weapon is the round
    VRRL_CLASS_COUNT
};

// Which hand performs the loading. MEASURED per weapon.
enum { VRRL_HAND_SUPPORT = 0, VRRL_HAND_TRIGGER = 1 };

// The gesture. Ordered so that > VRRL_IDLE means "a gesture is in progress".
enum {
    VRRL_IDLE = 0,
    VRRL_REACH,        // the loading hand has left its firing grip
    VRRL_GRIPPED,      // it is at the loading zone with the grip held
    VRRL_PULLED,       // it has moved away far enough: the magazine is out
    VRRL_POUCH,        // it has reached the ammunition
    VRRL_ACQUIRED,     // a fresh magazine is in hand
    VRRL_INSERT,       // it is back at the loading zone
    VRRL_COMMIT,       // the single engine reload is issued HERE
    VRRL_RECOVER,      // returning to the firing grip; issues nothing
    VRRL_STATES
};

const char* VRReload_StateName(int state);
const char* VRReload_ClassName(int cls);

// The per-weapon geometry, from the measured table. All positions are in the
// WEAPON's frame, metres.
struct VRReloadProfile {
    int   cls;                 // VRRL_CLASS_*
    int   hand;                // VRRL_HAND_*
    float firingGripM[3];      // where the loading hand rests
    float loadZoneM[3];        // magwell / receiver / loading point
    int   valid;               // 0 => no physical reload for this weapon
    int   directLoad;          // clip/receiver: no detachable magazine to remove
};

// The tunables. Defaults are stated, not hidden: these are choices about a
// play space, and the day a hardware session says otherwise they should move.
struct VRReloadTuning {
    float zoneRadiusM;         // how close counts as "at" a zone   (0.12)
    float pullThresholdM;      // how far out the magazine must come (0.15)
    float pouchBelowHeadM;     // the ammo pouch, relative to the head (0.45)
    float pouchForwardM;       //                                      (0.15)
    float pouchRadiusM;        //                                      (0.22)
    float leaveGripM;          // how far from the firing grip starts a reach (0.10)
    float timeoutSec;          // a gesture that stalls is abandoned  (8.0)
};
void VRReload_GetTuning(VRReloadTuning* out);
void VRReload_SetTuning(const VRReloadTuning* t);

// What the machine was told this frame.
struct VRReloadInput {
    int   valid;               // controller tracking is good
    float handM[3];            // the LOADING hand, in the weapon's frame
    float handWorldM[3];       // the same hand, relative to the head
    int   gripHeld;            // the grip button on that controller
    int   menuOpen;
    int   weaponChanged;       // a different weapon is in hand than last frame
    double now;                // seconds
};

// What the machine decided.
struct VRReloadOutput {
    int state;
    int issueReload;           // 1 on exactly one frame per completed gesture
    int cancelled;             // 1 on the frame a gesture was abandoned
};

void VRReload_Reset();
void VRReload_SetProfile(const VRReloadProfile* p);

// Select the profile for the gun model currently being drawn, identified by
// its batch's vertex count -- the same exact key the gun landmarks use (see
// vr_handpose.h). Returns 1 when a live gesture profile was installed.
//
// A vertex count with no row, or a row for a weapon that has no hand reload,
// installs an INVALID profile: the machine then makes no gesture at all,
// which is the right answer for a grenade and for a mod this build has never
// seen. It is never a reason to fall back to a generic magwell.
int VRReload_SelectByVertexCount(int vertexCount);
int VRReload_TableRows();
void VRReload_Get(VRReloadProfile* out);

// Advance one frame. Returns the output by value so a caller cannot forget to
// read `issueReload` and then wonder why nothing reloaded.
VRReloadOutput VRReload_Step(const VRReloadInput* in);

int VRReload_State();
int VRReload_ReloadsIssued();
int VRReload_Cancellations();

// True while the loading hand is doing something other than holding the
// weapon -- the renderer uses this to stop treating it as the support grip.
int VRReload_HandBusy();
